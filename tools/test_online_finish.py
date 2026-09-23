#!/usr/bin/env python3
"""Checks when an online fight may be declared won.

The bug this guards: a win was declared the moment nobody in the room read as
alive, and a network hitch makes every remote read as gone at once. The fight was
then won in the middle of a battle, on the winning side, with the other fighters
vanishing from the arena - and leaving that results screen froze the game.

The rules now: the room has to answer normally (net_status() == NET_PLAYING), a
room that still lists somebody needs only a short confirmation before the win
(a reconnecting client is listed dead for a tick or two), and a room with nobody
in it has to stay empty for foes_gone_min seconds.

The test compiles the real game scripts (game/game.c) with the host geometry and
stub runtime, drives check_online_finish with a scripted room and counts the
seconds. Run from the repository root after python3 gen.py:

    python3 tools/test_online_finish.py
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
/* The room as this client reads it: the status of the connection, and the players
 * it lists. Slot 0 is this client, so the fighters in the room are slots 1..3. */
static double stub_status = NET_PLAYING;
static double stub_online[4] = {1, 0, 0, 0};
static double stub_alive[4] = {0, 0, 0, 0};

/* net_slot() is 0 in the shared stubs, which makes slot 0 this client. */
double net_status(void) { return stub_status; }
double net_player_online(double s) {
    int i = (int)s;
    return i >= 0 && i < 4 ? stub_online[i] : 0;
}

static void check(int cond, const char *what) {
    if (cond) return;
    printf("FAIL: %s\n", what);
    exit(1);
}

/* Puts the fight in a known state: one foe in the room, listed as online or not,
 * alive or not, with the match already seen or not. */
static void reset_case(double status, double online, double alive, double seen) {
    finished = 0;
    online_ready = 1;
    match_seen_foe = seen;
    foes_seen_t = seen ? foes_seen_min + 1 : 0;
    foes_gone_t = 0;
    stub_status = status;
    stub_online[1] = online;
    stub_alive[1] = alive;
    for (int s = 0; s < max_players; s++) {
        arr_set(remotes, s * remote_fields, 1);
        arr_set(remotes, s * remote_fields + 5, stub_alive[s]);
    }
}

static void tick_seconds(double seconds) {
    long long n = (long long)(seconds * 60.0 + 0.5);
    for (long long k = 0; k < n; k++) ds_fn_check_online_finish();
}

int main(void) {
    screen_w = 1280; screen_h = 720;
    amgr = (AAssetManager *)&dummy_amgr_storage;
    ds_main();
    ds_fn_init();
    dt = 1.0 / 60.0;
    game_state = ST_ONLINE;

    /* A connection that is not playing yet: no win, however dead everyone looks. */
    reset_case(NET_CONNECTING, 0, 0, 1);
    tick_seconds(6.0);
    check(finished == 0, "a fight was won while the connection was still coming up");

    /* A dead foe is the normal win, after the short confirmation. */
    reset_case(NET_PLAYING, 1, 0, 1);
    tick_seconds(foes_dead_min * 0.5);
    check(finished == 0, "a dead foe ended the fight without its confirmation");
    tick_seconds(foes_dead_min * 0.5 + 0.1);
    check(finished == 1, "a dead foe in the room did not end the fight");

    /* Nobody in the room: the fight waits for the longer timer, so a dropout of a
     * few seconds does not hand out a win. */
    reset_case(NET_PLAYING, 0, 0, 1);
    tick_seconds(foes_gone_min - 0.5);
    check(finished == 0, "an empty room ended the fight before the timer");
    tick_seconds(1.0);
    check(finished == 1, "an empty room never ended the fight");

    /* The foe comes back while the timer runs: no win. */
    reset_case(NET_PLAYING, 0, 0, 1);
    tick_seconds(foes_gone_min - 0.5);
    stub_online[1] = 1; stub_alive[1] = 1;
    arr_set(remotes, 1 * remote_fields + 5, 1);
    tick_seconds(foes_gone_min);
    check(finished == 0, "a foe who came back did not stop the count down");

    /* A foe in the room: no win while he lives, and seeing him marks the match. */
    reset_case(NET_PLAYING, 1, 1, 0);
    tick_seconds(foes_seen_min + 1.0);
    check(match_seen_foe == 1, "seeing a live foe did not mark the match");
    check(finished == 0, "the fight was won with a live foe in the room");

    puts("online finish: the win needs a live room, a short dead confirmation and a long empty one");
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
    with tempfile.TemporaryDirectory(prefix="online-finish-") as td:
        binary = build(Path(td))
        run = subprocess.run([str(binary)], capture_output=True, text=True, cwd=str(ROOT))
        sys.stderr.write(run.stderr)
        sys.stdout.write(run.stdout)
        return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
