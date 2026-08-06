#!/usr/bin/env python3
"""
Game DS build script - compiles all .ds files in a game directory.
"""

import sys
import os
import glob
from pathlib import Path
from ds_compiler import Compiler

def find_ds_files(directory: str) -> list:
    """Return sorted list of .ds files in directory (non-recursive)."""
    pattern = os.path.join(directory, '*.ds')
    return sorted(glob.glob(pattern))

def main():
    GAME_DIR = 'game'
    COMPAT_BUILD = False

    # Parse arguments
    if len(sys.argv) == 1:
        # Default: game/ -> game/game.c
        input_path = GAME_DIR
        output_path = os.path.join(GAME_DIR, 'game.c')
        COMPAT_BUILD = True
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

    # Compatibility: copy files to repository root if game directory was used
    if COMPAT_BUILD:
        try:
            # Copy game.c to root
            root_game_c = 'game.c'
            if os.path.abspath(output_path) != os.path.abspath(root_game_c):
                import shutil
                shutil.copy2(output_path, root_game_c)
                print(f"Copied {output_path} -> {root_game_c} for compatibility")

            # Copy AndroidManifest.xml if present
            manifest_src = os.path.join(input_path, 'AndroidManifest.xml')
            if os.path.isfile(manifest_src):
                shutil.copy2(manifest_src, 'AndroidManifest.xml')
                print(f"Copied {manifest_src} -> AndroidManifest.xml")
            else:
                print(f"Warning: {manifest_src} not found", file=sys.stderr)

            # Copy assets if present
            assets_src = os.path.join(input_path, 'assets')
            if os.path.isdir(assets_src):
                staging = 'staging/assets'
                os.makedirs(staging, exist_ok=True)
                # copy recursively
                for root, dirs, files in os.walk(assets_src):
                    rel = os.path.relpath(root, assets_src)
                    dest_root = os.path.join(staging, rel)
                    os.makedirs(dest_root, exist_ok=True)
                    for f in files:
                        src_file = os.path.join(root, f)
                        dst_file = os.path.join(dest_root, f)
                        shutil.copy2(src_file, dst_file)
                print(f"Copied assets to {staging}")
        except Exception as e:
            print(f"Compatibility copy failed: {e}", file=sys.stderr)

    sys.exit(0)

if __name__ == '__main__':
    main()
