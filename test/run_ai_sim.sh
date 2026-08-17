#!/usr/bin/env bash
# Прогон бенчмарка ИИ бота (test/ai_sim.c) без окна и без сети.
# Печатает по каждому «игроку» время до первого попадания бота, темп его
# попаданий, темп попаданий игрока и счёт по раундам.
set -e
cd "$(dirname "$0")/.."
python3 gen.py >/dev/null
gcc -O1 -D_POSIX_C_SOURCE=200809L -I. -Igame \
    test/ai_sim.c runtime.c graphics.c net.c game/game.c \
    -lpthread -lm -o test/ai_sim
./test/ai_sim 2>/dev/null
