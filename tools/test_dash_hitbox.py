#!/usr/bin/env python3
"""Checks that the dash hitbox is drawn again when the hitbox overlay is on.

The dash zone used to be a grid of world cells that looked like a rendering bug,
so it was switched off entirely - and the dash lost its hitbox. Now the zone is
one strip along the part of the dash already traveled, as wide as the damage
zone is, drawn with the same colour and alpha as every other hitbox.

The test compiles the real game scripts (game/game.c) with the host geometry and
stub runtime, draws ability hitboxes for a player dash, a bot dash and a dash of
a remote player, and counts the recorded draw commands. Run from the repository
root after python3 gen.py:

    python3 tools/test_dash_hitbox.py
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT))
import ui_preview  # noqa: E402  (autostubs and the real rasteriser)
import frame_bench  # noqa: E402  (command recording and the real geometry)

MAIN = r"""
/* Counts hitbox commands: hitbox_rgb is black and hitbox_zone_alpha is 96, so
 * the zone colour is 0x60000000 exactly. */
static int n_rect, n_rot, n_circle, n_line, n_round;
static void count_hitboxes(void) {
    n_rect = n_rot = n_circle = n_line = n_round = 0;
    for (size_t k = 0; k < cmd_n; k++) {
        uint32_t c = 0; int hit = 1;
        switch (cmds[k].t) {
        case DS_CMD_RECT_ROT: c = cmds[k].v.rot.c; break;
        case DS_CMD_RECT:     c = cmds[k].v.rc.c; break;
        case DS_CMD_CIRCLE:   c = cmds[k].v.ci.c; break;
        case DS_CMD_LINE:     c = cmds[k].v.ln.c; break;
        case DS_CMD_ROUND:    c = cmds[k].v.rr.c; break;
        default: hit = 0; break;
        }
        if (!hit || c != 0x60000000u) continue;
        if (cmds[k].t == DS_CMD_RECT_ROT) n_rot++;
        else if (cmds[k].t == DS_CMD_RECT) n_rect++;
        else if (cmds[k].t == DS_CMD_CIRCLE) n_circle++;
        else if (cmds[k].t == DS_CMD_LINE) n_line++;
        else n_round++;
    }
}

static void check(int cond, const char *what) {
    if (cond) return;
    printf("FAIL: %s\n", what);
    exit(1);
}

int main(void) {
    screen_w = 1280; screen_h = 720;
    amgr = (AAssetManager *)&dummy_amgr_storage;
    ds_main();
    ds_fn_init();
    language = 1; show_hitboxes = 1;
    ds_fn_set_class_owned(CLASS_AZUM, 1);
    player_class = CLASS_AZUM;
    ds_fn_sync_selected_class();
    game_state = ST_SOLO;
    ds_fn_init_game();
    dt = 1.0 / 60.0;

    /* Player dash to the right: 400 px traveled, the whole 382.5 px path. */
    dash_active = 1; dash_box_a = 1;
    dash_x0 = 300; dash_y0 = 400; dash_dx = 1; dash_dy = 0;
    player->x = 700; player->y = 400;
    frame_open = 1; cmd_n = 0;
    ds_fn_draw();
    count_hitboxes();
    double pr = ds_fn_dash_hit_radius_solo();
    double path = azum_dash_speed * azum_dash_time;
    double travel = player->x - dash_x0;
    if (travel > path) travel = path;
    check(n_rot == 1, "рывок игрока: полосы зоны нет");
    check(n_rect == 0 && n_round == 0, "рывок игрока: клетки или скруглённые плиты");
    check(n_line == 0 && n_circle == 0, "рывок игрока: капсула или круг вместо полосы");
    int found = 0;
    for (size_t k = 0; k < cmd_n; k++) {
        if (cmds[k].t != DS_CMD_RECT_ROT || cmds[k].v.rot.c != 0x60000000u) continue;
        found = 1;
        check(fabs(cmds[k].v.rot.h - 2 * pr) < 1e-3, "ширина полосы не равна диаметру зоны урона");
        check(fabs(cmds[k].v.rot.y - (400 - pr)) < 1e-3, "полоса не по оси рывка");
        check(fabs((cmds[k].v.rot.x + cmds[k].v.rot.w / 2) - (dash_x0 + travel / 2)) < 1e-3,
              "полоса не по уже проеханному отрезку");
    }
    check(found, "рывок игрока: полоса не найдена");

    /* Bot dash in solo: same strip, drawn backwards. */
    dash_active = 0; dash_box_a = 0;
    enemy_dash_active = 1; edash_box_a = 1;
    enemy_dash_x0 = 1100; enemy_dash_y0 = 400; enemy_dash_dx = -1; enemy_dash_dy = 0;
    enemy->x = 800; enemy->y = 400;
    cmd_n = 0;
    ds_fn_draw();
    count_hitboxes();
    check(n_rot == 1 && n_rect == 0, "рывок бота: зона рисуется не одной полосой");
    enemy_dash_active = 0; edash_box_a = 0;

    /* Remote dash in online: the traveled part of the snapshot is drawn. */
    game_state = ST_ONLINE;
    /* The net_slot() stub returns 0, so slot 1 is a remote player. */
    int slot = 1;
    arr_set(rdash_box_a, slot, 1);
    arr_set(remote_dash, slot * dash_fields + 2, 300);
    arr_set(remote_dash, slot * dash_fields + 3, 200);
    arr_set(remote_dash, slot * dash_fields + 4, 1);
    arr_set(remote_dash, slot * dash_fields + 5, 0);
    arr_set(remote_dash, slot * dash_fields + 6, 0);
    cmd_n = 0;
    ds_fn_draw();
    count_hitboxes();
    check(n_rot == 1 && n_rect == 0, "рывок сетевого соперника: зона рисуется не одной полосой");

    /* Hitboxes off: no zone at all. */
    show_hitboxes = 0;
    cmd_n = 0;
    ds_fn_draw();
    count_hitboxes();
    check(n_rot == 0 && n_rect == 0 && n_circle == 0 && n_line == 0 && n_round == 0,
          "с выключенными хитбоксами зона всё равно рисуется");

    puts("рывок: зона урона рисуется одной полосой по проеханному отрезку (игрок, бот, соперник)");
    return 0;
}
"""


def build(temp: Path) -> Path:
    src = temp / "probe.c"
    stubs = temp / "stubs.c"
    binary = temp / "probe"
    src.write_text(ui_preview.STUBS + frame_bench.LAYER + MAIN, encoding="utf-8")
    stubs.write_text("#include <stdarg.h>\n#include <stdio.h>\n#include <string.h>\n", encoding="utf-8")
    protos = ui_preview.prototypes()
    cmd = [*ui_preview.CC, "-std=gnu99", "-O1", "-I", str(ROOT), "-I", str(ROOT / "game"),
           "-I", str(ROOT / "tools" / "host_test" / "stub"), str(src), str(stubs), "-lm",
           "-o", str(binary)]
    done: set[str] = set()
    for _ in range(10):
        run = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
        missing = sorted(set(re.findall(r"undefined reference to `(\w+)'", run.stderr)))
        if not missing:
            if run.returncode == 0:
                return binary
            sys.exit("сборка теста не удалась:\n" + run.stderr)
        lines = ["/* Autostubs: signatures from runtime.h and net.h, neutral bodies. */",
                 "#include <stdarg.h>", "#include <stdio.h>"]
        unknown = []
        for name in missing:
            if name in done:
                continue
            if name in ui_preview.VAR_DECLS:
                lines.append(ui_preview.VAR_DECLS[name])
            elif name in protos:
                lines.append(ui_preview.stub_source(name, *protos[name]))
            else:
                unknown.append(name)
                continue
            done.add(name)
        if unknown:
            sys.exit("нет прототипов для заглушек: " + ", ".join(unknown))
        stubs.write_text("\n".join(lines) + "\n", encoding="utf-8")
    sys.exit("не удалось слинковать тест за 10 итераций")


def main() -> int:
    game_c = ROOT / "game" / "game.c"
    if not game_c.exists():
        sys.exit("game/game.c is missing, run python3 gen.py first")
    with tempfile.TemporaryDirectory(prefix="dash-hitbox-") as td:
        binary = build(Path(td))
        run = subprocess.run([str(binary)], capture_output=True, text=True, cwd=str(ROOT))
        sys.stderr.write(run.stderr)
        sys.stdout.write(run.stdout)
        return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
