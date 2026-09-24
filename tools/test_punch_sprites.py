#!/usr/bin/env python3
"""Punch and dash hits register on their closing frames, and the zombie skin
never mixes two creatures in one fighter.

On a cold start the frame time spikes to the 0.1 s clamp. The swing window is
0.2 s, and the timers used to tick down before the hit test and zero the window
inside its own last frame, so the hit test (which ran only while punch_left>0)
skipped the closing frame and the punch could flash its pose without ever
registering. The dash had the same hole through tick_dash closing the flight
before tick_dash_solo_hit ran. Both now resolve first and close after.

A texture that failed to decode used to leave the zombie skin half there: the
idle fell back to the class cube while the swing pose of the brain popped up on
a hit. The zombie pair now loads as a pair or not at all.

Run from the repository root after python3 gen.py:

    python3 tools/test_punch_sprites.py
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT))
import ui_preview  # noqa: E402
import frame_bench  # noqa: E402

MAIN = r"""
/* The sprite loads of this test: the brain idle misses its first three asks,
 * like a decode hiccup on a cold start, and everything else lands at once. */
static int zombie_asks = 0;
int png_load(const char *name) {
    if (name && !strcmp(name, "zombie_azum.png") && zombie_asks < 3) {
        zombie_asks++;
        return 0;
    }
    return 1;
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
    language = 1;
    ds_fn_set_class_owned(CLASS_AZUM, 1);
    player_class = CLASS_AZUM;
    azum_skin = SKIN_ZOMBIE;
    ds_fn_sync_selected_class();
    game_state = ST_SOLO;
    ds_fn_init_game();
    dt = 1.0 / 60.0;

    /* A point blank punch resolves and damages. */
    player->x = 500; player->y = 400; player->angle = 0;
    enemy->x = 560; enemy->y = 400;
    double ehp = enemy->hp;
    ds_fn_start_punch_now();
    ds_fn_update_game();
    printf("punch: enemy hp %.1f -> %.1f, punch.hit=%g punch_left=%.3f\n",
           ehp, enemy->hp, punch->hit, punch_left);
    check(enemy->hp < ehp && punch->hit == 1, "a point blank punch does not damage the enemy");

    /* The closing frame of a swing resolves: the window is about to run out
     * inside this very frame and the hit must still be tested. */
    ds_fn_reset_battle();
    player->x = 500; player->y = 400; player->angle = 0;
    enemy->x = 560; enemy->y = 400;
    ehp = enemy->hp;
    ds_fn_start_punch_now();
    punch_left = 0.0001;
    ds_fn_update_game();
    check(enemy->hp < ehp, "the closing frame of a swing does not hit");

    /* The swing dies with its window: punch.active must not stay on. */
    ds_fn_reset_battle();
    ds_fn_start_punch_now();
    for (int i = 0; i < 30; i++) ds_fn_update_game();
    check(punch_left == 0 && punch->active == 0, "the swing did not close after its window");

    /* The closing frame of the dash resolves the same way. */
    ds_fn_reset_battle();
    player->x = 500; player->y = 400; player->angle = 0;
    enemy->x = 505; enemy->y = 400;
    ehp = enemy->hp;
    ds_fn_start_dash_now();
    dash_t = 0.0001;
    ds_fn_update_game();
    printf("dash: enemy hp %.1f -> %.1f, dash.hit=%g\n", ehp, enemy->hp, dash_hit);
    check(enemy->hp < ehp, "the closing frame of a dash does not hit");

    /* The zombie pair loads as a pair: the brain sprites appear only when both
     * halves are there, so one fighter never mixes two creatures. */
    azum_tex_ok = 1; azum_punch_tex_ok = 1; ordinary_punch_tex_ok = 1;
    azum_zombie_tex_ok = 1; azum_zombie_punch_tex_ok = 1;
    const char *s0 = ds_fn_fighter_sprite(CLASS_AZUM, 0, SKIN_ZOMBIE);
    const char *s1 = ds_fn_fighter_sprite(CLASS_AZUM, 1, SKIN_ZOMBIE);
    printf("zombie pair: idle = %s ; punch = %s\n", s0, s1);
    check(!strcmp(s0, "zombie_azum.png"), "the zombie idle sprite is not the brain");
    check(!strcmp(s1, "zombie_azum_punch.png"), "the zombie swing sprite is not the brain");

    azum_zombie_punch_tex_ok = 0;
    s0 = ds_fn_fighter_sprite(CLASS_AZUM, 0, SKIN_ZOMBIE);
    s1 = ds_fn_fighter_sprite(CLASS_AZUM, 1, SKIN_ZOMBIE);
    printf("brain swing missing: idle = %s ; punch = %s\n", s0, s1);
    check(strstr(s0, "zombie") == NULL && strstr(s1, "zombie") == NULL,
          "a missing brain sprite mixes the creatures");
    check(!strcmp(s0, "azum.png") && !strcmp(s1, "azum_punch.png"),
          "the fallback of a broken pair is not the class pair");

    azum_zombie_tex_ok = 0; azum_zombie_punch_tex_ok = 1;
    s0 = ds_fn_fighter_sprite(CLASS_AZUM, 0, SKIN_ZOMBIE);
    s1 = ds_fn_fighter_sprite(CLASS_AZUM, 1, SKIN_ZOMBIE);
    printf("brain idle missing: idle = %s ; punch = %s\n", s0, s1);
    check(strstr(s0, "zombie") == NULL && strstr(s1, "zombie") == NULL,
          "a missing brain idle mixes in the brain swing on a hit");
    check(!strcmp(s0, "azum.png") && !strcmp(s1, "azum_punch.png"),
          "the fallback of a broken pair is not the class pair");

    /* Every sprite missing: one uniform ordinary pair, never nothing. */
    azum_tex_ok = 0; azum_punch_tex_ok = 0; ordinary_punch_tex_ok = 0;
    s0 = ds_fn_fighter_sprite(CLASS_AZUM, 0, SKIN_ZOMBIE);
    s1 = ds_fn_fighter_sprite(CLASS_AZUM, 1, SKIN_ZOMBIE);
    printf("everything missing: idle = %s ; punch = %s\n", s0, s1);
    check(!strcmp(s0, "ordinary.png") && !strcmp(s1, "ordinary.png"),
          "with every sprite missing the fighter is not one uniform pair");

    /* A sprite whose first loads hiccuped must be asked again: the first answer
     * latches the flags and the fallback cubes used to stay for the whole
     * session. reload_textures retries the failed loads until they land. */
    azum_tex_ok = 1; azum_punch_tex_ok = 1; ordinary_punch_tex_ok = 1;
    santa_punch_tex_ok = 1; ebuc_punch_tex_ok = 1; candy_tex_ok = 1;
    azum_zombie_tex_ok = 0; azum_zombie_punch_tex_ok = 1;
    zombie_asks = 0; /* three misses again */
    tex_reload_left = 60; tex_reload_t = 0;
    for (int i = 0; i < 60 && azum_zombie_tex_ok == 0; i++) {
        tex_reload_t = 0; /* the half second between the asks is up */
        ds_fn_reload_textures();
    }
    printf("reloaded after the hiccups: zombie idle ok = %g, misses = %d\n",
           azum_zombie_tex_ok, zombie_asks);
    check(azum_zombie_tex_ok == 1, "a failed sprite load is never asked again");
    check(zombie_asks == 3, "the retry does not keep asking while the load misses");
    s0 = ds_fn_fighter_sprite(CLASS_AZUM, 0, SKIN_ZOMBIE);
    check(!strcmp(s0, "zombie_azum.png"), "the healed sprite is still not used");
    check(tex_reload_left == 0, "the retries keep asking after every load landed");

    puts("punch and dash close with their windows, closing frames hit, sprites never mix");
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
    for _ in range(12):
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
    sys.exit("could not link the test in 12 rounds of auto stubs")


def main() -> int:
    game_c = ROOT / "game" / "game.c"
    if not game_c.exists():
        sys.exit("game/game.c is missing, run python3 gen.py first")
    with tempfile.TemporaryDirectory(prefix="punch-sprites-") as td:
        binary = build(Path(td))
        run = subprocess.run([str(binary)], capture_output=True, text=True, cwd=str(ROOT))
        sys.stderr.write(run.stderr)
        sys.stdout.write(run.stdout)
        return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
