#!/usr/bin/env bash
set -eu

# Game-specific files live in GAME_DIR.  The arguments make it possible to
# compile another .ds file without changing the compiler or the Android host.
GAME_DIR="${GAME_DIR:-game}"
SOURCE="${1:-$GAME_DIR/game.ds}"
OUTPUT="${2:-$GAME_DIR/game.c}"

if [ ! -f "$SOURCE" ]; then
    echo "error: DimScript source not found: $SOURCE" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"
compiler="$(mktemp "${TMPDIR:-/tmp}/dimscript-compiler.XXXXXX")"
trap 'rm -f "$compiler"' EXIT

"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
    ds_compiler.c -o "$compiler"
"$compiler" "$SOURCE" "$OUTPUT"

echo "$OUTPUT generated from $SOURCE"
