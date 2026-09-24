#!/usr/bin/env python3
"""Checks the dash as it is again: a straight locked-direction flight whose
hitbox zone is one strip over the travelled part, fading out in place.

The dash used to follow the stick mid-flight (steering at dash speed) and the
zone was drawn from the start to wherever the fighter now was, so it turned
into hitbox remnants that stretched after him. The dash is again a burst in the
direction locked at the start (the stick only turns his face), the strip ends
where the flight ended - dash_travel freezes at the dash end - and the zone for
the bot and for a remote player works the same way.

The test compiles the real game scripts (game/game.c) with the host geometry and
stub runtime. It checks the steering lock through move_player itself and counts
the recorded draw commands of the zone strips. Run from the repository root
after python3 gen.py:

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
    frame_open = 1;

    /* The flight is a burst in the direction locked at the start: with the
     * stick pulled down while dashing right the direction never turns, the
     * fighter never slides sideways and dash_travel matches the flight. */
    ds_fn_reset_battle();
    player->x = 500; player->y = 400; player->angle = 0;
    joy.dx = 0; joy.dy = 1;
    ds_fn_start_dash_now();
    for (int i = 0; i < 20; i++) { ds_fn_move_player(); ds_fn_tick_dash(); }
    printf("dash with the stick down: dir=(%.2f,%.2f) pos=(%.0f,%.0f) travel=%.1f\n",
           dash_dx, dash_dy, player->x, player->y, dash_travel);
    check(fabs(dash_dx - 1) < 1e-6 && fabs(dash_dy) < 1e-6,
          "the dash direction turned mid-flight (steering)");
    check(fabs(player->y - 400) < 1e-6,
          "the dash slid sideways with the stick (steering)");
    check(fabs((player->x - dash_x0) - dash_travel) < 1e-6,
          "dash_travel does not match the flight");
    check(fabs(dash_travel - 20 * azum_dash_speed * dt) < 1e-3,
          "the dash does not fly at the locked dash speed");
    /* The flight closes with its window and dash_travel freezes at its end. */
    int frames = 20;
    while (dash_active > 0 && frames < 60) { ds_fn_move_player(); ds_fn_tick_dash(); frames++; }
    check(dash_active == 0, "the dash did not close with its window");
    check(fabs(dash_travel - azum_dash_speed * azum_dash_time) < 0.5,
          "dash_travel did not freeze at the end of the flight");
    joy.dx = 0; joy.dy = 0;

    /* The zone is one strip over the travelled part of the flight. The fighter
     * has walked on past the dash end: the strip still ends where the flight
     * ended and does not stretch after him. */
    dash_active = 1; dash_box_a = 1;
    dash_x0 = 300; dash_y0 = 400; dash_dx = 1; dash_dy = 0;
    dash_travel = 382.5;
    player->x = 900; player->y = 400;
    cmd_n = 0;
    ds_fn_draw();
    count_hitboxes();
    double pr = ds_fn_dash_hit_radius_solo();
    check(n_rot == 1, "player dash: no zone strip");
    check(n_rect == 0 && n_round == 0, "player dash: cells or rounded plates");
    check(n_line == 0 && n_circle == 0, "player dash: capsule or circle instead of a strip");
    int found = 0;
    for (size_t k = 0; k < cmd_n; k++) {
        if (cmds[k].t != DS_CMD_RECT_ROT || cmds[k].v.rot.c != 0x60000000u) continue;
        found = 1;
        check(fabs(cmds[k].v.rot.h - 2 * pr) < 1e-3, "strip width is not the damage diameter");
        check(fabs(cmds[k].v.rot.y - (400 - pr)) < 1e-3, "strip is off the dash axis");
        check(fabs(cmds[k].v.rot.w - dash_travel) < 1e-3, "strip length is not the travelled length");
        check(fabs((cmds[k].v.rot.x + cmds[k].v.rot.w / 2) - (dash_x0 + dash_travel / 2)) < 1e-3,
              "strip does not cover the travelled segment");
        check(cmds[k].v.rot.x + cmds[k].v.rot.w < player->x - 1,
              "strip stretches after the fighter (remnant)");
    }
    check(found, "player dash: strip not found");

    /* Fade-out in place: the flight is over and the fighter has walked far
     * away, the fading zone must stay frozen where the flight ended. */
    dash_active = 0; dash_box_a = 1;
    player->x = 1100; player->y = 430;
    cmd_n = 0;
    ds_fn_draw();
    count_hitboxes();
    check(n_rot == 1, "dash fade: no zone strip");
    found = 0;
    for (size_t k = 0; k < cmd_n; k++) {
        if (cmds[k].t != DS_CMD_RECT_ROT || cmds[k].v.rot.c != 0x60000000u) continue;
        found = 1;
        check(fabs(cmds[k].v.rot.w - dash_travel) < 1e-3, "dash fade: the strip changed length");
        check(fabs((cmds[k].v.rot.x + cmds[k].v.rot.w / 2) - (dash_x0 + dash_travel / 2)) < 1e-3,
              "dash fade: the strip end moved with the fighter");
    }
    check(found, "dash fade: strip not found");

    /* Bot dash in solo: the same strip backwards from his frozen end. */
    dash_active = 0; dash_box_a = 0;
    enemy_dash_active = 1; edash_box_a = 1;
    enemy_dash_x0 = 1100; enemy_dash_y0 = 400; enemy_dash_dx = -1; enemy_dash_dy = 0;
    enemy_dash_travel = 292.5;
    enemy->x = 807.5; enemy->y = 400;
    cmd_n = 0;
    ds_fn_draw();
    count_hitboxes();
    check(n_rot == 1 && n_rect == 0, "bot dash: the zone is not one strip");
    found = 0;
    for (size_t k = 0; k < cmd_n; k++) {
        if (cmds[k].t != DS_CMD_RECT_ROT || cmds[k].v.rot.c != 0x60000000u) continue;
        found = 1;
        check(fabs(cmds[k].v.rot.w - enemy_dash_travel) < 1e-3,
              "bot dash: strip length is not his travelled length");
        check(fabs((cmds[k].v.rot.x + cmds[k].v.rot.w / 2) - (1100 - enemy_dash_travel / 2)) < 1e-3,
              "bot dash: strip does not cover the travelled part backwards");
    }
    check(found, "bot dash: strip not found");
    enemy_dash_active = 0; edash_box_a = 0;

    /* Remote dash in online: the travelled part of the snapshot is drawn. */
    game_state = ST_ONLINE;
    /* The net_slot() stub returns 0, so slot 1 is a remote player. */
    int slot = 1;
    double rtravel = 250;
    arr_set(rdash_box_a, slot, 1);
    arr_set(remote_dash, slot * dash_fields + 2, 300);
    arr_set(remote_dash, slot * dash_fields + 3, 200);
    arr_set(remote_dash, slot * dash_fields + 4, 1);
    arr_set(remote_dash, slot * dash_fields + 5, 0);
    arr_set(remote_dash, slot * dash_fields + 6, 0);
    /* The leg the remote fighter is on, as update_remote_dashes reads it from the
     * room snapshot. */
    arr_set(remote_dash, slot * dash_fields + 8, rtravel);
    cmd_n = 0;
    ds_fn_draw();
    count_hitboxes();
    check(n_rot == 1 && n_rect == 0, "remote dash: the zone is not one strip");
    found = 0;
    for (size_t k = 0; k < cmd_n; k++) {
        if (cmds[k].t != DS_CMD_RECT_ROT || cmds[k].v.rot.c != 0x60000000u) continue;
        found = 1;
        check(fabs(cmds[k].v.rot.w - rtravel) < 1e-3, "remote dash: strip length is not the leg");
        check(fabs((cmds[k].v.rot.x + cmds[k].v.rot.w / 2) - (300 + rtravel / 2)) < 1e-3,
              "remote dash: strip does not cover the leg from the room snapshot");
    }
    check(found, "remote dash: strip not found");

    /* Hitboxes off: no zone at all. */
    show_hitboxes = 0;
    cmd_n = 0;
    ds_fn_draw();
    count_hitboxes();
    check(n_rot == 0 && n_rect == 0 && n_circle == 0 && n_line == 0 && n_round == 0,
          "the zone is drawn even with hitboxes off");

    puts("dash: straight locked flight, one strip that fades out in place (player, bot, remote)");
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
            sys.exit("failed to build the test:\n" + run.stderr)
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
            sys.exit("no prototypes for these stubs: " + ", ".join(unknown))
        stubs.write_text("\n".join(lines) + "\n", encoding="utf-8")
    sys.exit("could not link the test in 10 rounds of auto stubs")


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
