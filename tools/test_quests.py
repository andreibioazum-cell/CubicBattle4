#!/usr/bin/env python3
"""Quests: a cut-down reward and no cooldown.

A finished quest pays quest_cups and quest_candies (2 cups and 1 candy) and is
replaced by a new quest at once; the slot never sits empty for five minutes as
it used to. A save of an older version that still holds a slot in cooldown
(type -1 with a stamp) gets a quest in that slot on the next load, and the
stamp is cleared. The test compiles the real game scripts; the native quest
store is a small in-memory copy of native/net/quests.inc. Run from the
repository root after python3 gen.py:

    python3 tools/test_quests.py
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import test_dash_hitbox  # noqa: E402  (the same build: real scripts, auto stubs)

MAIN = r"""
static double store[12]; static int has_store = 0, saves = 0;
void net_save_quest_state(double t0, double p0, double n0, double x0,
                          double t1, double p1, double n1, double x1,
                          double t2, double p2, double n2, double x2) {
    double v[12] = { t0, p0, n0, x0, t1, p1, n1, x1, t2, p2, n2, x2 };
    memcpy(store, v, sizeof v); has_store = 1; saves++;
}
double net_load_quest_state(double slot, double field) { return store[(int)slot * 4 + (int)field]; }
double net_quest_has_state(void) { return has_store; }

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
    check(quest_cups == 2 && quest_candies == 1, "the reward is not 2 cups and 1 candy");

    ds_fn_quest_load();
    check(arr_len(quest_type) == 3, "three quests are not there");
    /* Finish every slot several times: each finish pays once and a new quest
     * takes the slot at once. */
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 3; i++) {
            arr_set(quest_prog, i, arr_get(quest_need, i));
            int c0 = cups, k0 = candies;
            ds_fn_quest_complete(i);
            check(cups == c0 + 2 && candies == k0 + 1, "a finished quest did not pay 2 cups and 1 candy");
            check(arr_get(quest_type, i) >= 0 && arr_get(quest_type, i) <= 3, "the slot is empty after a finish (cooldown)");
            check(arr_get(quest_prog, i) == 0, "the new quest does not start from zero");
            check(arr_get(quest_next, i) == 0 && store[i * 4 + 3] == 0, "a cooldown stamp was stored");
            ds_fn_quest_complete(i);
            check(cups == c0 + 2, "an unfinished new quest paid again");
        }
    }
    /* A quest finished through play (a solo win) is replaced at once too. */
    game_state = ST_SOLO;
    arr_set(quest_type, 0, 0); arr_set(quest_prog, 0, 0); arr_set(quest_need, 0, 1);
    /* The other slots wait for online wins, which a solo win does not count. */
    for (int i = 1; i < 3; i++) { arr_set(quest_type, i, 1); arr_set(quest_prog, i, 0); arr_set(quest_need, i, 1); }
    int c1 = cups;
    ds_fn_quest_on_win(1);
    check(cups == c1 + 2 && arr_get(quest_prog, 0) == 0, "a won quest did not pay and roll over");
    puts("quests: +2 cups and +1 candy, a finished quest is replaced at once");

    /* A save from an older version: slot 1 sits in a five minute cooldown. */
    double old[12] = { 0, 0, 1, 0,   -1, 0, 1, 1900000000,   3, 1, 3, 0 };
    memcpy(store, old, sizeof old); has_store = 1;
    arr_clear(quest_type);
    ds_fn_quest_load();
    check(arr_get(quest_type, 1) >= 0, "the old cooldown slot is still empty after a load");
    check(arr_get(quest_next, 1) == 0 && store[7] == 0, "the old cooldown stamp was not cleared");
    check(arr_get(quest_type, 2) == 3 && arr_get(quest_prog, 2) == 1, "a quest in progress was lost on load");
    ds_fn_draw_quests();
    puts("quests: an old save's cooldown slot gets a quest at once, progress kept");
    return 0;
}
"""


def main() -> int:
    subprocess.run([sys.executable, str(ROOT / "gen.py")], cwd=str(ROOT), check=True,
                   stdout=subprocess.DEVNULL)
    test_dash_hitbox.MAIN = MAIN
    with tempfile.TemporaryDirectory() as tmp:
        binary = test_dash_hitbox.build(Path(tmp))
        run = subprocess.run([str(binary)], cwd=str(ROOT), capture_output=True, text=True, timeout=300)
        print(run.stdout, end="")
        if run.returncode != 0:
            print(run.stderr[-2000:], end="")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
