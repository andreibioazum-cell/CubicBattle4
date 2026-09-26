#!/usr/bin/env python3
"""Saving checks: everything the player earns must reach the device AND the cloud.

The regression this guards: `save_progress()` in ui/progress_rewards.ds used to
be a multi-line `net_save_progress_all(...)` call, the DimScript compiler had no
line continuation and silently dropped it — so nothing (cups, candies, the ebuC,
levels, skins) was ever written to progress.dat or to Firebase.

The test compiles the real DimScript sources, links them against a fake network
layer that plays the role of net.c (progress.dat / settings.dat / cloud), and
then drives the real script functions: buy the ebuC, restart, check it is still
there. Requires a host C compiler (CC). No Android, no Firebase.
"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ds_compiler import DimScriptCompiler  # noqa: E402
from gen import find_ds_files  # noqa: E402

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include "game.c"

int screen_w = 1280, screen_h = 720;
double dt = 0.05;
int mouse_clicked = 0;
Joy joy;
double ds_mouse_x = 0, ds_mouse_y = 0;

void ds_log(const char *format, ...) { (void)format; }
void ds_log_err(const char *format, ...) { (void)format; }
void ds_console_log(int is_error, const char *format, ...) { (void)is_error; (void)format; }
void ds_runtime_error(const char *format, ...) { fputs(format, stderr); abort(); }
void keyboard_hide(void) {}
void net_disconnect(void) {}

/* Runtime string helpers: mods come from an input field, so str_trim is the real
 * one here and the script saves a name that is already trimmed. */
double str_len(const char *s) { return s ? (double)strlen(s) : 0; }
int str_eq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
const char *str_trim(const char *s) {
    static char buf[128];
    const char *end;
    size_t n;
    if (!s) return "";
    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
    n = (size_t)(end - s);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return buf;
}

struct DSArray { int len; double values[512]; };
DSArray *arr_new(void) { DSArray *a = calloc(1, sizeof(*a)); assert(a); return a; }
void arr_free(DSArray *a) { free(a); }
void arr_clear(DSArray *a) { if (a) a->len = 0; }
void arr_push(DSArray *a, double v) { assert(a && a->len < 512); a->values[a->len++] = v; }
double arr_get(DSArray *a, double i) { return a && i >= 0 && i < a->len ? a->values[(int)i] : 0; }
void arr_set(DSArray *a, double i, double v) {
    assert(a && i >= 0 && i < 512);
    while (a->len <= i) arr_push(a, 0);
    a->values[(int)i] = v;
}
double arr_len(DSArray *a) { return a ? a->len : 0; }
/* Quests: this test needs no native state, only safe stubs. */
void net_save_quest_state(double t0, double p0, double n0, double x0,
                          double t1, double p1, double n1, double x1,
                          double t2, double p2, double n2, double x2) {
    (void)t0;(void)p0;(void)n0;(void)x0;(void)t1;(void)p1;(void)n1;(void)x1;
    (void)t2;(void)p2;(void)n2;(void)x2;
}
double net_load_quest_state(double s, double f) { (void)s; (void)f; return 0; }
double net_quest_has_state(void) { return 0; }
double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
double lerp(double a, double b, double t) { return a + (b - a) * t; }
double dist(double x, double y, double a, double b) { return hypot(x - a, y - b); }
int png_load(const char *name) { (void)name; return 1; }
int snd_load(const char *name) { (void)name; return 0; }
void snd_volume(const char *name, double v) { (void)name; (void)v; }
int snd_loop(const char *name) { (void)name; return 1; }
void snd_stop(const char *name) { (void)name; }
int snd_playing(const char *name) { (void)name; return 0; }
void net_set_firebase_key(const char *key) { (void)key; }
void net_autologin(const char *url) { (void)url; }

/* -- Device and cloud: net.c in miniature -------------------------------
 * Every net_save_* writes here and every net_load_* reads from here, just like
 * progress.dat and settings.dat on the phone. The save_* counters show that the
 * game tried to save at all. */
static struct {
    int progress_saves, settings_saves, cloud_pushes;
    double cups, candies, cls, azum, santa, ebuc, level, levels;
    double ol, ou, al, au, sl, su, el, eu, bp, skin;
    double flags, revives;
    double language, hitboxes, musicvol, winter, showfps;
} store;

void net_save_progress_all(double cups, double candies, double cls, double azum, double santa,
    double ebuc, double level, double levels, double ol, double ou, double al, double au,
    double sl, double su, double el, double eu, double bp, double skin) {
    store.progress_saves++;
    store.cups = cups; store.candies = candies; store.cls = cls;
    store.azum = azum; store.santa = santa; store.ebuc = ebuc;
    store.level = level; store.levels = levels;
    store.ol = ol; store.ou = ou; store.al = al; store.au = au;
    store.sl = sl; store.su = su; store.el = el; store.eu = eu;
    store.bp = bp; store.skin = skin;
}
double net_load_cups(void) { return store.cups; }
double net_load_candies(void) { return store.candies; }
double net_load_class(void) { return store.cls; }
double net_load_azum(void) { return store.azum; }
double net_load_santa(void) { return store.santa; }
double net_load_ebuc(void) { return store.ebuc; }
double net_load_level(void) { return store.level; }
double net_load_levels_unlocked(void) { return store.levels; }
double net_load_ordinary_level(void) { return store.ol; }
double net_load_ordinary_levels_unlocked(void) { return store.ou; }
double net_load_azum_level(void) { return store.al; }
double net_load_azum_levels_unlocked(void) { return store.au; }
double net_load_santa_level(void) { return store.sl; }
double net_load_santa_levels_unlocked(void) { return store.su; }
double net_load_ebuc_level(void) { return store.el; }
double net_load_ebuc_levels_unlocked(void) { return store.eu; }
double net_load_bp_level(void) { return store.bp; }
double net_load_azum_skin(void) { return store.skin; }
double net_load_achievement_flags(void) { return store.flags; }
void net_save_achievement_flags(double f) { store.flags = f; }
double net_load_azum_revives(void) { return store.revives; }
void net_save_azum_revives(double r) { store.revives = r; }
void net_mark_achievement_flag(double bit) { store.flags += bit; store.cloud_pushes++; }

void net_save_settings(double language, double hitboxes) {
    store.settings_saves++;
    store.language = language;
    store.hitboxes = hitboxes;
}
double net_load_language(void) { return store.language; }
double net_load_hitboxes(void) { return store.hitboxes; }
double net_load_music_volume(void) { return store.musicvol; }
void net_save_music_volume(double v) { store.musicvol = v; }
double net_load_winter_theme(void) { return store.winter; }
void net_save_winter_theme(double v) { store.winter = v; }
double net_load_fps_meter(void) { return store.showfps; }
void net_save_fps_meter(double v) { store.showfps = v; }

/* The network snapshot (class, level and skin of our fighter) just counts calls. */
void net_set_class(double v) { (void)v; store.cloud_pushes++; }
void net_set_level(double v) { (void)v; }
void net_set_skin(double v) { (void)v; }

static void near(const char *what, double a, double b) {
    if (fabs(a - b) >= 1e-6) {
        fprintf(stderr, "near fail (%s): %g vs %g\n", what, a, b);
        abort();
    }
}
static void fresh_device(void) {
    memset(&store, 0, sizeof(store));
    store.hitboxes = 1;  /* the settings.dat default */
    store.musicvol = 70; /* the default music volume */
    store.winter = 1;    /* the winter theme is on by default */
    store.showfps = 0;   /* the battle frame counter is off by default */
    reset();
}

/* Buying the ebuC has to reach the save: currency, class owner, selection. */
static void test_buy_ebuc_saves(void) {
    fresh_device();
    /* The ebuC is bought with cups (120), not candies: the candies stay. */
    cups = 500;
    candies = 7;
    ds_fn_pick_class(CLASS_EBUC);
    assert(store.progress_saves >= 1);
    near("store.ebuc, 1", store.ebuc, 1);
    near("store.cls, CLASS_EBUC", store.cls, CLASS_EBUC);
    near("ebuc_cost, 120", ebuc_cost, 120);
    near("store.cups, 500 - ebuc_cost", store.cups, 500 - ebuc_cost);
    near("store.candies, 7", store.candies, 7);
    near("ds_fn_class_owned_of(CLASS_EBUC), 1", ds_fn_class_owned_of(CLASS_EBUC), 1);
    /* The branch level and the skin go through the same save. */
    cups = 100;
    assert(ds_fn_buy_level(CLASS_EBUC, 1) == 1);
    near("store.el, 1", store.el, 1);
    near("store.eu, 1", store.eu, 1);
    near("store.cls, CLASS_EBUC", store.cls, CLASS_EBUC);
    /* Only Azum has skins: buy him and take the zombie one. */
    achievement_mask = ACH_LEGENDS;
    cups = 500;
    ds_fn_pick_class(CLASS_AZUM);
    near("store.azum, 1", store.azum, 1);
    ds_fn_pick_skin(SKIN_ZOMBIE);
    near("store.skin, SKIN_ZOMBIE", store.skin, SKIN_ZOMBIE);
    puts("save: ebuC, its level and skin reach net_save_progress_all");
}

/* A game restart: whatever was saved has to load back. */
static void test_restart_keeps_ebuc(void) {
    fresh_device();
    candies = 400;
    cups = 60 + ebuc_cost;
    ds_fn_pick_class(CLASS_EBUC);
    ds_fn_buy_level(CLASS_EBUC, 1);
    int before = store.progress_saves;
    /* A restart clears the script memory while the device file stays. */
    reset();
    assert(ds_fn_class_owned_of(CLASS_EBUC) == 0);
    ds_fn_progress_from_storage();
    assert(store.progress_saves == before);  /* reading must not rewrite the file */
    near("ds_fn_class_owned_of(CLASS_EBUC), 1", ds_fn_class_owned_of(CLASS_EBUC), 1);
    near("player_class, CLASS_EBUC", player_class, CLASS_EBUC);
    near("candies, 400", candies, 400);
    near("cups, 60 - ds_fn_level_cost(1)", cups, 60 - ds_fn_level_cost(1));
    near("ds_fn_class_level_of(CLASS_EBUC), 1", ds_fn_class_level_of(CLASS_EBUC), 1);
    near("player_level, 1", player_level, 1);
    puts("restart: ebuC, currencies and its level survive a full script reset");
}

/* Settings: a file of their own on the device and their own save calls. */
static void test_settings_save(void) {
    fresh_device();
    language = 1;
    show_hitboxes = 0;
    ds_fn_save_settings();
    assert(store.settings_saves == 1);
    near("store.language, 1", store.language, 1);
    near("store.hitboxes, 0", store.hitboxes, 0);

    /* After a restart the language and hitboxes come from the file. */
    reset();
    near("language, 0", language, 0);
    near("show_hitboxes, 1", show_hitboxes, 1);
    ds_fn_settings_from_storage();
    near("language, 1", language, 1);
    near("show_hitboxes, 0", show_hitboxes, 0);

    /* Music volume: read from the file, cycled off -> 100 -> 50 -> off by the
     * music button (music_volume_cycle) and saved at once. */
    store.musicvol = 45;
    reset();
    ds_fn_settings_from_storage();
    near("music_volume, 45", music_volume, 45);
    ds_fn_music_volume_cycle();
    near("music_volume, 0", music_volume, 0);
    near("store.musicvol, 0", store.musicvol, 0);
    ds_fn_music_volume_cycle();
    near("music_volume, 100", music_volume, 100);
    ds_fn_music_volume_cycle();
    near("music_volume, 50", music_volume, 50);
    near("store.musicvol, 50", store.musicvol, 50);

    /* The winter theme and the frame counter: their own file keys, the same path. */
    reset();
    ds_fn_settings_from_storage();
    near("winter_theme, 1", winter_theme, 1);
    near("show_fps, 0", show_fps, 0);
    winter_theme = 0;
    show_fps = 1;
    ds_fn_save_settings();
    near("store.winter, 0", store.winter, 0);
    near("store.showfps, 1", store.showfps, 1);
    /* After a restart the theme and the counter load back. */
    reset();
    near("winter_theme, 1", winter_theme, 1);
    near("show_fps, 0", show_fps, 0);
    ds_fn_settings_from_storage();
    near("winter_theme, 0", winter_theme, 0);
    near("show_fps, 1", show_fps, 1);
    puts("settings: language, hitboxes, music, winter theme and FPS meter survive a restart");
}

/* The whole startup path: init() has to bring up both progress and settings. */
static void test_init_loads_everything(void) {
    fresh_device();
    store.ebuc = 1;
    store.cls = CLASS_EBUC;
    store.candies = 77;
    store.cups = 12;
    store.el = 2;
    store.eu = 2;
    store.skin = 1;
    store.flags = ACH_LEGENDS;
    store.language = 1;
    store.winter = 1;
    ds_fn_init();
    near("winter_theme, 1", winter_theme, 1);
    near("snow_tex_ok, 1", snow_tex_ok, 1);
    near("candies, 77", candies, 77);
    near("cups, 12", cups, 12);
    near("player_class, CLASS_EBUC", player_class, CLASS_EBUC);
    near("ds_fn_class_owned_of(CLASS_EBUC), 1", ds_fn_class_owned_of(CLASS_EBUC), 1);
    near("ds_fn_class_level_of(CLASS_EBUC), 2", ds_fn_class_level_of(CLASS_EBUC), 2);
    near("achievement_mask, ACH_LEGENDS", achievement_mask, ACH_LEGENDS);
    near("language, 1", language, 1);
    /* The theme is off, so no snow tile loads and the arena stays grass. */
    store.winter = 0;
    reset();
    ds_fn_init();
    near("winter_theme, 0", winter_theme, 0);
    near("snow_tex_ok, 0", snow_tex_ok, 0);
    puts("init: startup reads progress and settings from the device");
}

/* Leaving a battle saves progress, so picked candies are not lost. */
static void test_leaving_battle_saves(void) {
    fresh_device();
    game_state = ST_SOLO;
    candies = 5;
    int before = store.progress_saves;
    ds_fn_leave_screen(ST_SOLO);
    assert(store.progress_saves == before + 1);
    near("store.candies, 5", store.candies, 5);
    before = store.progress_saves;
    ds_fn_leave_screen(ST_LOBBY);
    assert(store.progress_saves == before);  /* leaving the menu writes nothing */
    puts("battle exit: progress is saved when leaving solo/online");
}

/* The ebuC costs 120 cups (not candies), and its card stands right after Azum,
 * with Santa after the ebuC. */
static void test_ebuc_price_and_order(void) {
    fresh_device();
    near("ds_fn_class_cost_of(CLASS_EBUC), 120", ds_fn_class_cost_of(CLASS_EBUC), 120);
    near("ds_fn_class_pays_candies(CLASS_EBUC), 0", ds_fn_class_pays_candies(CLASS_EBUC), 0);
    near("ds_fn_class_pays_candies(CLASS_SANTA), 1", ds_fn_class_pays_candies(CLASS_SANTA), 1);
    cups = 119; candies = 999;
    ds_fn_pick_class(CLASS_EBUC);          /* one cup short: candies do not help */
    near("owned after 119 cups, 0", ds_fn_class_owned_of(CLASS_EBUC), 0);
    near("class_msg_kind (cups), 0", class_msg_kind, 0);
    near("cups untouched, 119", cups, 119);
    near("candies untouched, 999", candies, 999);
    cups = 120;
    ds_fn_pick_class(CLASS_EBUC);
    near("owned after 120 cups, 1", ds_fn_class_owned_of(CLASS_EBUC), 1);
    near("cups spent, 0", cups, 0);
    near("candies kept, 999", candies, 999);
    near("store.cups, 0", store.cups, 0);
    /* Card order: Ordinary, Azum, ebuC, Santa. */
    candy_enabled = 1;
    near("visible count, 4", ds_fn_classes_visible_count(), 4);
    near("card 0, ordinary", ds_fn_visible_class_at(0), CLASS_ORDINARY);
    near("card 1, azum", ds_fn_visible_class_at(1), CLASS_AZUM);
    near("card 2, ebuC", ds_fn_visible_class_at(2), CLASS_EBUC);
    near("card 3, santa", ds_fn_visible_class_at(3), CLASS_SANTA);
    near("index of ebuC, 2", ds_fn_class_visible_index(CLASS_EBUC), 2);
    near("index of santa, 3", ds_fn_class_visible_index(CLASS_SANTA), 3);
    /* Without the candy season an unbought Santa is hidden: three cards. */
    candy_enabled = 0;
    ds_fn_set_class_owned(CLASS_SANTA, 0);
    near("visible count, 3", ds_fn_classes_visible_count(), 3);
    near("card 2, ebuC", ds_fn_visible_class_at(2), CLASS_EBUC);
    near("index of santa, -1", ds_fn_class_visible_index(CLASS_SANTA), -1);
    candy_enabled = 1;
    puts("shop: ebuC for 120 cups, cards ordinary/azum/ebuC/santa");
}

int main(void) {
    test_buy_ebuc_saves();
    test_restart_keeps_ebuc();
    test_ebuc_price_and_order();
    test_settings_save();
    test_init_loads_everything();
    test_leaving_battle_saves();
    puts("all saving checks passed");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="cubic-saving-") as directory:
        temp = Path(directory)
        compiler = DimScriptCompiler()
        assert compiler.compile(find_ds_files(str(ROOT / "game/scripts")), str(temp / "game.c"))
        assert not compiler.errors and not compiler.warnings

        # -- Wiring: the game really calls the save functions --
        fns = compiler.functions
        save_body = "".join(fns["save_progress"][2])
        assert "net_save_progress_all(" in save_body, (
            "save_progress() must call net_save_progress_all — the whole "
            "progress write (device + cloud) lives in that one call"
        )
        assert save_body.count("net_save_progress_all(") == 1
        # 18 positional fields: cups, candies, cls, azum, santa, ebuc, level,
        # levels, 4x (level, levels_unlocked) per class, bp, skin.
        call = save_body[save_body.index("net_save_progress_all("):]
        assert call.count(",") == 17, f"expected 18 fields, got {call.count(',') + 1}"
        for fn, hook in (("touch_settings", "save_settings()"),
                         ("leave_screen", "save_progress()"),
                         ("login_do", "settings_from_storage()"),
                         ("init", "settings_from_storage()")):
            assert hook in "".join(fns[fn][2]), f"{fn} must call {hook}"

        android = temp / "android"
        android.mkdir()
        (android / "asset_manager.h").write_text("typedef struct AAssetManager AAssetManager;\n")
        (android / "log.h").touch()
        (android / "native_window.h").write_text("typedef struct ANativeWindow ANativeWindow;\n")
        (temp / "test.c").write_text(HARNESS)
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=c99", "-O0",
            "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
            "-ffunction-sections", "-fdata-sections", "-I", str(temp), "-I", str(ROOT),
            str(temp / "test.c"), "-Wl,--gc-sections", "-lm", "-o", str(temp / "test"),
        ], check=True)
        subprocess.run([str(temp / "test")], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
