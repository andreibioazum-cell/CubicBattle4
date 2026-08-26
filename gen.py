"""Generate C code for a DimScript game project."""
import os
import sys
from ds_compiler import DimScriptCompiler


def find_ds_files(directory):
    """Рекурсивно собирает все .ds файлы и сортирует их по модулям.

    Порядок важен для глобальных объявлений (объекты и их экземпляры должны
    идти до первого использования), поэтому файлы внутри game/scripts лежат по
    модулям, а порядок модулей зафиксирован здесь: конфигурация и состояние —
    раньше, бой и эффекты — позже, движок (главный цикл) замыкает список.
    """
    order = [
        "core/config.ds",
        "ui/locale.ds",
        "core/entities.ds",
        "core/ui.ds",
        "ui/chat.ds",
        "ui/menu.ds",
        "combat/battle.ds",
        "fx/dust.ds",
        "core/engine.ds",
    ]
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
            # Незнакомые файлы (например, экспериментальные) идут после всех
            # модулей, по алфавиту.
            return (1, rel)

    return sorted(files, key=key)


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
        # В проекте скрипты лежат в <game_dir>/scripts, а список модулей в
        # find_ds_files отсчитывается именно от этой папки. Если передан корень
        # проекта (например, game), ищем скрипты в его подкаталоге scripts,
        # иначе порядок модулей не совпадёт с order в find_ds_files.
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
