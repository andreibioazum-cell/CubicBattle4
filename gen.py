#!/usr/bin/env python3
import glob, os, sys
from ds_compiler import DimScriptCompiler

def main():
    game_dir = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else 'game'
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(game_dir, 'game.c')
    order = ["config.ds", "entities.ds", "ui.ds", "chat.ds", "menu.ds", "battle.ds", "engine.ds"]
    files = glob.glob(os.path.join(game_dir, '*.ds'))
    sources = sorted(files, key=lambda f: (0, order.index(os.path.basename(f))) if os.path.basename(f) in order else (1, f))
    if not sources:
        print(f"Error: no .ds files in {game_dir}", file=sys.stderr); return 1
    c = DimScriptCompiler()
    if not c.compile(sources, out): return 1
    print(f"{out} generated from {len(c.lines)} line(s)")
    return 0

if __name__ == '__main__':
    sys.exit(main())
