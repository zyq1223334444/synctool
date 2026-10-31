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
- **Minimum retained directory level** — `--min-level N` makes directories at depth `N+1` whole units; omit it to compare file by file. `--min-level 1` means depth-2 directories (`root\a\b`, e.g. `C++\SyncTool`); `--min-level 0` means depth-1 (`root\a`).
- **Equal-time content compare** — equal mtimes are no longer skipped blindly: content hashes are compared. Equal hashes skip; different hashes queue a conflict prompt asking which folder is the source, or skip. Prompts run on a separate thread while copy workers continue; multiple conflicts are asked one by one.
- **Directory backups as zip** — when a directory unit is replaced, the old tree is packed with the fastest deflate level and registered in `manifest.txt`, so restore knows it is a directory.
- **`.backup`** — the backup folder is `.backup`, hidden on Windows; a v1 `backup` folder is migrated automatically on the first run.
- **Plan first, then execute** — a full scan builds a JSON operation plan in the system temp folder, held open with an exclusive handle (prevents concurrent runs and temp cleanup).
- **Visible progress** — scan, preflight, hash and apply phases with current operation, bytes, speed and ETA.
- **Disk space preflight** — required bytes are aggregated per volume with a 1 GB margin; it refuses to start when the target cannot hold the result. A 512 MB low-space guard also stops a running sync.
- **Parallel copies** — `--jobs N` works on independent units; directory zips are streamed to disk, so memory does not grow with directory size.
- **Folder occupancy on Windows** — root folders are held open (shared read, exclusive write) for the whole sync; child files can still be written by other programs (Windows has no kernel mandatory directory lock), so please do not edit them while syncing.
- **Safe by default** — backup first, skip the unit if the backup fails, never delete before a successful backup. Ctrl+C asks whether to keep the run's backups for a later resume, then whether to roll back completed changes (delete added entries, restore replaced ones from backups).
- **Backup cleanup** — `clean` deletes selected time points by index, `--time` or `--all`, with sizes shown and a confirmation.
- **Long paths** — on Windows, when `LongPathsEnabled` is off the tool asks, enables it automatically (elevating via UAC when needed) and relaunches itself so the embedded `longPathAware` manifest takes effect.
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

:: Restore one folder to a time point (omit --time to pick by index)
SyncTool.exe restore D:\work --time 2026-10-25-22-26-13

:: Clean backups: pick by index, one timestamp, or all
SyncTool.exe clean D:\work
SyncTool.exe clean D:\work --time 2026-10-25-22-26-13
SyncTool.exe clean D:\work --all --dry-run
```

```bash
# Linux / macOS: same commands
./SyncTool sync ~/work /mnt/backup/work --min-level 1
./SyncTool restore ~/work
./SyncTool clean ~/work
```

Without `--min-level`, files are compared one by one. With `--min-level 1`, depth-2 directories (`root/a/b`, e.g. `C++/SyncTool`) are units; any newer file replaces the whole directory and the old one is zipped. `--min-level 0` is `root/a`.

**Equal time, different content**: hashes are compared after the scan. On a mismatch the tool lists every folder with its size and hash prefix; type an index to pick the source, or `0` to skip. Multiple conflicts are queued; ordinary copying continues on the worker threads. `--yes` skips all conflicts.

## Commands and options

```
SyncTool sync <folder1> <folder2> [...] [options]
SyncTool restore <folder> [--time <timestamp>] [--dry-run]
SyncTool list <folder>
SyncTool clean <folder> [--time <timestamp>|--all] [--dry-run] [--yes]
SyncTool help | --version
```

| Option | Meaning |
|---|---|
| `--min-level N` | minimum retained directory level; depth `N+1` directories are units; omit for per-file comparison |
| `--jobs N` | worker threads (default: CPU count, 2–8; use 2–4 on mechanical drives) |
| `--exclude <pattern>` | skip paths; repeatable; `*` and `?` supported, matched against relative path or file name. Example: `--exclude "*/build/*" --exclude "*.tmp"` |
| `--dry-run` | scan, preflight, compare hashes and print the plan; modify nothing; reports conflict count |
| `--force` | continue even if the disk-space preflight fails (the 512 MB runtime guard still applies) |
| `--yes` | accept defaults for all prompts: skip conflicts, keep backups on interrupt, enable long paths, confirm cleanup |
| `--no-long-paths` | do not auto-enable Windows long-path support |
| `--no-hash-cache` | do not use the cross-run hash cache; re-read every file (most accurate, slower) |
| `--time <timestamp>` | restore/clean this time point; omit to pick by index |
| `--all` | `clean` only: remove every time point |
| `--version` | print the version |

Time points are numbered from **1**; `clean` accepts multiple indexes such as `1,3`.

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
      journal.jsonl                per-run change log (kept on interrupt for rollback)
```

- File backups keep the relative path; restore deletes the current file and copies the backup back.
- Directory backups are `<relative path>.zip` and are listed in `manifest.txt`; restore deletes the whole directory and unzips.
- One timestamp folder per sync; collisions within a second get `-2`, `-3`.
- On interrupt, backups and `journal.jsonl` are kept by default so the next run can continue; rollback deletes entries added by the run and restores replaced ones from their backups.
- `clean` removes selected time points by index; cleanup is irreversible.
- Root `.backup` / `backup` folders are never synced.

## How it works

**1. Scan** — all folders are scanned in parallel, skipping root `.backup`/`backup` and symlinks, grouping files/directories into units according to `--min-level`, and recording each unit's mtime, size and file count in each folder.

**2. Preflight and plan** — the newest version of each unit wins (on equal time: directory over file, earlier folder argument over later). For every target folder the plan says add, replace or skip. The plan is written to a JSON temp file and checked against free space per volume.

**3. Hash compare** — units with equal mtime and type get a content hash (files directly; directories combine sorted paths, sizes and file hashes). Equal hashes are skipped; different hashes become conflicts. Hash results are cached in the system temp folder, keyed by path+size+mtime.

**4. Apply** — a thread pool copies ordinary units. A separate resolver thread asks conflict questions one by one. For a unit that needs replacement:

1. write journal `started`;
2. back up the old version (file copy, or a streamed zip for directories, plus a manifest entry);
3. if the backup fails, mark `failed`, skip the unit and delete nothing;
4. otherwise write `backedUp`, delete the old version, copy the winner, preserve mtimes, write `done`;
5. directory units are copied as a whole tree.

**Conflicts** list every folder's index, size and hash prefix; type an index to pick the source or `0` to skip. Questions are queued; ordinary copy workers keep running (unless all ordinary work is already done).

**Restore** reads `.backup\<timestamp>\`, uses `manifest.txt` to tell directory zips from file backups, deletes the current target path, then copies or unzips. Paths that are not in the backup are untouched.

**Interrupt** — Ctrl+C stops workers, asks whether to keep this run's backups; if not, asks whether to roll back. Rollback walks the journal backwards: added paths are deleted, replaced paths are restored from backups.

## Safety

- **Exclusive plan file** — `SyncTool/plan-<hash>.json` in the system temp folder is opened with an exclusive handle; two syncs over the same folder set cannot run together, and the file is deleted on normal exit and on Ctrl+C.
- **Space preflight** — per-volume demand (zip backup estimated at 85% of raw + 1 GB margin). It refuses to start if the result cannot fit.
- **Runtime guard** — under 512 MB free it stops writing; completed units have backups.
- **Folder occupancy** — on Windows the root handles are held with shared read / exclusive write, blocking rename, delete and Explorer entry. This is not a kernel mandatory lock; do not edit the folders while syncing.
- **Path safety** — zip entries with absolute paths, drive letters or `..` are rejected; symlinks are not followed.
- **Interrupt safety** — Ctrl+C stops workers and asks "keep backups / roll back changes". The journal records `started / backedUp / done / failed` per unit, so rollback can delete added paths and restore replaced ones precisely.
- **Long-path safety** — Windows builds embed a `longPathAware` manifest; when `LongPathsEnabled` is off the tool asks, writes the registry value (elevating if needed) and relaunches itself.
- **Hash cache** — equal-time hashes are cached in the system temp folder, invalidated by path+size+mtime; synced folders are not polluted.

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

Windows builds embed `res\app.manifest` (needs the Windows SDK `rc.exe`; MinGW uses `windres`) so the process is long-path aware. Linux/macOS do not need that resource.

`.github/workflows/build.yml` builds Windows, Linux and both macOS architectures, and on a `v*` tag creates a Release with the four single-file assets.

## Layout

```
synctool/
  src/main.cpp                 scan / plan / hash / sync / restore / clean / list
  res/app.manifest             longPathAware manifest (Windows)
  res/app.rc                   manifest resource script
  third_party/miniz/           embedded zip library
  build.bat                    one-command Windows build (MSVC / MinGW)
  build.sh                     one-command Linux / macOS build
  .github/workflows/build.yml  CI + release
  README.md / README.zh-CN.md  bilingual docs
  LICENSE                      MIT
```

## FAQ

**Are equal timestamps copied?**
They are content-hashed: equal hashes are skipped; different hashes list every folder and ask for a source, or `0` to skip. `--yes` skips all conflicts automatically.

**Why is `.backup` so large?**
A directory unit is backed up as a whole tree. Use `SyncTool clean <folder>` to remove old time points by index; nothing is deleted automatically.

**Mechanical drives are slow. What should I use?**
Lower `--jobs` to 2–4 so workers do not fight for the same head. Trees with many small files (msys2, node_modules) are inherently slow to scan and compress. The first run also hashes equal-time units; later runs use the hash cache.

**Can the hash cache miss a change?**
Entries are invalidated by path + size + mtime; normal edits and copies change mtime, and the tool erases entries for paths it writes. Only a rewrite that preserves both size and mtime (rare) could be missed; use `--no-hash-cache` to force a full re-read every run.

**Can I exclude caches?**

```bat
SyncTool.exe sync A B C --min-level 1 --exclude "*/node_modules/*" --exclude "*/.git/*" --exclude "*/build/*"
```

**Can I keep using the folders while syncing?**
No. Windows folder handles block directory-level changes, but child files can still be written by other programs; editing during a sync can make the result inconsistent.

**What happens to backups after Ctrl+C?**
The tool asks whether to keep the run's backups (default yes, so the next run can continue). If not, it asks whether to roll back completed changes; rollback failures keep the backups for manual recovery.

**Do I need to enable Windows long paths manually?**
No. If `LongPathsEnabled` is off, the tool asks, writes the registry value (UAC elevation when needed) and relaunches itself. Pass `--no-long-paths` to skip.

**What about the v1 `backup` folder?**
The first v2 run renames a root `backup` to `.backup` and hides it; `restore`/`list`/`clean` accept both names.

## Changelog

### v2.1.0 — 2026-10-31

- Equal-time content hashing: equal hashes skip, different hashes prompt for a source or skip; prompts run on a separate resolver thread while workers keep copying; multiple conflicts are queued.
- Ctrl+C asks "keep backups for resume / roll back this run"; `journal.jsonl` records per-unit `started / backedUp / done / failed`; rollback deletes added paths and restores files or directory zips.
- New `clean` command: index multi-select, `--time`, `--all`, size display and confirmation.
- All time-point selection uses 1-based indexes; `restore`/`list`/`clean` are consistent.
- Windows long paths: embedded `longPathAware` manifest; auto-enable registry, optional UAC elevation, self-relaunch.
- `--yes` accepts defaults; `--no-long-paths` skips auto-enable.
- Hash cache in the system temp folder avoids re-reading large files on later runs.

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
