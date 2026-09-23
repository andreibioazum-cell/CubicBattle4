"""Generate C code for a DimScript game project."""
import os
import sys
from ds_compiler import DimScriptCompiler


def find_ds_files(directory):
    """Collects every .ds file recursively and sorts it by module.

    The order matters for global declarations: objects and their instances must
    come before the first use. The files inside game/scripts are grouped by
    module, and the module order is pinned here: configuration and state first,
    battle and effects after, with the engine, the main loop, closing the list.
    """
    order = [
        "core/config.ds",          # constants: screens, theme, layout, balance
        "ui/locale_core.ds",       # main RU/EN strings
        "ui/locale_progress.ds",   # progress, reward and shop strings
        "ui/locale_extra.ds",      # quests and the top list
        "core/entities.ds",        # objects and battle state
        "core/ui.ds",              # UI kit: buttons, cards, hit tests, text
        "ui/progress_classes.ds",  # classes, levels and skins
        "ui/progress_rewards.ds",  # rewards, saving and syncing
        "ui/promo.ds",             # solo cards and the promo screen
        "ui/layout.ds",            # screen geometry, draw_* and touch_*
        "ui/chat.ds",              # online chat
        "ui/menu_screens.ds",      # menu screen drawing
        "ui/quests.ds",            # quests
        "ui/menu_input.ds",        # menu navigation and taps
        "combat/battle_rules.ds",  # class data: textures, HP, damage, poison
        "combat/battle_turrets.ds", # the buk dispenser turrets
        "combat/battle_hitscan.ds", # hit geometry of punches and dashes
        "combat/battle_damage.ds", # damage, healing, stun, revive
        "combat/battle_setup.ds",  # battle start, spawning and player movement
        "combat/battle_movement.ds", # bot aiming and movement
        "combat/battle_ai.ds",     # bot decisions and attacks
        "combat/battle_status.ds", # freeze, stun and poison
        "combat/battle_enemy_class.ds", # the random enemy class and its supers
        "combat/battle_enemy_dash.ds", # enemy dash and the solo player dash
        "combat/battle_enemy_turrets.ds", # enemy buk shield turrets
        "combat/battle_shield.ds", # buk turret shield and snowflake piercing
        "combat/battle_abilities.ds", # abilities and the main battle update
        "combat/battle_online.ds", # network snapshots and match finish
        "combat/battle_actions_fx.ds", # attack start and abilities
        "combat/battle_super_render.ds", # drawing turrets, the beam and the universe
        "combat/battle_hitboxes.ds", # damage hitboxes of abilities
        "combat/battle_hitbox_fades.ds", # smooth alphas of those hitboxes
        "combat/battle_render.ds", # arena, fighters and battle HUD
        "combat/battle_event_plates.ds", # the plates and Santa event
        "combat/battle_events_input.ds", # events, banners and input
        "fx/weather.ds",           # arena background and the snow effect
        "fx/dust.ds",              # dust trail
        "core/engine.ds",          # main loop
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
        # list in find_ds_files is relative to that folder. A project root such as
        # game is therefore redirected to its scripts subdirectory, otherwise the
        # module order would not match order in find_ds_files.
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
