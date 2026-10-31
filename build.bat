@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

if not exist output mkdir output
if not exist build mkdir build

set "INC=-I third_party\miniz"
set "CSRC=third_party\miniz\miniz.c third_party\miniz\miniz_zip.c third_party\miniz\miniz_tdef.c third_party\miniz\miniz_tinfl.c"
set "BUILT=0"

rem ---- 1) MSVC already in PATH ----
where cl >nul 2>nul
if !errorlevel! equ 0 (
    echo [MSVC] Building with cl...
    rc /nologo /fo build\app.res res\app.rc
    cl /nologo /std:c++17 /EHsc /utf-8 /O2 /W3 /MT %INC% src\main.cpp %CSRC% build\app.res /Fe:output\SyncTool.exe /Fo:build\ /link /MANIFEST:NO
    if !errorlevel! equ 0 set "BUILT=1"
)

rem ---- 2) Visual Studio via vswhere ----
if not "!BUILT!"=="1" (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        set "VSROOT="
        for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
        if defined VSROOT (
            set "VCVARS=!VSROOT!\VC\Auxiliary\Build\vcvarsall.bat"
            if exist "!VCVARS!" (
                echo [MSVC] Building inside Visual Studio environment...
                cmd /c ""!VCVARS!" x64 >nul 2>nul && rc /nologo /fo build\app.res res\app.rc && cl /nologo /std:c++17 /EHsc /utf-8 /O2 /W3 /MT %INC% src\main.cpp %CSRC% build\app.res /Fe:output\SyncTool.exe /Fo:build\ /link /MANIFEST:NO"
                if !errorlevel! equ 0 set "BUILT=1"
            )
        )
    )
)

rem ---- 3) MinGW-w64 ----
if not "!BUILT!"=="1" (
    where g++ >nul 2>nul
    if !errorlevel! equ 0 (
        echo [MinGW] Building with g++...
        set "RES="
        where windres >nul 2>nul
        if !errorlevel! equ 0 (
            windres res\app.rc -O coff -o build\app.res
            if !errorlevel! equ 0 set "RES=build\app.res"
        )
        gcc -O2 -c %INC% third_party\miniz\miniz.c -o build\miniz.o
        gcc -O2 -c %INC% third_party\miniz\miniz_zip.c -o build\miniz_zip.o
        gcc -O2 -c %INC% third_party\miniz\miniz_tdef.c -o build\miniz_tdef.o
        gcc -O2 -c %INC% third_party\miniz\miniz_tinfl.c -o build\miniz_tinfl.o
        if !errorlevel! equ 0 (
            g++ -std=c++17 -O2 -Wall -Wextra -pthread %INC% src\main.cpp build\miniz.o build\miniz_zip.o build\miniz_tdef.o build\miniz_tinfl.o !RES! -o output\SyncTool.exe -static -static-libgcc -static-libstdc++ -ladvapi32 -lshell32
            if !errorlevel! equ 0 set "BUILT=1"
        )
    )
)

if "!BUILT!"=="1" (
    echo Build OK: %CD%\output\SyncTool.exe
    exit /b 0
)
echo [ERROR] No compiler found. Install Visual Studio Build Tools (Desktop C++) or MinGW-w64, then retry.
exit /b 1
