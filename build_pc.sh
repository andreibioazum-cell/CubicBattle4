#!/bin/bash
set -e
python3 gen.py
CC="${CC:-x86_64-w64-mingw32-gcc}"
$CC -O3 -s -Wall -Wextra -I. -Igame game/game.c runtime.c main_win32.c -lgdi32 -lwininet -luser32 -lkernel32 -lm -o Game.exe -mwindows
mkdir -p assets
cp -r game/assets/* assets/
echo "Build complete: Game.exe"
