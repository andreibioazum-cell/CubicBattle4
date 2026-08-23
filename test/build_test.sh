#!/usr/bin/env bash
# Сборка головного тест-стенда (Linux, без окна, ASAN+UBSAN).
set -e
cd "$(dirname "$0")/.."
python3 gen.py >/dev/null
gcc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -D_POSIX_C_SOURCE=200809L -I. -Igame \
    test/main_linux.c net.c runtime.c graphics.c audio.c game/game.c \
    -lpthread -lm -o test/game_test
echo "test/game_test built"
