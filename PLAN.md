# SyncTool v2 设计与实施记录

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
- 目标不存在 -> 新增；类型冲突或严格更旧 -> 替换；时间相同 -> 跳过。

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
- 执行：固定线程池，原子下标领取操作；操作之间按单元互不重叠。
- manifest 写入用互斥锁；错误统一收集。
- zip 使用 `mz_zip_writer_init_file` + `mz_zip_writer_add_file` 流式写盘，内存不随目录增长。

### 3.6 平台分支
| 能力 | Windows | Linux / macOS |
|---|---|---|
| 控制台 UTF-8 | `SetConsoleOutputCP` | 默认 UTF-8 |
| 时间戳 | `localtime_s` | `localtime_r` |
| 隐藏属性 | `SetFileAttributesW` | 以 `.` 开头即隐藏 |
| 临时文件锁 | `CreateFileW` share=0 | `open` + `flock` |
| 根目录占用 | `CreateFileW` 共享读/独占写 + `FILE_FLAG_BACKUP_SEMANTICS` | `open(O_DIRECTORY)` + `flock` |
| Ctrl+C | `SetConsoleCtrlHandler` | `sigaction` |

Windows 没有内核级强制目录锁，根目录句柄只能阻止重命名/删除/资源管理器进入；子文件仍可能被写入，因此文档中明确提示"同步期间不要修改"。

### 3.7 中断
- 全局原子取消标志；文件循环、zip 循环、操作循环都检查。
- 当前文件写完后停止；已备份的单元可还原；临时计划与目录句柄在析构中释放；退出码 130。

## 四、命令

```
SyncTool sync <文件夹...> [--min-level N] [--jobs N] [--exclude 模式]... [--dry-run] [--force]
SyncTool restore <文件夹> [--time 时间戳] [--dry-run]
SyncTool list <文件夹>
SyncTool help | --version
```

退出码：0 成功，1 有错误，2 空间不足，130 中断。

## 五、测试

在独立沙箱验证：

- 文件单元：新旧版本、只在一侧存在、时间相同跳过；
- 目录单元：`--min-level 2` 整体替换、zip 备份、manifest、空目录；
- 还原：文件先删后复、目录整树删除后解压、mtime 恢复；
- `.backup` 隐藏属性、v1 `backup` 迁移；
- 中文路径、深层目录；
- 预检空间不足退出、`--force`；
- 计划文件正常/中断清理；
- Windows 根目录占用不阻塞自身写入；
- 进度输出（TTY 单行刷新 / 重定向 5 秒一行）。

## 六、发布

- `build.bat`（MSVC / MinGW）、`build.sh`（Linux / macOS，`STATIC=1` 全静态）。
- GitHub Actions：Windows / Linux / macOS x64 / macOS arm64 四份构建；`v*` 标签自动 Release。
- README 双语，MIT License，Release 资产命名与用户既有仓库一致。
