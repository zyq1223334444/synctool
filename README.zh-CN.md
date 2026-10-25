[🇬🇧 English](README.md) · [⬇ 下载](https://github.com/zyq1223334444/synctool/releases) · [更新日志](#更新日志)

# SyncTool

**把多个文件夹当成同一份数据来用：最新者胜出，旧版本进 `.backup`，随时按时间点还原。**

一个 Windows / Linux / macOS 的单文件命令行工具，C++17 编写，不依赖任何运行库或外部压缩软件。给它几个文件夹，它让它们变成同一份内容；给每个被替换的版本留一份可还原的备份。

| | |
|---|---|
| **同步** | `SyncTool sync A B C --min-level 1` —— 三份文件夹互相同步，谁的版本新就用谁的 |
| **备份** | 被替换的旧文件/旧目录自动进入各文件夹自己的 `.backup\<时间>\`，Windows 下自动隐藏 |
| **还原** | `SyncTool restore A --time 2026-10-25-22-26-13` —— 只还原一个文件夹，先删后复 |

## 功能

- **对等同步**：命令行列出的文件夹互为对等；每个最小单元取修改时间最新的版本，复制到所有文件夹。
- **最小保留目录层数**：`--min-level N` 时，深度为 `N+1` 的目录作为一个整体单元参与比较与替换；不设置时逐文件比较。例如 `--min-level 1` 表示 `root\a`、`root\b` 这一层目录各自整体替换。
- **目录整体备份**：目录单元被替换时，旧目录打成 `.zip`（最快压缩档），并在 `manifest.txt` 中登记，还原时自动识别为目录。
- **`.backup`**：备份目录统一为 `.backup`；Windows 下自动设置隐藏属性；v1 的 `backup` 目录首次运行时会自动迁移。
- **先计划、后执行**：同步前完整扫描所有文件夹，算出"要复制什么、备份什么、删除什么"，写入系统临时目录的 JSON 计划文件，并用独占句柄占住它（防止被清理、防止同一组文件夹并发同步）。
- **进度可见**：扫描进度、预检汇总、执行总进度、当前操作、已处理数据量、速度与剩余时间。
- **磁盘空间预检**：按卷汇总"复制 + 备份估算 + 1 GB 余量"，空间不足直接中止，不会写满磁盘；运行中如果剩余空间低于 512 MB 也会立即停止。
- **并行复制**：`--jobs N` 多线程并行处理不同单元；目录 zip 改为流式写盘，内存占用与目录大小无关。
- **同步期间占用文件夹**：Windows 下以"共享读、独占写"打开各根目录并保持到结束，提示不要手动修改；子文件仍可能被其他程序写入（Windows 没有内核级强制目录锁），所以请把这条当作保护而不是绝对隔离。
- **安全兜底**：备份失败就跳过该单元，绝不先删后拷；备份成功才删除旧版本；Ctrl+C 会清理临时计划文件与文件夹占用，退出码 130。
- **中文路径**：全程 UTF-8，Windows / Linux / macOS 的中文文件名与目录名都按原样同步。
- **排除规则**：`--exclude` 支持 `*` / `?`，可排除缓存、构建产物、`.git` 等不需要同步的内容。
- **跨平台**：Windows、Linux、macOS 三套单文件构建；GitHub Actions 自动编译并随 Release 发布。

## 下载

从 [Releases](https://github.com/zyq1223334444/synctool/releases) 下载。二进制由 CI 编译并附在 Release 里，仓库不提交编译产物。

| 平台 | 文件 | 说明 |
|---|---|---|
| Windows x64 | `synctool-<版本>-win64.exe` | 单文件，双击或命令行运行 |
| Linux x86_64 | `synctool-<版本>-linux_x86_64.tar.gz` | 静态链接，解压即用 |
| macOS Intel | `synctool-<版本>-macos_x86_64.tar.gz` | 命令行工具 |
| macOS Apple 芯片 | `synctool-<版本>-macos_arm64.tar.gz` | 命令行工具 |

Windows SmartScreen 会提示未签名程序，选择"仍要运行"即可。

## 快速开始

```bat
:: Windows：三个文件夹互相同步，第二层目录作为整体单元
SyncTool.exe sync D:\work E:\work-backup F:\work-mirror --min-level 1

:: 只预检，不改任何文件（会打印每个文件夹需要多少空间、每个操作是什么）
SyncTool.exe sync D:\work E:\work-backup --min-level 1 --dry-run

:: 看某个文件夹有哪些备份时间点
SyncTool.exe list D:\work

:: 还原到指定时间点（只还原一个文件夹）
SyncTool.exe restore D:\work --time 2026-10-25-22-26-13
```

```bash
# Linux / macOS 用法完全一致
./SyncTool sync ~/work /mnt/backup/work --min-level 1
./SyncTool restore ~/work
```

不带 `--min-level` 时是**逐文件**比较；带 `--min-level 1` 时，`root` 下每个子目录（`root/a`、`root/b`）作为整体单元，任何一个文件更新就整体替换，旧目录打成 zip。

## 命令与参数

```
SyncTool sync <文件夹1> <文件夹2> [...] [选项]
SyncTool restore <文件夹> [--time <时间戳>] [--dry-run]
SyncTool list <文件夹>
SyncTool help | --version
```

| 选项 | 说明 |
|---|---|
| `--min-level N` | 最小保留目录层数。深度 `N+1` 的目录作为整体单元；不设置 = 深度无限、逐文件比较 |
| `--jobs N` | 并行线程数（默认按 CPU，2–8；机械硬盘建议 2–4） |
| `--exclude <模式>` | 排除路径，可多次使用；支持 `*` / `?`，匹配相对路径或文件名。例：`--exclude "*/build/*" --exclude "*.tmp"` |
| `--dry-run` | 只扫描、预检、生成计划，不修改任何文件 |
| `--force` | 磁盘空间预检不足时仍继续（运行中的 512 MB 兜底仍生效） |
| `--time <时间戳>` | 还原到指定时间点；不写则列出时间点交互选择 |
| `--version` | 打印版本 |

退出码：`0` 成功 / `1` 有错误 / `2` 空间不足中止 / `130` 被 Ctrl+C 中断。

## 备份结构

以 `root` 为例，某次同步时间 `2026-10-25-22-26-13`：

```
root\
  a\b\c\abc.txt                    同步后的最新文件
  a\b\c1\                          同步后的最新目录
  .backup\                         （Windows 隐藏）
    2026-10-25-22-26-13\
      a\b\c\abc.txt                被替换的旧文件，保留原相对路径
      a\b\c1.zip                   被替换的旧目录，快速压缩
      manifest.txt                 记录哪些 zip 是目录备份
```

- 文件备份：按原相对路径直接存放，还原时先删当前文件再复制回来。
- 目录备份：`<相对路径>.zip`，并在 `manifest.txt` 里登记；还原时删除整个目录再解压。
- 每次同步一个时间戳目录；同一秒重复自动加 `-2`、`-3`。
- 备份只增不删，可还原到任意历史时间点。
- 根目录下的 `.backup` / `backup` 不参与同步。

## 工作原理

**阶段一 · 扫描**：并行扫描每个文件夹，跳过根级 `.backup`/`backup` 与符号链接，按 `--min-level` 把文件或目录归并成"单元"，记录每个单元在每个文件夹里的修改时间、大小和文件数。

**阶段二 · 预检与计划**：每个单元选时间最新的版本为胜者（时间相同则目录优先、参数靠前的文件夹优先）；对每个目标文件夹判断需要"新增、替换、跳过"。所有操作写入临时 JSON 计划，并做按卷的磁盘空间检查。

**阶段三 · 执行**：多线程并行处理不同单元。对每个需要替换的单元：

1. 先备份旧版本：文件复制到 `.backup\<时间>\<相对路径>`，目录流式压缩成 `.zip` 并登记 manifest；
2. 备份失败 → 跳过该单元，**不删除任何东西**；
3. 备份成功 → 删除旧版本 → 复制胜者（保留原修改时间，避免下次误判为新）；
4. 每个文件复制完更新时间戳，目录单元整树复制。

**还原**：读取 `.backup\<时间>\`，按 `manifest.txt` 识别目录 zip；对每个备份条目先删除目标位置的当前文件/目录，再复制或解压回去。备份中不存在的路径不受影响。

## 安全设计

- **计划文件独占**：计划写在系统临时目录 `SyncTool/plan-<hash>.json`，用独占句柄打开；同一组文件夹不能并发同步，结束后（含 Ctrl+C）自动删除。
- **空间预检**：按卷汇总需求（备份按 zip 压缩后 85% 保守估算 + 1 GB 余量）；不足直接退出，避免半截文件。
- **运行中兜底**：剩余空间低于 512 MB 立即停止写入，已完成的单元有备份，可还原。
- **文件夹占用**：Windows 下打开根目录句柄（共享读、独占写），阻止重命名/删除与 Explorer 进入；这不是内核级强制锁，仍建议同步时不要修改目录。
- **路径安全**：解压时拒绝绝对路径、盘符和 `..`；不跟随符号链接，避免循环与越界。
- **中断安全**：Ctrl+C 设置取消标志，当前文件写完后停止，清理临时计划与锁，退出码 130。

## 构建

Windows（MSVC 或 MinGW-w64）：

```bat
build.bat
```

Linux / macOS：

```bash
./build.sh
STATIC=1 ./build.sh   # Linux 全静态，用于发布
```

依赖只有 C++17 编译器与 pthread；zip 使用内置的 [miniz](https://github.com/richgel999/miniz)（公共领域），无需 zlib。

CI 在 `.github/workflows/build.yml`：Windows / Linux / macOS（Intel + Apple 芯片）四份构建，打 `v*` 标签时自动创建 Release 并上传单文件产物。

## 目录结构

```
synctool/
  src/main.cpp                 全部逻辑（扫描 / 计划 / 同步 / 还原 / 列表）
  third_party/miniz/           内嵌 zip 库
  build.bat                    Windows 一键编译（MSVC / MinGW）
  build.sh                     Linux / macOS 一键编译
  .github/workflows/build.yml  四平台 CI + Release
  README.md / README.zh-CN.md  双语文档
  LICENSE                      MIT
```

## 常见问题

**时间一样会复制吗？**
不会。只有目标严格更旧、或类型冲突（一边文件一边目录）时才替换；时间相同视为等价。

**为什么同步后备份目录很大？**
目录单元是整棵子树备份，zip 里保存了所有文件；确认无误后可以手动删除旧的时间戳目录。工具本身只增不删备份。

**机械硬盘同步很慢？**
`--jobs` 降到 2–4，避免多线程在同一个盘上互相寻道。大量小文件的目录（如 msys2、node_modules）本身就会慢，扫描和压缩都需要时间。

**可以排除缓存吗？**
可以，例如：

```bat
SyncTool.exe sync A B C --min-level 1 --exclude "*/node_modules/*" --exclude "*/.git/*" --exclude "*/build/*"
```

**同步时能继续用这些文件夹吗？**
不要。Windows 下工具会占用根目录，但子文件仍可能被其他程序改写；同步中修改可能导致这一轮结果不一致，请等结束。

**v1 的 `backup` 怎么办？**
v2 首次运行会把根级 `backup` 自动改名为 `.backup` 并隐藏；`restore`/`list` 同时兼容两种名字。

## 更新日志

### v2.0.0 — 2026-10-25

- 备份目录改为 `.backup`，Windows 自动隐藏；自动迁移 v1 的 `backup`。
- 同步前完整扫描并生成 JSON 操作计划，临时文件独占、退出/中断自动清理。
- 三阶段进度：扫描 / 预检 / 执行；显示总进度、当前操作、数据量、速度、剩余时间。
- 按卷磁盘空间预检；运行中 512 MB 低空间兜底。
- Windows 同步期间占用根文件夹；提示不要修改被同步目录。
- 多线程并行复制（`--jobs`）、流式 zip（内存占用不再随目录大小增长）。
- 时间相同的单元不再复制。
- 跨平台：Windows / Linux / macOS；GitHub Actions 四平台构建与 Release。
- 新增 `--exclude`、`--dry-run` 还原、退出码约定。

### v1.0.0 — 2026-10-25

- 对等同步、`--min-level`、目录 zip 备份 + manifest、按时间点还原、`list`/`help`。
- Windows 单平台，单线程。
