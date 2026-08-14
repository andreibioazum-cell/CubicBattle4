@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo Building Cubic Battle for Windows PC (.exe)
echo ========================================================

:: 1. Generate game/game.c from DimScript source
echo [1/3] Compiling DimScript sources to game/game.c...
python gen.py
if errorlevel 1 (
    echo [ERROR] Failed to compile DimScript sources.
    pause
    exit /b 1
)

:: 2. Check compiler (GCC / Clang / MSVC)
echo [2/3] Compiling C code to Game.exe...
where gcc >nul 2>nul
if %errorlevel% equ 0 (
    echo Using GCC (MinGW)...
    gcc -O3 -Wall -Wextra -I. -Igame game/game.c runtime.c main_win32.c -lgdi32 -lwininet -luser32 -lkernel32 -lm -o Game.exe -mwindows
    if errorlevel 1 (
        echo [ERROR] Compilation with GCC failed.
        pause
        exit /b 1
    )
    goto build_success
)

where cl >nul 2>nul
if %errorlevel% equ 0 (
    echo Using MSVC (cl.exe)...
    cl /O2 /W3 /I. /Igame game/game.c runtime.c main_win32.c /Fe:Game.exe /link gdi32.lib wininet.lib user32.lib kernel32.lib shell32.lib /SUBSYSTEM:WINDOWS
    if errorlevel 1 (
        echo [ERROR] Compilation with MSVC failed.
        pause
        exit /b 1
    )
    goto build_success
)

echo [ERROR] Neither GCC (MinGW) nor MSVC (cl.exe) was found in PATH.
echo Please install MinGW (w64devkit / MSYS2) or Visual Studio build tools.
pause
exit /b 1

:build_success
echo [3/3] Copying assets alongside Game.exe...
if not exist "assets" mkdir "assets"
xcopy /E /I /Y "game\assets" "assets" >nul

echo ========================================================
echo Build complete: Game.exe
echo Run Game.exe to start the game!
echo ========================================================
