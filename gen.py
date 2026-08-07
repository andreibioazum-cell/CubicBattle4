#!/usr/bin/env python3
"""
Game DS build script - показывает сгенерированный C-код в логах
"""

import sys
import os
import glob
from ds_compiler import DimScriptCompiler

def find_ds_files(directory):
    pattern = os.path.join(directory, '*.ds')
    return sorted(glob.glob(pattern))

def main():
    GAME_DIR = 'game'

    if len(sys.argv) == 1:
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
        print("Usage: python gen.py [game-directory [output.c]] | [source.ds output.c]")
        sys.exit(2)

    if os.path.isdir(input_path):
        sources = find_ds_files(input_path)
        if not sources:
            print(f"Error: no .ds files found in {input_path}")
            sys.exit(1)
    else:
        if not os.path.isfile(input_path):
            print(f"Error: file not found: {input_path}")
            sys.exit(1)
        sources = [input_path]

    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)

    comp = DimScriptCompiler()
    success = comp.compile(sources, output_path)

    if not success:
        print(f"Compilation failed")
        sys.exit(1)

    print(f"{output_path} generated from {len(sources)} DimScript file(s)")
    
    # ВЫВОДИМ СГЕНЕРИРОВАННЫЙ C-КОД В ЛОГИ
    print("\n" + "="*60)
    print("GENERATED C CODE:")
    print("="*60)
    try:
        with open(output_path, 'r') as f:
            c_code = f.read()
            print(c_code)
    except Exception as e:
        print(f"Error reading generated file: {e}")
    print("="*60)
    print("END OF GENERATED C CODE")
    print("="*60 + "\n")

    sys.exit(0)

if __name__ == '__main__':
    main()
