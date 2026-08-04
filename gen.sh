#!/usr/bin/env bash
set -eu

# Build the compiler on the host and use the checked-in DimScript source as the
# only game source.  Keeping a hand-written game.c here used to hide compiler
# and runtime failures from the language itself.
compiler="$(mktemp "${TMPDIR:-/tmp}/dimscript-compiler.XXXXXX")"
trap 'rm -f "$compiler"' EXIT

"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
    ds_compiler.c -o "$compiler"
"$compiler" game.ds game.c

echo "game.c generated from game.ds"
