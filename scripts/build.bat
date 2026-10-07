@echo off
REM SutureOS build script (batch version to avoid PowerShell encoding issues)
setlocal enabledelayedexpansion

set ROOT=C:\Users\fanqi\Desktop\缝合怪系统
set CC=C:\msys64\mingw64\bin\clang.exe
set LDCAND1=%ROOT%\tools\cross\bin\x86_64-elf-ld.exe
set LDCAND2=C:\Users\fanqi\AppData\Local\Temp\xb64\install\bin\x86_64-elf-ld.exe

if not exist "%CC%" (
    echo clang not found: %CC%
    exit /b 1
)

REM Find linker
set LD=
if exist "%LDCAND1%" set LD=%LDCAND1%
if not defined LD if exist "%LDCAND2%" set LD=%LDCAND2%
if not defined LD (
    echo x86_64-elf-ld not found. Run scripts/fetch-toolchain first.
    exit /b 1
)

set BUILD=%ROOT%\build
set OBJ=%BUILD%\obj

if exist "%OBJ%" rmdir /s /q "%OBJ%"
mkdir "%OBJ%"

REM Get clang resource directory
for /f "delims=" %%i in ('%CC% -print-resource-dir') do set RESOURCE_DIR=%%i

set CFLAGS=--target=x86_64-unknown-none-elf -ffreestanding -fno-builtin -mno-red-zone -mno-sse -mno-mmx -mno-80387 -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem "%RESOURCE_DIR%\include" -I"%ROOT%\include" -I"%ROOT%\arch\x86_64" -I"%ROOT%\kernel\interrupts" -O2 -g -Wall -Wextra

echo Collecting sources...

set SOURCES=
for /r "%ROOT%\boot" %%f in (*.S) do set SOURCES=!SOURCES! "%%f"
for /r "%ROOT%\arch\x86_64" %%f in (*.c) do set SOURCES=!SOURCES! "%%f"
for /r "%ROOT%\arch\x86_64" %%f in (*.S) do set SOURCES=!SOURCES! "%%f"
for /r "%ROOT%\kernel" %%f in (*.c) do set SOURCES=!SOURCES! "%%f"
for /r "%ROOT%\kernel" %%f in (*.S) do set SOURCES=!SOURCES! "%%f"
for /r "%ROOT%\lib" %%f in (*.c) do set SOURCES=!SOURCES! "%%f"

if "!SOURCES!"=="" (
    echo no sources found
    exit /b 1
)

echo Compiling...

set OBJS=
set FAIL=0
for %%s in (!SOURCES!) do (
    set "REL=%%s"
    set "REL=!REL:%ROOT%\=!"
    set "FLAT=!REL:\=__!"
    set "FLAT=!FLAT:/=__!"
    set "O=%OBJ%\!FLAT!.o"
    set "EXT=%%~xs"
    set "ARGS=-c !CFLAGS! -o \"!O!\" \"%%s\""
    echo Compiling !REL!
    %CC% !ARGS!
    if errorlevel 1 (
        echo COMPILE FAIL: !REL!
        set /a FAIL+=1
    ) else (
        set OBJS=!OBJS! "!O!"
    )
)

if %FAIL% gtr 0 (
    echo %FAIL% file(s) failed to compile
    exit /b 1
)

echo Linking...
set ELF=%BUILD%\kernel.elf
"%LD%" -T "%ROOT%\boot\linker.ld" -o "%ELF%" %OBJS%
if errorlevel 1 (
    echo link failed
    exit /b 1
)

echo BUILD OK -> %ELF%
%LD% --version
for %%f in ("%ELF%") do echo kernel.elf: %%~zf bytes