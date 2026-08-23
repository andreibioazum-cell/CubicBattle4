#!/usr/bin/env bash
# build_web.sh — браузерная сборка ZeroHabit через Emscripten.
#
# Итог кладётся в web/dist: index.html, game.js, game.wasm, game.data
# (картинки и шрифт) и favicon, если в корне проекта лежит icon.png.
# Достаточно раздать эту папку любым статическим сервером или залить на itch.io.
#
#   ./build_web.sh          # обычная сборка
#   python3 -m http.server -d web/dist 8080   # локальная проверка
set -euo pipefail
cd "$(dirname "$0")"

command -v emcc >/dev/null || { echo "emcc не найден: активируй emsdk (source emsdk_env.sh)"; exit 1; }

python3 gen.py
mkdir -p web/dist
rm -f web/dist/index.html web/dist/game.js web/dist/game.wasm web/dist/game.data

# Иконка страницы: берём icon.png из корня проекта (или icon.ico, если он есть).
if [ -f icon.png ]; then
  cp icon.png web/dist/favicon.png
  echo "иконка: icon.png -> web/dist/favicon.png"
elif [ -f icon.ico ]; then
  cp icon.ico web/dist/favicon.ico
  echo "иконка: icon.ico -> web/dist/favicon.ico"
else
  echo "иконка: icon.png в корне не найден, favicon пропущен"
fi

emcc game/game.c runtime.c main_web.c \
  -I. -Igame \
  -O3 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sINITIAL_MEMORY=64MB \
  -sEXIT_RUNTIME=0 \
  -sFILESYSTEM=1 \
  -lidbfs.js \
  --preload-file game/assets@/assets \
  --shell-file web/shell.html \
  -o web/dist/index.html

# emcc называет побочные файлы по имени выходного html — переименуем в game.*
if [ -f web/dist/index.js ]; then
  mv web/dist/index.js web/dist/game.js
  [ -f web/dist/index.wasm ] && mv web/dist/index.wasm web/dist/game.wasm
  [ -f web/dist/index.data ] && mv web/dist/index.data web/dist/game.data
  sed -i.bak -e 's/index\.js/game.js/g' -e 's/index\.wasm/game.wasm/g' -e 's/index\.data/game.data/g' \
    web/dist/index.html web/dist/game.js
  rm -f web/dist/*.bak
fi

echo "готово: web/dist"
ls -la web/dist
