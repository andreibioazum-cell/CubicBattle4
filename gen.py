#!/usr/bin/env python3
"""
Game DS build script - compiles all .ds files in a game directory.
"""

import sys
import os
import glob
from ds_compiler import Compiler

def find_ds_files(directory: str) -> list:
    """Return sorted list of .ds files in directory (non-recursive)."""
    pattern = os.path.join(directory, '*.ds')
    return sorted(glob.glob(pattern))

def main():
    GAME_DIR = 'game'

    # Parse arguments
    if len(sys.argv) == 1:
        # Default: game/ -> game/game.c
        input_path = GAME_DIR
        output_path = os.path.join(GAME_DIR, 'game.c')
    elif len(sys.argv) == 2:
        if os.path.isdir(sys.argv[1]):
            input_path = sys.argv[1]
            output_path = os.path.join(input_path, 'game.c')
        else:
            input_path = sys.argv[1]
            output_path = os.path.splitext(input_path)[0] + '.c'
    elif len(sys.argv) == 3:
        if os.path.isdir(sys.argv[1]):
            input_path = sys.argv[1]
            output_path = sys.argv[2]
        else:
            input_path = sys.argv[1]
            output_path = sys.argv[2]
    else:
        print("Usage: python gen.py [game-directory [output.c]] | [source.ds output.c]", file=sys.stderr)
        sys.exit(2)

    # Collect sources
    if os.path.isdir(input_path):
        sources = find_ds_files(input_path)
        if not sources:
            print(f"Error: no .ds files found in {input_path}", file=sys.stderr)
            sys.exit(1)
    else:
        if not os.path.isfile(input_path):
            print(f"Error: file not found: {input_path}", file=sys.stderr)
            sys.exit(1)
        sources = [input_path]

    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)

    # Compile
    comp = Compiler()
    success = comp.compile(sources, output_path)

    if not success:
        print(f"Compilation failed with {comp.errors} errors", file=sys.stderr)
        sys.exit(1)

    print(f"{output_path} generated from {len(sources)} DimScript file(s)")
    sys.exit(0)

if __name__ == '__main__':
    main()
