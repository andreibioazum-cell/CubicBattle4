#!/usr/bin/env python3
"""Promo cards in the real game script (promo.ds, battle_setup.ds).

  chance  — a solo battle starts with a card on the arena 5 times in 100, right
            away (no drop timer), clear of both fighters; online never has one;
  card    — picking a card up asks for a fresh random code and the card shows
            exactly that code; the next card shows another one;
  screen  — the promo screen never prints a card code, used or not;
  redeem  — a well-formed code that no card gave is refused, the code of the
            last card gives +50 cups and +20 candies once, and a second code
            after that is refused as already redeemed.

The native side (random codes, promo.dat, the cloud) is tools/test_promo.py.
Run from the repository root after python3 gen.py:

    python3 tools/test_promo_cards.py
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT))
import test_punch_sprites  # noqa: E402  (its build() links the script with auto stubs)
import ui_preview  # noqa: E402

MAIN = r"""
/* Native promo stand-ins: a sequence of distinct codes, the last one redeems. */
static char issued[8] = "";
static int issued_n = 0, used_flag = 0, mark_used_calls = 0;
static const char *codes[] = { "KXM7", "Q4WZ", "HT9P", "BN3R" };
const char *net_promo_new_code(void) {
    snprintf(issued, sizeof issued, "%s", codes[issued_n++ % 4]);
    return issued;
}
const char *net_promo_code(void) { return issued; }
double net_promo_check(const char *c) { return issued[0] && c && !strcmp(c, issued); }
double net_promo_used(void) { return used_flag; }
void net_promo_mark_used(void) { used_flag = 1; mark_used_calls++; issued[0] = 0; }

/* The redeem path upper-cases and trims the input: real versions, not stubs. */
const char *str_trim(const char *s) {
    static char b[64]; size_t n;
    while (s && (*s == ' ' || *s == '\t')) s++;
    snprintf(b, sizeof b, "%s", s ? s : "");
    n = strlen(b);
    while (n && (b[n-1] == ' ' || b[n-1] == '\t')) b[--n] = 0;
    return b;
}
const char *str_upper(const char *s) {
    static char b[64];
    snprintf(b, sizeof b, "%s", s ? s : "");
    for (char *p = b; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    return b;
}

static void check(int cond, const char *what) {
    if (cond) return;
    printf("FAIL: %s\n", what);
    exit(1);
}

/* Draws one call into a fresh command list and says whether any text drawn
 * contains the needle. */
static int drew_text(void (*fn)(void), const char *needle) {
    frame_open = 1; cmd_n = 0;
    fn();
    int found = 0;
    for (size_t i = 0; i < cmd_n; i++)
        if (cmds[i].t == DS_CMD_TEXT && cmds[i].v.tt.s && strstr(cmds[i].v.tt.s, needle)) found = 1;
    frame_open = 0; cmd_n = 0;
    return found;
}
static void draw_promo_screen(void) { ds_fn_draw_promo(); }
static void draw_card_overlay(void) { ds_fn_draw_card_open(); }

int main(void) {
    screen_w = 1280; screen_h = 720;
    amgr = (AAssetManager *)&dummy_amgr_storage;
    ds_main();
    ds_fn_init();
    language = 1;
    game_state = ST_SOLO;
    dt = 1.0 / 60.0;
    srand(12345);
    stub_login_status = 2;                      /* logged in */

    /* 5 in 100, right at the start, clear of both fighters. */
    int battles = 40000, with_card = 0, close = 0;
    for (int i = 0; i < battles; i++) {
        ds_fn_init_game();
        if (card_active == 1) {
            with_card++;
            if (dist(card_x, card_y, player->x, player->y) < 140 ||
                dist(card_x, card_y, enemy->x, enemy->y) < 140) close++;
        }
    }
    double pct = 100.0 * with_card / battles;
    printf("chance: %d of %d solo battles start with a card (%.2f%%), %d too close\n",
           with_card, battles, pct, close);
    check(pct > 4.5 && pct < 5.5, "the card chance is not 5 per cent");
    check(close * 100 < with_card, "cards land on top of the fighters");
    int online = 0;
    for (int i = 0; i < 4000; i++) { ds_fn_init_online(); online += card_active == 1; }
    check(online == 0, "an online battle got a card");

    /* Pick a card up: a fresh code, shown on the card. */
    do { ds_fn_init_game(); } while (card_active == 0);
    player->x = card_x; player->y = card_y;
    ds_fn_card_update();
    check(card_open == 1 && issued_n == 1, "walking over the card does not open it");
    printf("card 1: %s\n", card_code);
    check(!strcmp(card_code, "KXM7"), "the card does not keep the code it was given");
    check(drew_text(draw_card_overlay, "KXM7"), "the card does not show its code");
    ds_fn_card_close();
    do { ds_fn_init_game(); } while (card_active == 0);
    player->x = card_x; player->y = card_y;
    ds_fn_card_update();
    printf("card 2: %s\n", card_code);
    check(!strcmp(card_code, "Q4WZ") && drew_text(draw_card_overlay, "Q4WZ"),
          "the next card does not get a new code");
    ds_fn_card_close();

    /* The promo screen never prints the code. */
    game_state = ST_PROMO;
    check(!drew_text(draw_promo_screen, "Q4WZ") && !drew_text(draw_promo_screen, "KXM7"),
          "the promo screen shows a card code");

    /* Redeeming: a guess is wrong, the old card is wrong, the last card pays once. */
    double c0 = cups, k0 = candies;
    promo_focus = 0;
    promo_input = "ABC7"; ds_fn_promo_submit();
    check(cups == c0 && mark_used_calls == 0, "a code no card gave was accepted");
    promo_input = "kxm7"; ds_fn_promo_submit();
    check(cups == c0 && mark_used_calls == 0, "the code of an older card was accepted");
    promo_input = "q4wz"; ds_fn_promo_submit();       /* typed in lower case */
    printf("redeem: cups %g -> %g, candies %g -> %g\n", c0, cups, k0, candies);
    check(cups == c0 + promo_reward_cups && candies == k0 + promo_reward_candies && mark_used_calls == 1,
          "the code of the last card does not pay the reward");
    check(!drew_text(draw_promo_screen, "Q4WZ"), "the promo screen shows the code after the reward");

    /* One reward per account: a later card still has a code, it does not pay. */
    game_state = ST_SOLO;
    do { ds_fn_init_game(); } while (card_active == 0);
    player->x = card_x; player->y = card_y;
    ds_fn_card_update();
    check(drew_text(draw_card_overlay, card_code), "a card after the reward does not show its code");
    ds_fn_card_close();
    double c1 = cups;
    promo_input = (char *)card_code; ds_fn_promo_submit();
    check(cups == c1 && mark_used_calls == 1, "a second reward was paid on the same account");

    puts("promo cards: 5% at the battle start, a random code per card, never on the promo screen, one reward");
    return 0;
}
"""


def main() -> int:
    game_c = ROOT / "game" / "game.c"
    if not game_c.exists():
        sys.exit("game/game.c is missing, run python3 gen.py first")
    test_punch_sprites.MAIN = MAIN
    # The preview stubs pin the login status to 0; redeeming needs a session.
    fixed = "double net_login_status(void) { return 0; }"
    assert fixed in ui_preview.STUBS
    ui_preview.STUBS = ui_preview.STUBS.replace(
        fixed, "static double stub_login_status = 0;\n"
               "double net_login_status(void) { return stub_login_status; }")
    with tempfile.TemporaryDirectory(prefix="promo-cards-") as td:
        binary = test_punch_sprites.build(Path(td))
        run = subprocess.run([str(binary)], capture_output=True, text=True, cwd=str(ROOT))
        sys.stderr.write(run.stderr[-2000:])
        sys.stdout.write(run.stdout)
        return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
