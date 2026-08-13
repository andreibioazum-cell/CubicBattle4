#!/usr/bin/env python3
"""Generate C code for a DimScript game project."""

import glob
import os
import sys

from ds_compiler import DimScriptCompiler


def find_ds_files(directory):
    files = glob.glob(os.path.join(directory, '*.ds'))
    # Новый порядок без цифр: config -> entities -> ui -> menu -> battle -> engine
    # Если старые файлы с цифрами — сортируем как раньше.
    order = ["config.ds", "entities.ds", "ui.ds", "menu.ds", "battle.ds", "engine.ds",
             "core.ds", "game.ds"]
    # Если есть файлы без цифр в имени, используем order
    has_plain = any(os.path.basename(f) in order or not os.path.basename(f)[0].isdigit() for f in files)
    if has_plain:
        def key(p):
            b = os.path.basename(p)
            try:
                idx = order.index(b)
                return (0, idx)
            except ValueError:
                # остальные после ordered, по алфавиту
                return (1, b)
        return sorted(files, key=key)
    return sorted(files)


def usage(stream=sys.stdout):
    print(
        "Usage: python gen.py [--dump] "
        "[game-directory [output.c]] | [source.ds output.c]",
        file=stream,
    )


def main():
    game_dir = 'game'
    args = sys.argv[1:]
    dump_c = os.environ.get('DIMSCRIPT_DUMP_C', '').lower() in ('1', 'true', 'yes')

    if '--help' in args or '-h' in args:
        usage()
        return 0
    if '--dump' in args:
        dump_c = True
        args = [arg for arg in args if arg != '--dump']

    if len(args) == 0:
        input_path = game_dir
        output_path = os.path.join(game_dir, 'game.c')
    elif len(args) == 1:
        input_path = args[0]
        if os.path.isdir(input_path):
            output_path = os.path.join(input_path, 'game.c')
        else:
            output_path = os.path.splitext(input_path)[0] + '.c'
    elif len(args) == 2:
        input_path, output_path = args
    else:
        usage(sys.stderr)
        return 2

    if os.path.isdir(input_path):
        sources = find_ds_files(input_path)
        if not sources:
            print(f"Error: no .ds files found in {input_path}", file=sys.stderr)
            return 1
    else:
        if not os.path.isfile(input_path):
            print(f"Error: file not found: {input_path}", file=sys.stderr)
            return 1
        sources = [input_path]

    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)

    compiler = DimScriptCompiler()
    if not compiler.compile(sources, output_path):
        print("Compilation failed", file=sys.stderr)
        return 1

    note = ""
    if compiler.warnings:
        note = f" with {compiler.warnings} warning(s)"
    if compiler.errors:
        note += f" (errors above are non-fatal: game still builds)"
    print(f"{output_path} generated from {len(compiler.loaded_sources)} DimScript file(s){note}")

    if dump_c:
        print("\n" + "=" * 60)
        print("GENERATED C CODE:")
        print("=" * 60)
        with open(output_path, 'r', encoding='utf-8') as generated:
            print(generated.read())
        print("=" * 60)
        print("END OF GENERATED C CODE")
        print("=" * 60 + "\n")

    return 0


if __name__ == '__main__':
    sys.exit(main())
