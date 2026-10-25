[🇨🇳 中文](README.zh-CN.md) · [⬇ Download](https://github.com/zyq1223334444/synctool/releases) · [Changelog](#changelog)

# SyncTool

**Treat several folders as one dataset: the newest version wins, the old one goes to `.backup`, restore any time point later.**

A single-file command line tool for Windows, Linux and macOS. C++17, no runtime, no external zip program. Give it a few folders and it makes them the same; every replaced version is kept in a restorable backup.

| | |
|---|---|
| **Sync** | `SyncTool sync A B C --min-level 1` — peer sync, newest modification time wins |
| **Backup** | replaced files and directories go to each folder's own `.backup\<timestamp>\`; hidden on Windows |
| **Restore** | `SyncTool restore A --time 2026-10-25-22-26-13` — one folder at a time, delete then restore |

## Features

- **Peer sync** — all listed folders are equal; each minimal unit takes the newest version and is copied everywhere.
- **Minimum retained directory level** — `--min-level N` makes directories at depth `N+1` whole units; omit it to compare file by file. `--min-level 1` means every second-level directory (`root\a`, `root\b`, …) is replaced as a unit.
- **Directory backups as zip** — when a directory unit is replaced, the old tree is packed with the fastest deflate level and registered in `manifest.txt`, so restore knows it is a directory.
- **`.backup`** — the backup folder is `.backup`, hidden on Windows; a v1 `backup` folder is migrated automatically on the first run.
- **Plan first, then execute** — a full scan builds a JSON operation plan in the system temp folder, held open with an exclusive handle (prevents concurrent runs and temp cleanup).
- **Visible progress** — scan progress, preflight summary, overall progress, current operation, bytes, speed and ETA.
- **Disk space preflight** — required bytes are aggregated per volume with a 1 GB margin; it refuses to start when the target cannot hold the result. A 512 MB low-space guard also stops a running sync.
- **Parallel copies** — `--jobs N` works on independent units; directory zips are streamed to disk, so memory does not grow with directory size.
- **Folder occupancy on Windows** — root folders are held open (shared read, exclusive write) for the whole sync; child files can still be written by other programs (Windows has no kernel mandatory directory lock), so please do not edit them while syncing.
- **Safe by default** — backup first, skip the unit if the backup fails, never delete before a successful backup; Ctrl+C cleans the temp plan and locks and exits with 130.
- **UTF-8 paths** — Chinese and other Unicode names work on all three platforms.
- **Excludes** — `--exclude` with `*` / `?` for caches, build output, `.git`, …
- **Cross-platform** — Windows, Linux, macOS single-file builds; GitHub Actions builds and publishes them with each Release.

## Download

Get the binaries from [Releases](https://github.com/zyq1223334444/synctool/releases). CI builds them; compiled files are not committed.

| Platform | Asset | Notes |
|---|---|---|
| Windows x64 | `synctool-<version>-win64.exe` | single file |
| Linux x86_64 | `synctool-<version>-linux_x86_64.tar.gz` | statically linked |
| macOS Intel | `synctool-<version>-macos_x86_64.tar.gz` | command line |
| macOS Apple silicon | `synctool-<version>-macos_arm64.tar.gz` | command line |

Windows SmartScreen will warn about an unsigned binary; choose "Run anyway".

## Quick start

```bat
:: Windows: three folders, second-level directories as units
SyncTool.exe sync D:\work E:\work-backup F:\work-mirror --min-level 1

:: Plan only, change nothing
SyncTool.exe sync D:\work E:\work-backup --min-level 1 --dry-run

:: List backup time points
SyncTool.exe list D:\work

:: Restore one folder to a time point
SyncTool.exe restore D:\work --time 2026-10-25-22-26-13
```

```bash
# Linux / macOS: same commands
./SyncTool sync ~/work /mnt/backup/work --min-level 1
./SyncTool restore ~/work
```

Without `--min-level`, files are compared one by one. With `--min-level 1`, every subdirectory of each root is one unit: if any file inside is newer, the whole directory replaces the others and the old one is zipped.

## Commands and options

```
SyncTool sync <folder1> <folder2> [...] [options]
SyncTool restore <folder> [--time <timestamp>] [--dry-run]
SyncTool list <folder>
SyncTool help | --version
```

| Option | Meaning |
|---|---|
| `--min-level N` | minimum retained directory level; depth `N+1` directories are units; omit for per-file comparison |
| `--jobs N` | worker threads (default: CPU count, 2–8; use 2–4 on mechanical drives) |
| `--exclude <pattern>` | skip paths; repeatable; `*` and `?` supported, matched against relative path or file name. Example: `--exclude "*/build/*" --exclude "*.tmp"` |
| `--dry-run` | scan, preflight and print the plan; modify nothing |
| `--force` | continue even if the disk-space preflight fails (the 512 MB runtime guard still applies) |
| `--time <timestamp>` | restore this time point; omit to pick interactively |
| `--version` | print the version |

Exit codes: `0` success, `1` errors, `2` disk-space abort, `130` interrupted.

## Backup layout

For a folder `root` and sync timestamp `2026-10-25-22-26-13`:

```
root\
  a\b\c\abc.txt                    newest file after sync
  a\b\c1\                          newest directory after sync
  .backup\                         hidden on Windows
    2026-10-25-22-26-13\
      a\b\c\abc.txt                replaced file, original relative path
      a\b\c1.zip                   replaced directory, fast deflate
      manifest.txt                 which zip files are directory backups
```

- File backups keep the relative path; restore deletes the current file and copies the backup back.
- Directory backups are `<relative path>.zip` and are listed in `manifest.txt`; restore deletes the whole directory and unzips.
- One timestamp folder per sync; collisions within a second get `-2`, `-3`.
- Backups are append-only and can be restored at any time point.
- Root `.backup` / `backup` folders are never synced.

## How it works

**1. Scan** — all folders are scanned in parallel, skipping root `.backup`/`backup` and symlinks, grouping files/directories into units according to `--min-level`, and recording each unit's mtime, size and file count in each folder.

**2. Preflight and plan** — the newest version of each unit wins (on equal time: directory over file, earlier folder argument over later). For every target folder the plan says add, replace or skip. The plan is written to a JSON temp file and checked against free space per volume.

**3. Apply** — independent units are processed by a thread pool. For a unit that needs replacement:

1. back up the old version (file copy, or a streamed zip for directories, plus a manifest entry);
2. if the backup fails, skip the unit and delete nothing;
3. otherwise delete the old version and copy the winner, preserving mtimes so it is not seen as newer next time;
4. directory units are copied as a whole tree.

**Restore** reads `.backup\<timestamp>\`, uses `manifest.txt` to tell directory zips from file backups, deletes the current target path, then copies or unzips. Paths that are not in the backup are untouched.

## Safety

- **Exclusive plan file** — `SyncTool/plan-<hash>.json` in the system temp folder is opened with an exclusive handle; two syncs over the same folder set cannot run together, and the file is deleted on normal exit and on Ctrl+C.
- **Space preflight** — per-volume demand (zip backup estimated at 85% of raw + 1 GB margin). It refuses to start if the result cannot fit.
- **Runtime guard** — under 512 MB free it stops writing; completed units have backups.
- **Folder occupancy** — on Windows the root handles are held with shared read / exclusive write, blocking rename, delete and Explorer entry. This is not a kernel mandatory lock; do not edit the folders while syncing.
- **Path safety** — zip entries with absolute paths, drive letters or `..` are rejected; symlinks are not followed.
- **Interrupt safety** — Ctrl+C sets a cancel flag, stops after the current file, removes the temp plan and folder locks, exits 130.

## Build

Windows (MSVC or MinGW-w64):

```bat
build.bat
```

Linux / macOS:

```bash
./build.sh
STATIC=1 ./build.sh   # fully static on Linux, for releases
```

The only requirements are a C++17 compiler and pthread. Zip support is the bundled public-domain [miniz](https://github.com/richgel999/miniz); no zlib, no external `zip`/`unzip`.

`.github/workflows/build.yml` builds Windows, Linux and both macOS architectures, and on a `v*` tag creates a Release with the four single-file assets.

## Layout

```
synctool/
  src/main.cpp                 scan / plan / sync / restore / list
  third_party/miniz/           embedded zip library
  build.bat                    one-command Windows build (MSVC / MinGW)
  build.sh                     one-command Linux / macOS build
  .github/workflows/build.yml  CI + release
  README.md / README.zh-CN.md  bilingual docs
  LICENSE                      MIT
```

## FAQ

**Are equal timestamps copied?**
No. A unit is replaced only when the target is strictly older, or when one side is a file and the other a directory. Equal time means equivalent.

**Why is `.backup` so large?**
A directory unit is backed up as a whole tree. Old timestamp folders can be deleted manually when you no longer need them; the tool itself never deletes backups.

**Mechanical drives are slow. What should I use?**
Lower `--jobs` to 2–4 so workers do not fight for the same head. Trees with many small files (msys2, node_modules) are inherently slow to scan and compress.

**Can I exclude caches?**

```bat
SyncTool.exe sync A B C --min-level 1 --exclude "*/node_modules/*" --exclude "*/.git/*" --exclude "*/build/*"
```

**Can I keep using the folders while syncing?**
No. Windows folder handles block directory-level changes, but child files can still be written by other programs; editing during a sync can make the result inconsistent.

**What about the v1 `backup` folder?**
The first v2 run renames a root `backup` to `.backup` and hides it; `restore`/`list` accept both names.

## Changelog

### v2.0.0 — 2026-10-25

- Backups moved to `.backup`, hidden on Windows; automatic migration from v1 `backup`.
- Full preflight scan with a JSON plan, held exclusively and cleaned on exit/interrupt.
- Three-phase progress (scan / preflight / apply) with current operation, bytes, speed and ETA.
- Per-volume disk-space preflight; 512 MB runtime guard.
- Windows folder occupancy during sync; explicit warning not to edit synced folders.
- Parallel copies (`--jobs`) and streamed zips (constant memory).
- Equal-timestamp units are skipped.
- Cross-platform Windows / Linux / macOS; four-platform CI and releases.
- New `--exclude`, restore `--dry-run`, documented exit codes.

### v1.0.0 — 2026-10-25

- Peer sync, `--min-level`, directory zip backups + manifest, restore by time point, `list`/`help`.
- Windows only, single-threaded.
