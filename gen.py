"""Generate C code for a DimScript game project."""
import os
import sys
from dimscript import DimScriptCompiler


MODULES_FILE = 'modules.txt'


def read_module_order(directory):
    """Module order of a game: <scripts>/modules.txt, one path per line.

    The game owns its module list, so adding or splitting a game module never
    touches the compiler or this driver. Blank lines and text after # are
    ignored; a missing file means plain alphabetical order.
    """
    path = os.path.join(directory, MODULES_FILE)
    if not os.path.isfile(path):
        return []
    order = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            rel = line.split('#', 1)[0].strip().replace('\\', '/')
            if rel and rel not in order:
                order.append(rel)
    return order


def find_ds_files(directory):
    """Collects every .ds file recursively and sorts it by module.

    The order matters for global declarations: objects and their instances must
    come before the first use. The game pins it in game/scripts/modules.txt:
    configuration and state first, battle and effects after, with the engine,
    the main loop, closing the list.
    """
    order = read_module_order(directory)
    files = []
    for root, _dirs, names in os.walk(directory):
        for n in names:
            if n.endswith('.ds'):
                files.append(os.path.join(root, n))

    def key(p):
        rel = os.path.relpath(p, directory).replace(os.sep, '/')
        try:
            return (0, order.index(rel))
        except ValueError:
            # Unknown files, experimental ones for instance, come after every
            # module, in alphabetical order.
            return (1, rel)

    return sorted(files, key=key)


def run_lint(scripts_dir):
    """Runs tools/ds_lint.py over the scripts that were just compiled.

    The DimScript compiler silently drops assignments to undeclared names, so
    without the lint a typo in menu_input.ds would vanish from game.c without a
    trace and only show up in the built game. By default the lint only prints
    what it finds; DS_LINT_STRICT=1 makes gen.py fail instead, which is worth
    turning on in CI.
    """
    if not scripts_dir:
        return []
    try:
        from tools import ds_lint
    except Exception as e:  # the lint must not break generation
        print(f"ds_lint skipped: {e}")
        return []
    errors = ds_lint.lint_dir(scripts_dir)
    if errors:
        print(f"ds_lint: {len(errors)} problem(s)")
        for e in errors:
            print(" -", e)
    else:
        print("ds_lint: ok")
    return errors


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
        src_dir = input_path
        # In this project the scripts live in <game_dir>/scripts, and the module
        # list in modules.txt is relative to that folder. A project root such as
        # game is therefore redirected to its scripts subdirectory, otherwise the
        # module order would not match modules.txt.
        scripts = os.path.join(input_path, 'scripts')
        if os.path.isdir(scripts):
            src_dir = scripts
        sources = find_ds_files(src_dir)
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
    if hasattr(compiler, 'warnings') and compiler.warnings:
        note = f" with {compiler.warnings} warning(s)"
    if compiler.errors:
        note += f" (errors above are non-fatal: game still builds)"
    print(f"{output_path} generated from {len(compiler.lines)} line(s){note}")

    lint_errors = run_lint(src_dir if os.path.isdir(input_path) else None)
    if lint_errors and os.environ.get('DS_LINT_STRICT', '').lower() in ('1', 'true', 'yes'):
        return 1

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
