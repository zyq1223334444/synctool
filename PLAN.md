# SyncTool v2.1 设计与实施记录

## 一、目标

在 v1（对等同步、`--min-level`、目录 zip 备份、按时间点还原）的基础上，完成用户提出的第二轮改造：

1. 备份目录改为 `.backup`，Windows 下隐藏，扫描时排除；
2. 同一最小单元修改时间相同则不复制；
3. 同步前先完整检测所有文件夹，确认要复制/备份/删除什么，把操作写入用户临时目录的 JSON 计划文件，并用独占句柄占用，防清理、防并发；结束/中断时清除；
4. 显示总进度、当前操作、分进度；
5. 同步时提示不要修改被同步目录；Windows 下占用根文件夹；
6. 优化算法提升速度；
7. 增加 Linux / macOS 单文件构建；
8. 按用户 GitHub 仓库风格发布。

v2.1 追加（2026-10-31）：

9. Ctrl+C 询问保留备份（下次继续）或回滚本次更改；
10. 时间相同先比对哈希，不同则排队询问源/跳过，询问期间其他线程继续复制；
11. 新增清理备份命令，时间点用序号选择；
12. Windows 长路径未开启时询问并自动开启（必要时提权 + 重启自身）。

## 二、总体流程

```
参数解析 -> 校验文件夹 -> 迁移 v1 backup -> 占用根目录
   -> 并行扫描（生成单元表）
   -> 预检（选胜者、生成操作计划、按卷空间检查、写临时 JSON）
   -> 执行（多线程：备份 -> 删除 -> 复制）
   -> 写 manifest -> 释放锁/删除计划 -> 汇总
```

## 三、关键设计

### 3.1 单元与胜者
- `--min-level N`：深度 `N+1` 的目录是目录单元，单元时间 = 子树内文件 mtime 的最大值；否则文件单元。
- 胜者：时间最大；时间相同目录优先、参数靠前的文件夹优先。
- 目标不存在 -> 新增；类型冲突或严格更旧 -> 替换；时间相同 -> 先比对内容哈希：相同跳过，不同进入冲突队列，逐个询问源/跳过。

### 3.2 备份
- 根级 `.backup/<时间戳>/`；Windows `SetFileAttributes` 设为隐藏。
- 文件按相对路径复制；目录流式压缩为 `<相对路径>.zip`（miniz level=1），写入 `manifest.txt`。
- 备份失败 -> 跳过该单元，不删除旧版本。
- v1 的根级 `backup` 自动改名为 `.backup`（`.backup` 已存在则不动）。

### 3.3 临时计划
- 路径：系统临时目录 `SyncTool/plan-<fnv1a(文件夹集合)>.json`。
- Windows `CreateFileW(..., share=0, CREATE_ALWAYS)`；POSIX `open` + `flock`。
- 内容：工具版本、时间、`minLevel`、`jobs`、文件夹列表、汇总、逐操作记录（key/target/action/winner/字节数）。
- 独占句柄保证临时清理器删不掉、同一组文件夹不能并发同步；析构时关闭并删除。

### 3.4 空间检查
- 每卷需求 = 复制字节 + 备份字节 × 85%（zip 压缩保守估算）+ 1GB 余量。
- 空间不足默认退出码 2；`--force` 可跳过预检。
- 运行中每 128/256 个文件检查一次可用空间，低于 512MB 设置取消标志，安全停止。

### 3.5 并发
- 扫描：每个文件夹一个线程。
- 哈希：线程池对同时间单元的各文件夹版本并行计算 128 位内容哈希；结果缓存到系统临时目录。
- 执行：固定线程池，原子下标领取普通操作；同时间冲突由独立解析线程排队询问，普通线程继续复制。
- manifest 写入用互斥锁；journal 追加写入用互斥锁；错误统一收集。
- zip 使用 `mz_zip_writer_init_file` + `mz_zip_writer_add_file` 流式写盘，内存不随目录增长。

### 3.6 同时间冲突
- 扫描后收集所有“时间相同、类型相同”的单元，计算内容哈希；哈希一致才计入跳过。
- 哈希不同则生成冲突：列出每个文件夹序号、大小、哈希前缀；输入序号选源，`0` 跳过。
- 选源后只复制到哈希不同的目标，复制走正常备份 + journal 流程。
- `--yes` 全部跳过；非交互 EOF 默认跳过，避免卡住。

### 3.7 中断与回滚
- journal 路径 `<root>/.backup/<本次时间>/journal.jsonl`，每单元记录 `seq/key/action/state/backup`。
- 状态：`started` -> `backedUp` -> `done`；备份失败 `failed`；复制失败保持 `backedUp`（旧版本可恢复）。
- Ctrl+C 后询问保留备份（默认）以便下次继续；不保留则询问回滚。
- 回滚按 seq 取每单元最终状态，逆序处理：新增删除，替换从文件备份或目录 zip 恢复；回滚期间临时清除取消标志。

### 3.8 清理备份
- `clean <文件夹>` 列出时间点及大小，序号从 1 开始，支持 `1,3` 多选、`0` 全部。
- `--time` 指定时间点，`--all` 全部，`--dry-run` 预览，`--yes` 跳过确认。
- 只删除 `.backup` 下的时间点目录，删除前二次确认。

### 3.9 Windows 长路径
- `res/app.manifest` 内嵌 `longPathAware=true`；MSVC 用 `rc` 编译资源，MinGW 用 `windres`。
- 启动同步时若 `LongPathsEnabled` 未开启，询问是否自动开启（`--yes` 默认开启，`--no-long-paths` 跳过）。
- 管理员直接写 `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled=1`；非管理员通过 `ShellExecuteExW("runas")` 提权执行 `--enable-long-paths`。
- 写入后重启自身并附加 `--long-paths-enabled`，让新进程加载 manifest；父进程等待并返回子进程退出码。

### 3.10 平台分支
| 能力 | Windows | Linux / macOS |
|---|---|---|
| 控制台 UTF-8 | `SetConsoleOutputCP` | 默认 UTF-8 |
| 时间戳 | `localtime_s` | `localtime_r` |
| 隐藏属性 | `SetFileAttributesW` | 以 `.` 开头即隐藏 |
| 临时文件锁 | `CreateFileW` share=0 | `open` + `flock` |
| 根目录占用 | `CreateFileW` 共享读/独占写 + `FILE_FLAG_BACKUP_SEMANTICS` | `open(O_DIRECTORY)` + `flock` |
| Ctrl+C | `SetConsoleCtrlHandler` | `sigaction` |

Windows 没有内核级强制目录锁，根目录句柄只能阻止重命名/删除/资源管理器进入；子文件仍可能被写入，因此文档中明确提示"同步期间不要修改"。

## 四、命令

```
SyncTool sync <文件夹...> [--min-level N] [--jobs N] [--exclude 模式]... [--dry-run] [--force] [--yes] [--no-long-paths]
SyncTool restore <文件夹> [--time 时间戳] [--dry-run]
SyncTool list <文件夹>
SyncTool clean <文件夹> [--time 时间戳|--all] [--dry-run] [--yes]
SyncTool help | --version
```

退出码：0 成功，1 有错误，2 空间不足，130 中断。

## 五、测试

在独立沙箱验证：

- 文件单元：新旧版本、只在一侧存在、时间相同且哈希相同跳过；
- 同时间哈希冲突：选源、跳过、多个冲突排队、`--yes` 跳过、正常复制不被阻塞；
- 目录单元：`--min-level 0/1` 整体替换、zip 备份、manifest、空目录、目录哈希冲突；
- 中断：保留继续、回滚文件替换、回滚新增、回滚目录 zip、无保留删除备份；
- 清理：序号多选、`--time`、`--all`、`--dry-run`、二次确认；
- Windows 长路径：`LongPathsEnabled=0` 时自动开启、重启、>260 字符文件同步；
- 还原：文件先删后复、目录整树删除后解压、mtime 恢复、序号选择；
- `.backup` 隐藏属性、v1 `backup` 迁移；
- 中文路径、深层目录；
- 预检空间不足退出、`--force`；
- 计划文件正常/中断清理；
- Windows 根目录占用不阻塞自身写入；
- 进度输出（TTY 单行刷新 / 重定向 5 秒一行）；
- Linux 静态构建冒烟。

## 六、发布

- `build.bat`（MSVC / MinGW）、`build.sh`（Linux / macOS，`STATIC=1` 全静态）。
- GitHub Actions：Windows / Linux / macOS x64 / macOS arm64 四份构建；`v*` 标签自动 Release。
- README 双语，MIT License，Release 资产命名与用户既有仓库一致。
