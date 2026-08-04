#!/usr/bin/env bash
set -eu

# A game is a directory of DimScript files.  Every .ds file in that directory
# is compiled together, so functions and globals can be split between files.
GAME_DIR="${GAME_DIR:-game}"
SOURCES=()

if [ "$#" -eq 0 ]; then
    INPUT="$GAME_DIR"
    OUTPUT="$GAME_DIR/game.c"
elif [ -d "$1" ]; then
    INPUT="$1"
    OUTPUT="${2:-$INPUT/game.c}"
    if [ "$#" -gt 2 ]; then
        echo "usage: $0 [game-directory [output.c]] | [source.ds output.c]" >&2
        exit 2
    fi
else
    INPUT="$1"
    OUTPUT="${2:-${1%.ds}.c}"
    if [ "$#" -gt 2 ]; then
        echo "usage: $0 [game-directory [output.c]] | [source.ds output.c]" >&2
        exit 2
    fi
fi

if [ -d "$INPUT" ]; then
    while IFS= read -r -d '' source; do
        SOURCES+=("$source")
    done < <(find "$INPUT" -maxdepth 1 -type f -name '*.ds' -print0 | sort -z)
else
    SOURCES+=("$INPUT")
fi

if [ "${#SOURCES[@]}" -eq 0 ]; then
    echo "error: no .ds files found in $INPUT" >&2
    exit 1
fi
for source in "${SOURCES[@]}"; do
    if [ ! -f "$source" ]; then
        echo "error: DimScript source not found: $source" >&2
        exit 1
    fi
done

mkdir -p "$(dirname "$OUTPUT")"
compiler="$(mktemp "${TMPDIR:-/tmp}/dimscript-compiler.XXXXXX")"
trap 'rm -f "$compiler"' EXIT

"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
    ds_compiler.c -o "$compiler"
"$compiler" --output "$OUTPUT" "${SOURCES[@]}"

echo "$OUTPUT generated from ${#SOURCES[@]} DimScript file(s)"
