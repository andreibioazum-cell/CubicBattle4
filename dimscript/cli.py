"""Command line: python -m dimscript file.ds [-o output.c]."""

import sys

from .compiler import DimScriptCompiler


def main():
    output = 'game/game.c'
    sources = []
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] in ('-o', '--output') and i + 1 < len(args):
            output = args[i + 1]
            i += 2
        else:
            sources.append(args[i])
            i += 1
    if not sources:
        print("Usage: python -m dimscript file.ds [-o output.c]", file=sys.stderr)
        sys.exit(2)
    ok = DimScriptCompiler().compile(sources, output)
    print(f"{output}: {'OK' if ok else 'FAILED'}")
    sys.exit(0 if ok else 1)
