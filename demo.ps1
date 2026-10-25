# ============================================================================
# SyncTool v2 一键演示
# 创建演示目录 -> 预检 -> 同步（--min-level 2，目录单元 zip 备份）-> 篡改 -> 还原
# 运行：powershell -ExecutionPolicy Bypass -File demo.ps1
# ============================================================================
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $here 'output\SyncTool.exe'
if (-not (Test-Path -LiteralPath $exe)) { $exe = Join-Path $here 'output\SyncTool' }
$demo = Join-Path $here 'output\demo'
if (-not (Test-Path -LiteralPath $exe)) { Write-Host "[错误] 未找到 SyncTool，请先运行 build.bat / build.sh。" -ForegroundColor Red; exit 1 }
if (Test-Path -LiteralPath $demo) { Remove-Item -LiteralPath $demo -Recurse -Force }

$A = Join-Path $demo 'A'; $B = Join-Path $demo 'B'; $C = Join-Path $demo 'C'
foreach ($d in @($A, (Join-Path $A 'sub\c1\deep'), (Join-Path $B 'sub\c1\deep'), $C)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }

Set-Content -LiteralPath (Join-Path $A 'readme.txt')    -Value 'A-旧版 readme'      -Encoding UTF8
Set-Content -LiteralPath (Join-Path $A 'sub\c1\核心.txt') -Value 'A-旧版 核心'       -Encoding UTF8
Set-Content -LiteralPath (Join-Path $A 'sub\c1\deep\代码.txt') -Value 'A-旧版 代码'  -Encoding UTF8
Set-Content -LiteralPath (Join-Path $B 'readme.txt')    -Value 'B-新版 readme'      -Encoding UTF8
Set-Content -LiteralPath (Join-Path $B 'sub\c1\核心.txt') -Value 'B-新版 核心'       -Encoding UTF8
Set-Content -LiteralPath (Join-Path $B 'sub\c1\deep\代码.txt') -Value 'B-新版 代码'  -Encoding UTF8
Set-Content -LiteralPath (Join-Path $C '独有文件.txt')   -Value 'C-独有'             -Encoding UTF8
(Get-Item -LiteralPath (Join-Path $A 'readme.txt')).LastWriteTime      = [datetime]'2026-01-01 10:00'
(Get-Item -LiteralPath (Join-Path $A 'sub\c1\核心.txt')).LastWriteTime = [datetime]'2026-01-02 10:00'
(Get-Item -LiteralPath (Join-Path $A 'sub\c1\deep\代码.txt')).LastWriteTime = [datetime]'2026-01-03 10:00'
(Get-Item -LiteralPath (Join-Path $B 'readme.txt')).LastWriteTime      = [datetime]'2026-06-01 10:00'
(Get-Item -LiteralPath (Join-Path $B 'sub\c1\核心.txt')).LastWriteTime = [datetime]'2026-06-02 10:00'
(Get-Item -LiteralPath (Join-Path $B 'sub\c1\deep\代码.txt')).LastWriteTime = [datetime]'2026-06-03 10:00'
(Get-Item -LiteralPath (Join-Path $C '独有文件.txt')).LastWriteTime    = [datetime]'2026-06-04 10:00'

Write-Host "`n[1/5] 预检（--dry-run）：只扫描和算计划，不改文件" -ForegroundColor Cyan
& $exe sync $A $B $C --min-level 2 --dry-run
if ($LASTEXITCODE -ne 0) { exit 1 }

Write-Host "`n[2/5] 同步 A B C（--min-level 2）：同名文件取最新，目录单元整体替换" -ForegroundColor Cyan
& $exe sync $A $B $C --min-level 2 --jobs 2
if ($LASTEXITCODE -ne 0) { exit 1 }

Write-Host "`n[3/5] 同步后 A 的内容与 .backup 结构：" -ForegroundColor Cyan
Get-ChildItem -LiteralPath $A -Recurse -File -Force | Where-Object { $_.FullName -notmatch '\\\.backup\\' } |
  ForEach-Object { Write-Host ("  " + $_.FullName.Replace($demo, '') + " : " + (Get-Content -LiteralPath $_.FullName -Raw).Trim()) }
$bdir = Join-Path $A '.backup'
$ts = (Get-ChildItem -LiteralPath $bdir -Directory -Force | Sort-Object Name -Descending | Select-Object -First 1).Name
Write-Host "  备份时间点: $ts"
Write-Host ("  .backup 属性: " + (Get-Item -LiteralPath $bdir -Force).Attributes)
Write-Host "  manifest.txt（目录 zip 备份登记）:"
Get-Content -LiteralPath (Join-Path $bdir "$ts\manifest.txt") | ForEach-Object { Write-Host "    $_" }

Write-Host "`n[4/5] 篡改 A 后还原到该时间点（先删后复）" -ForegroundColor Cyan
Set-Content -LiteralPath (Join-Path $A 'readme.txt') -Value '被篡改的内容' -Encoding UTF8
Remove-Item -LiteralPath (Join-Path $A 'sub\c1') -Recurse -Force
& $exe restore $A --time $ts
if ($LASTEXITCODE -ne 0) { exit 1 }

Write-Host "`n[5/5] 还原结果：" -ForegroundColor Cyan
Get-ChildItem -LiteralPath $A -Recurse -File -Force | Where-Object { $_.FullName -notmatch '\\\.backup\\' } |
  ForEach-Object { Write-Host ("  " + $_.FullName.Replace($demo, '') + " : " + (Get-Content -LiteralPath $_.FullName -Raw).Trim()) }
Write-Host "`n演示完成。演示目录：$demo" -ForegroundColor Green
