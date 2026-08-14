#!/bin/bash
set -e

echo "========================================================"
echo "Building Cubic Battle for Windows PC (.exe)"
echo "========================================================"

# 1. Generate game/game.c from DimScript source
echo "[1/3] Compiling DimScript sources to game/game.c..."
python3 gen.py

# 2. Cross-compile using MinGW
echo "[2/3] Compiling C code to Game.exe with MinGW..."
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    COMPILER="x86_64-w64-mingw32-gcc"
elif command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
    COMPILER="i686-w64-mingw32-gcc"
else
    echo "Error: MinGW cross-compiler (x86_64-w64-mingw32-gcc) not found."
    echo "Install it with: sudo apt-get install -y gcc-mingw-w64-x86-64"
    exit 1
fi

$COMPILER -O3 -s -Wall -Wextra -I. -Igame \
    game/game.c runtime.c main_win32.c \
    -lgdi32 -lwininet -luser32 -lkernel32 -lm \
    -o Game.exe -mwindows

echo "[3/3] Copying assets alongside Game.exe..."
mkdir -p assets
cp -r game/assets/* assets/

echo "========================================================"
echo "Build complete: Game.exe"
echo "========================================================"
