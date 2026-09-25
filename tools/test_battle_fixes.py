#!/usr/bin/env python3
"""Regression checks for the battle fixes batch, without Android or Firebase.

Covers: real dash hitbox (traveled length, rounded dark cells), turret shield of
the ebuC absorbing all damage, snowflakes piercing turrets, smooth hitbox fades,
filled snowflake hitbox, poison drawn green next to blue freeze, and the
warning/studio splash timing (black screen never jumps). Compiles the real
DimScript sources with a host C harness and asserts on the actual logic/draw
calls.
"""
from pathlib import Path
import json
import os
import re
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
#include <string.h>
#include "game.c"

int screen_w = 1280, screen_h = 720;
double dt = 0.05;
int mouse_clicked = 0;
Joy joy;
double ds_mouse_x = 0, ds_mouse_y = 0;

static int legal_marks;
void settings_mark_legal(void) { legal_marks++; }
void ds_log(const char *format, ...) { (void)format; }
void ds_log_err(const char *format, ...) { (void)format; }
void ds_console_log(int is_error, const char *format, ...) { (void)is_error; (void)format; }
void ds_runtime_error(const char *format, ...) { fputs(format, stderr); abort(); }

struct DSArray { int len; double values[8192]; };
DSArray *arr_new(void) { DSArray *a = calloc(1, sizeof(*a)); assert(a); return a; }
void arr_free(DSArray *a) { free(a); }
void arr_clear(DSArray *a) { if (a) a->len = 0; }
void arr_push(DSArray *a, double v) { assert(a && a->len < 8192); a->values[a->len++] = v; }
double arr_get(DSArray *a, double i) { return a && i >= 0 && i < a->len ? a->values[(int)i] : 0; }
void arr_set(DSArray *a, double i, double v) {
    assert(a && i >= 0 && i < 8192);
    while (a->len <= i) arr_push(a, 0);
    a->values[(int)i] = v;
}
double arr_len(DSArray *a) { return a ? a->len : 0; }
double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
double dist(double x, double y, double a, double b) { return hypot(x - a, y - b); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }
/* Runtime string helpers: the battle screens need them for the button labels
 * and the "+N cups" counters. */
/* As in the real runtime every call returns a fresh buffer, so nested
 * concatenations cannot overwrite each other. */
char *ds_concat(const char *left, const char *right) {
    const char *l = left ? left : "", *r = right ? right : "";
    size_t la = strlen(l), lb = strlen(r);
    char *out = malloc(la + lb + 1);
    assert(out);
    memcpy(out, l, la); memcpy(out + la, r, lb); out[la + lb] = 0;
    return out;
}
char *ds_num_to_string(double value) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    return buf;
}
/* The swing textures can be broken on purpose: tex() draws nothing for a
 * missing texture, which used to make a fighter vanish while punching. */
static int punch_tex_broken = 0;
int png_load(const char *name) {
    if (punch_tex_broken && name && strstr(name, "punch")) return 0;
    return 1;
}
void tex(float x, float y, const char *name, float a, float sc) { (void)x; (void)y; (void)name; (void)a; (void)sc; }
/* Shadows go through tex_tint, so the calls are counted and the texture kept. */
static int tint_calls = 0;
static char tint_last[64] = "";
void tex_tint(float x, float y, const char *name, float a, float sc, uint32_t c) {
    (void)x; (void)y; (void)a; (void)sc; (void)c;
    tint_calls++;
    snprintf(tint_last, sizeof(tint_last), "%s", name ? name : "");
}

int text_ink_width(const char *s) { (void)s; return 10; }
int text_ink_height(const char *s) { (void)s; return 10; }
int text_ink_top(const char *s) { (void)s; return 2; }
/* The last drawn string: the consent button at the end of draw_warning prints
 * its label last, so last_text holds that text. */
static char last_text[160];
void text_scaled(const char *s, float x, float y, uint32_t c, float scale) {
    (void)x; (void)y; (void)c; (void)scale;
    snprintf(last_text, sizeof(last_text), "%s", s ? s : "");
}

/* Net stubs (same prototypes as net.h): purchases/saves never reach the cloud
 * in these tests, they only need to not crash. */
void net_save_progress_all(double, double, double, double, double, double,
    double, double, double, double, double, double,
    double, double, double, double, double, double) {}
void net_save_progress(double, double, double, double, double, double, double, double) {}
/* The room snapshot is test driven: by default there is no slot, so solo. */
static double stub_slot = -1;
static double stub_online[4], stub_alive[4], stub_punch[4];
double net_slot(void) { return stub_slot; }
double net_event(void) { return 0; }
void net_set_class(double v) { (void)v; }
void net_set_level(double v) { (void)v; }
void net_set_skin(double v) { (void)v; }
double net_load_cups(void) { return 0; }
double net_load_candies(void) { return 0; }
double net_load_class(void) { return 0; }
double net_load_azum(void) { return 0; }
double net_load_santa(void) { return 0; }
double net_load_ebuc(void) { return 0; }
double net_load_level(void) { return 0; }
double net_load_levels_unlocked(void) { return 0; }
double net_load_ordinary_level(void) { return 0; }
double net_load_ordinary_levels_unlocked(void) { return 0; }
double net_load_azum_level(void) { return 0; }
double net_load_azum_levels_unlocked(void) { return 0; }
double net_load_santa_level(void) { return 0; }
double net_load_santa_levels_unlocked(void) { return 0; }
double net_load_ebuc_level(void) { return 0; }
double net_load_ebuc_levels_unlocked(void) { return 0; }
double net_load_bp_level(void) { return 0; }
double net_load_azum_skin(void) { return 0; }
void net_mark_achievement_flag(double) {}
void net_save_achievement_flags(double) {}
double net_has_achievement_flag(double) { return 0; }
double net_load_achievement_flags(void) { return 0; }
void net_save_azum_revives(double) {}
double net_load_azum_revives(void) { return 0; }
/* Promo codes (promo.inc): the tests pin the code and leave the activation flag
 * and the match streak alone. */
const char *net_promo_code(void) { return "ABC1"; }
const char *net_promo_new_code(void) { return "ABC1"; }
double net_promo_check(const char *code) { return code && !strcmp(code, "ABC1"); }
double net_promo_used(void) { return 0; }
void net_promo_mark_used(void) {}
double net_load_playtime(void) { return 0; }
void net_save_playtime(double s) { (void)s; }
void net_add_playtime(double d) { (void)d; }
/* Quests: a controllable clock and a store for the quest state. */
double fake_now = 0;
static double quest_state[12] = {0};
static int quest_state_valid = 0;
double net_quest_now(void) { return fake_now; }
void net_save_quest_state(double t0, double p0, double n0, double x0,
                          double t1, double p1, double n1, double x1,
                          double t2, double p2, double n2, double x2) {
    quest_state[0]=t0; quest_state[1]=p0; quest_state[2]=n0; quest_state[3]=x0;
    quest_state[4]=t1; quest_state[5]=p1; quest_state[6]=n1; quest_state[7]=x1;
    quest_state[8]=t2; quest_state[9]=p2; quest_state[10]=n2; quest_state[11]=x2;
    quest_state_valid = 1;
}
double net_load_quest_state(double slot, double field) {
    int s = (int)slot, f = (int)field;
    if (s < 0 || s > 2 || f < 0 || f > 3) return 0;
    return quest_state[s * 4 + f];
}
double net_quest_has_state(void) { return quest_state_valid; }
void net_set_mode(double v) { (void)v; }
void net_set_room(double v) { (void)v; }
/* Network publishing is unused in solo tests, but update_game links it in. */
void net_publish(double a, double b, double c, double d, double e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}
void net_publish_punch(double a, double b, double c, double d, double e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}
void net_publish_snow(double a, double b, double c, double d, double e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}
void net_publish_turrets(double a, double b, double c, double d, double e,
                         double f, double g, double h, double i, double j) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    (void)f; (void)g; (void)h; (void)i; (void)j;
}
void net_publish_dash(double a, double b, double c, double d, double e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}
void net_publish_universe(double a, double b, double c) { (void)a; (void)b; (void)c; }
void net_publish_thud(double a) { (void)a; }
void net_open(double v) { (void)v; }
void net_connect(const char *a, const char *b) { (void)a; (void)b; }
double net_login_status(void) { return 0; }
void keyboard_hide(void) {}
void keyboard_show(void) {}
const char *keyboard_get_text(void) { return ""; }
int keyboard_visible(void) { return 0; }
int keyboard_enter_pressed(void) { return 0; }
/* String and keyboard helpers for the promo screen (promo.ds). */
double str_len(const char *s) { return s ? (double)strlen(s) : 0.0; }
int str_eq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
int str_starts_with(const char *s, const char *pref) {
    return s && pref && strncmp(s, pref, strlen(pref)) == 0;
}
double str_index_of(const char *hay, const char *needle) {
    if (!hay || !needle) return -1;
    const char *p = strstr(hay, needle);
    return p ? (double)(p - hay) : -1;
}
const char *str_sub(const char *s, double start, double len) {
    static char buf[24];
    if (!s) return "";
    size_t st = (size_t)start, ln = (size_t)len, sl = strlen(s);
    if (st > sl) st = sl;
    if (st + ln > sl) ln = sl - st;
    memcpy(buf, s + st, ln);
    buf[ln] = 0;
    return buf;
}
const char *str_trim(const char *s) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "%s", s ? s : "");
    return buf;
}
const char *str_upper(const char *s) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "%s", s ? s : "");
    for (size_t i = 0; buf[i]; i++)
        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 'a' + 'A');
    return buf;
}
void net_event_set(double mode) { (void)mode; }
double net_status(void) { return 0; }
double net_count(void) { return 0; }
double net_player_online(double s) { int i=(int)s; return i>=0&&i<4?stub_online[i]:0; }
double net_player_x(double s) { (void)s; return 0; }
double net_player_y(double s) { (void)s; return 0; }
double net_player_angle(double s) { (void)s; return 0; }
double net_player_hp(double s) { (void)s; return 0; }
double net_player_alive(double s) { int i=(int)s; return i>=0&&i<4?stub_alive[i]:0; }
const char *net_player_nick(double s) { (void)s; return ""; }
double net_player_punch_x(double s) { (void)s; return 0; }
double net_player_punch_y(double s) { (void)s; return 0; }
double net_player_punch_dx(double s) { (void)s; return 0; }
double net_player_punch_dy(double s) { (void)s; return 0; }
double net_player_punch(double s) { int i=(int)s; return i>=0&&i<4?stub_punch[i]:0; }
double net_player_snow_x(double s) { (void)s; return 0; }
double net_player_snow_y(double s) { (void)s; return 0; }
double net_player_snow_dx(double s) { (void)s; return 0; }
double net_player_snow_dy(double s) { (void)s; return 0; }
double net_player_snow(double s) { (void)s; return 0; }
double net_player_station_x(double s) { (void)s; return 0; }
double net_player_station_y(double s) { (void)s; return 0; }
double net_player_station_hp(double s) { (void)s; return 0; }
double net_player_station2_x(double s) { (void)s; return 0; }
double net_player_station2_y(double s) { (void)s; return 0; }
double net_player_station2_hp(double s) { (void)s; return 0; }
double net_player_station3_x(double s) { (void)s; return 0; }
double net_player_station3_y(double s) { (void)s; return 0; }
double net_player_station3_hp(double s) { (void)s; return 0; }
double net_player_station(double s) { (void)s; return 0; }
double net_player_universe_x(double s) { (void)s; return 0; }
double net_player_universe_y(double s) { (void)s; return 0; }
double net_player_universe(double s) { (void)s; return 0; }
double net_player_thud(double s) { (void)s; return 0; }
double net_player_dash(double s) { (void)s; return 0; }
double net_player_dash_x(double s) { (void)s; return 0; }
double net_player_dash_y(double s) { (void)s; return 0; }
double net_player_dash_dx(double s) { (void)s; return 0; }
double net_player_dash_dy(double s) { (void)s; return 0; }
double net_player_class(double s) { (void)s; return 0; }
double net_player_level(double s) { (void)s; return 0; }

/* Draw capture: remember every primitive call. */
typedef struct { char kind; float x, y, w, h, r, t; uint32_t color; } DrawCall;
static DrawCall calls[2048];
static int call_count;
static void record(char kind, float x, float y, float w, float h, uint32_t color) {
    assert(call_count < 2048);
    calls[call_count].kind = kind;
    calls[call_count].x = x; calls[call_count].y = y;
    calls[call_count].w = w; calls[call_count].h = h;
    calls[call_count].color = color;
    call_count++;
}
void circle(float x, float y, float r, uint32_t c) { record('c', x, y, r, 0, c); }
void ring(float x, float y, float r, float t, uint32_t c) { record('r', x, y, r, t, c); }
void line(float x1, float y1, float x2, float y2, float t, uint32_t c) {
    record('l', x1, y1, x2, y2, c);
    calls[call_count - 1].t = t;   /* the strip thickness is the zone width */
}
void rect(float x, float y, float w, float h, uint32_t c) { record('q', x, y, w, h, c); }
/* Rotated rectangle, used by the straight punch zones and the strips: the angle goes into .t. */
void rect_rot(float x, float y, float w, float h, float ang, uint32_t c) {
    record('t', x, y, w, h, c);
    calls[call_count - 1].t = ang;
}
/* Both menus and dash zones use roundrect, so the call is kept with its radius. */
void roundrect(float x, float y, float w, float h, float r, uint32_t color) {
    record('o', x, y, w, h, color);
    calls[call_count - 1].r = r;
}

static int count_kind_color(char kind, uint32_t color) {
    int n = 0;
    for (int i = 0; i < call_count; i++) if (calls[i].kind == kind && calls[i].color == color) n++;
    return n;
}
static void near(double a, double b) {
    if (fabs(a - b) >= 1e-6) { fprintf(stderr, "near fail: %.9f vs %.9f\n", a, b); abort(); }
}

static void own_turret(int i, double x, double y, double hp) {
    arr_set(turret_x, i, x); arr_set(turret_y, i, y);
    arr_set(turret_hp, i, hp); arr_set(turret_maxhp, i, hp);
    arr_set(turret_spawn_t, i, 0);
}
static void foe_turret(int i, double x, double y, double hp) {
    arr_set(enemy_turret_x, i, x); arr_set(enemy_turret_y, i, y);
    arr_set(enemy_turret_hp, i, hp); arr_set(enemy_turret_maxhp, i, hp);
    arr_set(enemy_turret_spawn_t, i, 0);
}

static void test_shield_player(void) {
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    player_class = CLASS_EBUC;
    enemy_class = CLASS_ORDINARY;
    player->x = 500; player->y = 400; player->size = 25;
    enemy->x = 900; enemy->y = 400; enemy->size = 25;
    player->hp = 20; player->max_hp = 20;
    enemy->hp = 999; enemy->max_hp = 999;
    own_turret(0, 500, 460, 10);
    ds_fn_take_damage(4);
    near(arr_get(turret_hp, 0), 6);
    near(player->hp, 20);           /* the turret took all the damage */
    ds_fn_take_damage(8);
    near(arr_get(turret_hp, 0), 0); /* the turret is destroyed */
    near(player->hp, 18);           /* the rest of the damage reached the ebuC */
    ds_fn_take_damage(3);
    near(player->hp, 15);           /* without turrets the damage lands directly */
    assert(arr_len(station_boom_ts) == 1);
    /* Damage online, from a remote universe, goes through the same shield. */
    own_turret(0, 500, 460, 9);
    ds_fn_universe_collapse_remote(5);
    near(arr_get(turret_hp, 0), 4);
    near(player->hp, 15);
    puts("shield: ebuC turret absorbs player damage (solo+remote), overflow hits ebuC");
}

static void test_shield_enemy(void) {
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    enemy_class = CLASS_EBUC;
    player_class = CLASS_ORDINARY;
    player->x = 500; player->y = 500; player->size = 25;
    enemy->x = 900; enemy->y = 400; enemy->size = 25;
    player->hp = 999; player->max_hp = 999;
    enemy->hp = 50; enemy->max_hp = 50;
    foe_turret(0, 900, 340, 10);
    ds_fn_enemy_apply_damage(6);
    near(arr_get(enemy_turret_hp, 0), 4);
    near(enemy->hp, 50);
    ds_fn_enemy_apply_damage(5);
    near(arr_get(enemy_turret_hp, 0), 0);
    near(enemy->hp, 49);
    ds_fn_enemy_apply_damage(10);
    near(enemy->hp, 39);
    /* The ebuC super in solo damages the enemy turrets and its own. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    enemy_class = CLASS_EBUC;
    player_class = CLASS_EBUC;
    player->x = 500; player->y = 500; player->size = 25;
    enemy->x = 900; enemy->y = 400; enemy->size = 25;
    player->hp = 30; player->max_hp = 30;
    enemy->hp = 30; enemy->max_hp = 30;
    own_turret(0, 500, 460, 8);
    foe_turret(0, 900, 340, 8);
    ds_fn_universe_collapse(6);
    near(arr_get(turret_hp, 0), 2);      /* our own universe hits our own shield too */
    near(player->hp, 30);                /* the damage is fully absorbed */
    near(arr_get(enemy_turret_hp, 0), 2);
    near(enemy->hp, 30);
    ds_fn_universe_collapse(10);
    near(arr_get(turret_hp, 0), 0);
    near(player->hp, 22);                /* the rest, 10 minus 2, reached the ebuC */
    near(arr_get(enemy_turret_hp, 0), 0);
    near(enemy->hp, 22);
    puts("shield: enemy ebuC turret absorbs damage, own universe absorbed by own shield");
}

static void test_snow_pierce_enemy_turrets(void) {
    /* The player's snowflake flies at the enemy ebuC and pierces its turret. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    player_class = CLASS_SANTA;
    enemy_class = CLASS_EBUC;
    player->x = 300; player->y = 400; player->size = 25;
    enemy->x = 900; enemy->y = 400; enemy->size = 25;
    player->hp = 999; player->max_hp = 999;
    enemy->hp = 500; enemy->max_hp = 500;
    foe_turret(0, 600, 400, 100);  /* right on the path, with more HP than the damage */
    gift->x = player->x; gift->y = player->y;
    gift->dx = 1; gift->dy = 0;
    gift->active = 1; gift->t = 0; gift->pierce = 0;
    double before = arr_get(enemy_turret_hp, 0);
    int exploded = 0;
    for (int i = 0; i < 2000 && gift->active == 1; i++) {
        double px = gift->x;
        ds_fn_update_gift();
        if (!gift->active) exploded = 1;
        (void)px;
    }
    assert(exploded == 1);                 /* it arrived and exploded in the end */
    /* The turret is repaired once per flight through the pierce bit, not every frame. */
    near(before - arr_get(enemy_turret_hp, 0), santa_super_damage);
    puts("snow: pierces enemy turret (single chip) and keeps flying to the ebuC");
}

static void test_snow_pierce_player_turrets(void) {
    /* The enemy snowflake flies at the player and pierces the player's turret. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    enemy_class = CLASS_SANTA;
    player_class = CLASS_ORDINARY;
    player->x = 900; player->y = 400; player->size = 25;
    enemy->x = 300; enemy->y = 400; enemy->size = 25;
    player->hp = 999; player->max_hp = 999;
    enemy->hp = 500; enemy->max_hp = 500;
    own_turret(0, 600, 400, 100);
    ds_fn_enemy_start_snow();
    double before = arr_get(turret_hp, 0);
    int exploded = 0;
    for (int i = 0; i < 2000 && enemy_gift->active == 1; i++) {
        ds_fn_tick_enemy_gift();
        if (!enemy_gift->active) exploded = 1;
    }
    assert(exploded == 1);
    near(before - arr_get(turret_hp, 0), santa_super_damage);
    puts("snow: enemy snow pierces player turret once and flies on");
}

static void test_dash_real_length(void) {
    /* An online dash hits only the segment it has already travelled. */
    ds_fn_reset_battle();
    game_state = ST_ONLINE;
    player_class = CLASS_ORDINARY;
    player->y = 500; player->size = 25;
    player->hp = 999; player->max_hp = 999;
    enemy->x = 100; enemy->y = 100; enemy->size = 25;
    double total = azum_dash_speed * azum_dash_time;  /* the whole dash path */
    assert(total > 0);
    /* The turret sits halfway and the player is a little before the end of the path. */
    player->x = 200 + total - 40;
    own_turret(0, 200 + total * 0.5, 500, 50);
    assert(ds_fn_dash_resolve_target(200, 500, 1, 0, total * 0.2) == -1);
    double got = ds_fn_dash_resolve_target(200, 500, 1, 0, total * 0.7);
    assert(got == 1);                                  /* the turret is on the path */
    got = ds_fn_dash_resolve_target(200, 500, 1, 0, total);
    assert(got == 1);                                  /* the shield is ahead of the player */
    arr_set(turret_hp, 0, 0);                          /* the turret is dead */
    assert(ds_fn_dash_resolve_target(200, 500, 1, 0, total) == 0);
    puts("dash: damage only on the traveled segment, turret shield priority");
}

static void test_hitbox_fades(void) {
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    hitbox_fade_in = 0.1; hitbox_fade_out = 0.2;
    player->x = 500; player->y = 400; player->size = 25;
    enemy->x = 900; enemy->y = 400; enemy->size = 25;
    dt = 0.05;
    /* Dash: the alpha grows smoothly. */
    dash_active = 1; dash_box_a = 0;
    for (int i = 0; i < 4; i++) ds_fn_tick_hitbox_fades();
    near(dash_box_a, 1);
    /* After the end it fades out by 0.05 of 0.2 per frame. */
    dash_active = 0;
    ds_fn_tick_hitbox_fades();
    near(dash_box_a, 0.75);
    ds_fn_tick_hitbox_fades();
    near(dash_box_a, 0.5);
    for (int i = 0; i < 20; i++) ds_fn_tick_hitbox_fades();
    near(dash_box_a, 0);
    /* Snowflake: the projectile and the explosion appear and fade. */
    gift->active = 1; snow_ball_a = 0;
    boom_t = 1; boom_x = 300; boom_y = 300; snow_boom_a = 0;
    for (int i = 0; i < 4; i++) ds_fn_tick_hitbox_fades();
    near(snow_ball_a, 1); near(snow_boom_a, 1);
    gift->active = 0; boom_t = 0;
    for (int i = 0; i < 20; i++) ds_fn_tick_hitbox_fades();
    near(snow_ball_a, 0); near(snow_boom_a, 0);
    /* The turret builds up its alpha and fades out on death. */
    own_turret(0, 500, 460, 10);
    for (int i = 0; i < 4; i++) ds_fn_tick_hitbox_fades();
    near(arr_get(turret_box_a, 0), 1);
    arr_set(turret_hp, 0, 0);
    for (int i = 0; i < 20; i++) ds_fn_tick_hitbox_fades();
    near(arr_get(turret_box_a, 0), 0);
    puts("hitboxes: ability alphas fade in and out smoothly like the punch box");
}

static void test_hitbox_drawing(void) {
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    show_hitboxes = 1;
    player_class = CLASS_SANTA;
    enemy_class = CLASS_ORDINARY;
    player->x = 400; player->y = 400; player->size = 25;
    enemy->x = 1000; enemy->y = 400; enemy->size = 25;
    enemy->hp = 999; enemy->max_hp = 999;
    player->hp = 999; player->max_hp = 999;
    uint32_t dark = 0x00000000;            /* hitbox_rgb: dark, as before */
    uint32_t zone = (96u << 24) | dark;    /* hitbox_zone_alpha: one for every zone */
    /* Snowflake: the projectile and the explosion share one colour and one alpha,
     * and the path is a straight strip (rect_rot) with no round line() caps and no
     * ring() outlines over the fill. */
    gift->active = 1; snow_ball_a = 1;
    gift->x = 400; gift->y = 400; gift->dx = 1; gift->dy = 0; gift->t = 0;
    boom_t = 1; boom_x = 700; boom_y = 400; snow_boom_a = 1;
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    assert(count_kind_color('c', zone) >= 2);   /* the projectile and the explosion are filled */
    assert(count_kind_color('t', zone) >= 1);   /* the path strip */
    assert(count_kind_color('r', zone) == 0);   /* no outlines are left */
    /* At an alpha of 0.5 the zone is translucent, so the fade in works. */
    snow_ball_a = 0.5;
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    assert(count_kind_color('c', (48u << 24) | dark) >= 1);
    gift->active = 0; boom_t = 0;
    snow_ball_a = 0; snow_boom_a = 0;
    /* Dash: the damage zone is one strip (rect_rot) along the travelled segment,
     * with the colour and alpha of every other zone. Its width is the damage
     * diameter, so the drawing matches the damage. */
    dash_active = 1; dash_box_a = 1;
    dash_x0 = 300; dash_y0 = 400; dash_dx = 1; dash_dy = 0;
    player->x = 700; player->y = 400;
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    double pr = ds_fn_dash_hit_radius_solo();    /* the same radius as the damage */
    double path = azum_dash_speed * azum_dash_time;   /* 382.5 out of a 400 path */
    double travel = player->x - dash_x0;         /* travelled 400, so the whole path */
    if (travel > path) travel = path;
    assert(count_kind_color('t', zone) == 1);    /* exactly one strip */
    assert(count_kind_color('q', zone) == 0);    /* the grid of squares is gone */
    assert(count_kind_color('o', zone) == 0);    /* no roundrect in the hitboxes */
    assert(count_kind_color('l', zone) == 0);
    assert(count_kind_color('c', zone) == 0);
    for (int i = 0; i < call_count; i++) {
        if (calls[i].kind != 't' || calls[i].color != zone) continue;
        assert(fabs(calls[i].h - 2 * pr) < 1e-3);          /* the width is the zone diameter */
        assert(fabs(calls[i].y - (400 - pr)) < 1e-3);      /* the strip lies on the dash axis */
        assert(fabs((calls[i].x + calls[i].w / 2) - (dash_x0 + travel / 2)) < 1e-3);
    }
    /* A solo bot dash uses the same radius, colour and strip. */
    dash_active = 0; dash_box_a = 0;
    enemy_dash_active = 1; edash_box_a = 1;
    enemy_dash_x0 = 1100; enemy_dash_y0 = 400; enemy_dash_dx = -1; enemy_dash_dy = 0;
    enemy->x = 800; enemy->y = 400;
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    assert(count_kind_color('t', zone) == 1);
    assert(count_kind_color('q', zone) == 0);
    assert(count_kind_color('o', zone) == 0);
    assert(count_kind_color('l', zone) == 0);
    assert(count_kind_color('c', zone) == 0);
    enemy_dash_active = 0;
    /* No zone square is drawn around the dispenser anymore, neither a fill nor an
     * outline: the turret keeps only its shadow sprite. */
    edash_box_a = 0;
    own_turret(0, 500, 460, 10);
    arr_set(turret_box_a, 0, 1);
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    assert(count_kind_color('q', zone) == 0);  /* no square fill */
    assert(count_kind_color('o', zone) == 0);  /* no rounded fill */
    assert(count_kind_color('t', zone) == 0);  /* no strips */
    assert(count_kind_color('l', zone) == 0);  /* no capsules */
    assert(count_kind_color('r', zone) == 0);  /* no round outline */
    assert(count_kind_color('c', zone) == 0);  /* no circular fill */
    puts("hitboxes: one transparent colour everywhere, sharp corners only");
}

static void test_poison_green(void) {
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    enemy->x = 100; enemy->y = 100; enemy->size = 25;
    player->x = 300; player->y = 100; player->size = 25;
    /* Poison is green, so it does not blend into the blue freeze. */
    poison_a = 1;
    call_count = 0;
    ds_fn_draw_poison();
    assert(count_kind_color('c', (76u << 24) | 0xC853) == 1);     /* 0x00C853 */
    assert(count_kind_color('r', (178u << 24) | 0x69F0AE) == 1);
    ppoison_a = 1;
    call_count = 0;
    ds_fn_draw_player_poison();
    assert(count_kind_color('c', (76u << 24) | 0xC853) == 1);
    assert(count_kind_color('r', (178u << 24) | 0x69F0AE) == 1);
    /* Freeze stays blue. */
    freeze_a = 1;
    call_count = 0;
    ds_fn_draw_freeze();
    assert(count_kind_color('c', (76u << 24) | 0x1E88E5) == 1);
    assert(count_kind_color('r', (178u << 24) | 0x4FC3F7) == 1);
    puts("poison: green vs blue freeze — the two effects no longer look doubled");
}

static void test_splash_screens(void) {
    /* The epilepsy warning no longer fades on a timer: the screen stays until
     * consent (legal_accept in the scripts), otherwise the warning could be slept
     * through. */
    ds_fn_reset_battle();
    warn_open = 1; warn_a = 1; warn_ready = 0; warn_t = 0; warn_closing = 0;
    studio_open = 0;
    dt = 0.1;
    ds_fn_legal_accept();
    assert(warn_open == 1 && studio_open == 0);
    for (int i = 0; i < 15; i++) ds_fn_update_warning();
    near(warn_ready, 0.5);
    ds_fn_touch_warn(screen_w / 2, ds_fn_warn_btn_y() + 20, 0);
    assert(warn_open == 1 && studio_open == 0);
    for (int i = 0; i < 25; i++) ds_fn_update_warning();
    near(warn_ready, 1);
    assert(legal_marks == 0);
    ds_fn_touch_warn(screen_w / 2, ds_fn_warn_btn_y() + 20, 1);
    assert(legal_marks == 0);
    ds_fn_touch_warn(screen_w / 2, ds_fn_warn_btn_y() + 20, 0);
    /* Consent starts the smooth fade and no studio splash. */
    assert(legal_marks == 1 && warn_open == 1 && warn_closing == 1 && studio_open == 0);
    ds_fn_update_warning();
    assert(warn_a > 0 && warn_a < 1 && warn_open == 1);
    for (int i = 0; i < 10; i++) ds_fn_update_warning();
    assert(warn_a == 0 && warn_open == 0 && warn_closing == 0);
    /* While fading, the background, the text and the button share one alpha. */
    warn_open = 1; warn_a = 0.5; warn_closing = 0;
    call_count = 0;
    ds_fn_draw_warning();
    assert(calls[0].kind == 'q' && calls[0].color == 0x80000000);
    int saw_accept_btn = 0;
    for (int i = 0; i < call_count; i++) {
        if (calls[i].kind == 'o' && calls[i].color == 0x80000000) saw_accept_btn = 1;
    }
    assert(saw_accept_btn);
    /* The button starts fully lit while its label counts "(3)(2)(1)"; on the
     * fourth second the seconds disappear and the button accepts a tap. */
    warn_a = 1; warn_ready = 0; warn_t = 0.0;
    call_count = 0;
    ds_fn_draw_warning();
    assert(strstr(last_text, "(3)") != NULL);          /* first second */
    for (int i = 0; i < call_count; i++)
        if (calls[i].kind == 'o')
            assert(calls[i].color == 0xFFFFFFFF || calls[i].color == 0xFF000000); /* strictly monochrome */
    warn_t = 1.5; warn_ready = 0.5;
    ds_fn_draw_warning();
    assert(strstr(last_text, "(2)") != NULL);          /* second second */
    warn_t = 2.5; warn_ready = 0.83;
    ds_fn_draw_warning();
    assert(strstr(last_text, "(1)") != NULL);          /* third second */
    warn_t = 3.0; warn_ready = 1.0;
    ds_fn_draw_warning();
    assert(strstr(last_text, "(") == NULL);            /* the seconds are gone */
    int marks_before = legal_marks;
    ds_fn_touch_warn(screen_w / 2, ds_fn_warn_btn_y() + 20, 0);
    assert(legal_marks == marks_before + 1 && warn_open == 1 && warn_closing == 1 && studio_open == 0);
    puts("splash: warning smoothly fades after consent, no studio splash");
}

static void test_quest_cooldown(void) {
    ds_main();
    ds_fn_quest_init();
    /* A finished quest (type 0, target reached) and a pinned now. */
    fake_now = 1000;
    arr_set(quest_type, 0, 0);
    arr_set(quest_prog, 0, 1);
    arr_set(quest_need, 0, 1);
    int cups0 = cups;
    int cand0 = candies;
    ds_fn_quest_complete(0);
    /* The reward is cut down to quest_cups and quest_candies. */
    assert(cups == cups0 + (int)quest_cups);
    assert(candies == cand0 + (int)quest_candies);
    /* The slot went into cooldown and the stamp is stored natively, surviving a restart. */
    assert(arr_get(quest_type, 0) == -1);
    near(arr_get(quest_next, 0), 1000 + quest_respawn);
    near(quest_state[3], 1000 + quest_respawn);
    /* No new quest appears before the five minutes are up. */
    fake_now = 1000 + quest_respawn - 1;
    ds_fn_quest_tick();
    assert(arr_get(quest_type, 0) == -1);
    /* After quest_respawn real seconds a new quest appears. */
    fake_now = 1000 + quest_respawn;
    ds_fn_quest_tick();
    assert(arr_get(quest_type, 0) != -1);
    near(arr_get(quest_next, 0), 0);
    near(quest_state[3], 0);
    puts("quests: next appears after respawn, timer survives via native store");
}

static void test_punch_hitbox_fades(void) {
    /* The bot punch strip no longer flashes for a single frame: it has its own alpha. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    dt = 0.05;
    aim_fade_in = 0.09; aim_fade_out = 0.1;
    show_hitboxes = 1;
    enemy->state = 2;                       /* the bot punched */
    ds_fn_tick_hitbox_fades();
    assert(enemy_punch_a > 0 && enemy_punch_a < 1);
    for (int i = 0; i < 10; i++) ds_fn_tick_hitbox_fades();
    near(enemy_punch_a, 1);
    enemy->state = 0;
    ds_fn_tick_hitbox_fades();
    assert(enemy_punch_a > 0 && enemy_punch_a < 1);
    for (int i = 0; i < 10; i++) ds_fn_tick_hitbox_fades();
    near(enemy_punch_a, 0);
    /* It is drawn with that alpha rather than with one. */
    enemy_punch_a = 0.5;
    enemy->x = 400; enemy->y = 300; enemy->angle = 0;
    call_count = 0;
    ds_fn_draw_enemy_hitbox();
    assert(count_kind_color('t', (48u << 24) | 0x00000000) == 1);  /* floor(0.5*96) */
    assert(count_kind_color('l', (48u << 24) | 0x00000000) == 0);  /* no line() capsules */
    /* End of the battle: every hitbox fades out instead of vanishing at once. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    dash_active = 1; dash_box_a = 1;
    enemy->state = 1; enemy_punch_a = 1;
    own_turret(0, 500, 460, 10);
    arr_set(turret_box_a, 0, 1);
    finished = 1;
    ds_fn_tick_hitbox_fades();
    assert(dash_box_a > 0 && dash_box_a < 1);
    assert(enemy_punch_a > 0 && enemy_punch_a < 1);
    assert(arr_get(turret_box_a, 0) > 0 && arr_get(turret_box_a, 0) < 1);
    for (int i = 0; i < 20; i++) ds_fn_tick_hitbox_fades();
    near(dash_box_a, 0); near(enemy_punch_a, 0); near(arr_get(turret_box_a, 0), 0);
    puts("hitboxes: bot punch box fades, everything fades out at the end of battle");
}

static void test_status_circles(void) {
    /* Status effects the enemy put on the fighter use the same even circle and
     * fade out the same way. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    dt = 0.05;
    freeze_fade_in = 0.12; freeze_fade_out = 0.35;
    player->size = 45; enemy->size = 45;
    player->x = 300; player->y = 300;
    enemy->x = 900; enemy->y = 300;
    player_freeze = 1; player_poison = 1; player_stun = 1;
    enemy->freeze = 1; enemy->poison = 1; enemy->stun = 1;
    for (int i = 0; i < 10; i++) ds_fn_tick_status_fades();
    near(pfreeze_a, 1); near(ppoison_a, 1); near(pstun_a, 1);
    near(freeze_a, 1); near(poison_a, 1); near(stun_a, 1);
    player_freeze = 0; player_poison = 0; player_stun = 0;
    enemy->freeze = 0; enemy->poison = 0; enemy->stun = 0;
    ds_fn_tick_status_fades();
    assert(pfreeze_a > 0 && pfreeze_a < 1);
    assert(ppoison_a > 0 && ppoison_a < 1);
    assert(pstun_a > 0 && pstun_a < 1);
    for (int i = 0; i < 20; i++) ds_fn_tick_status_fades();
    near(pfreeze_a, 0); near(ppoison_a, 0); near(pstun_a, 0);
    near(freeze_a, 0); near(poison_a, 0); near(stun_a, 0);
    /* The circle around the fighter is drawn in solo (draw_game) in the enemy colour. */
    pfreeze_a = 1; ppoison_a = 1; pstun_a = 1;
    call_count = 0;
    ds_fn_draw_game();
    assert(count_kind_color('c', (76u << 24) | 0x1E88E5) == 1);   /* freeze */
    assert(count_kind_color('c', (76u << 24) | 0xC853) == 1);     /* poison */
    assert(count_kind_color('c', (76u << 24) | 0xF9A825) == 1);   /* stun */
    /* At the end of the battle the status timers stop, but the circles still fade. */
    player_freeze = 2; pfreeze_a = 1;
    finished = 2;
    ds_fn_tick_status_fades();
    assert(pfreeze_a > 0 && pfreeze_a < 1);
    for (int i = 0; i < 20; i++) ds_fn_tick_status_fades();
    near(pfreeze_a, 0);
    puts("status: enemy-applied effects circle the fighter and fade out smoothly");
}

static void test_dash_zone_narrow(void) {
    /* The dash zone is one and the same in damage and in drawing: its radius follows
     * a scale. That scale stays under one, so the dash is still aimed, yet it is
     * noticeably above the old 0.75 and lands more often. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    player->size = 45; enemy->size = 45;
    double old_r = player->size * 0.65 + enemy->size * 0.65 + 12;
    double pr = ds_fn_dash_hit_radius_solo();
    near(pr, old_r * dash_hit_radius_scale);
    assert(dash_hit_radius_scale < 1.0);
    assert(dash_hit_radius_scale >= 0.85);
    assert(pr < old_r);   /* the dash zone is narrower than it used to be */
    /* A dash passing between the old and the new radius no longer hits. */
    game_state = ST_ONLINE;
    double remote_old = player->size * 0.65 + 14;
    double remote_pr = ds_fn_dash_hit_radius_remote();
    near(remote_pr, remote_old * dash_hit_radius_scale);
    player->x = 500; player->y = 500 + (remote_old + remote_pr) / 2;
    assert(ds_fn_dash_resolve_target(200, 500, 1, 0, 600) == -1);
    player->y = 500 + remote_pr - 1;
    assert(ds_fn_dash_resolve_target(200, 500, 1, 0, 600) == 0);
    puts("dash: narrower damage zone, the drawn strip follows it");
}

static void test_own_punch_spares_own_turret(void) {
    /* Our own dispenser does not intercept our punch: it stands on the ebuC and
     * lets the punch pass into empty air with the turret intact and the distant
     * enemy unharmed, while an enemy on the strip takes the damage. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    player_class = CLASS_EBUC;
    enemy_class = CLASS_ORDINARY;
    dt = 0.05;
    player->x = 400; player->y = 400; player->size = 25; player->angle = 0;
    enemy->x = 900; enemy->y = 400; enemy->size = 25;
    player->hp = 20; player->max_hp = 20;
    enemy->hp = 999; enemy->max_hp = 999;
    own_turret(0, 400, 400, 9);
    enemy->cooldown = 5; enemy->think = 5;      /* the bot stays out of it */
    punch->active = 1; punch->hit = 0; punch_left = punch_time;
    ds_fn_update_game();
    near(arr_get(turret_hp, 0), 9);             /* our own punch does not chew on the dispenser */
    near(enemy->hp, 999);                       /* and does not hit the enemy through nothing */
    assert(punch->hit == 0);
    enemy->x = 460; enemy->y = 400;             /* the enemy is on the punch strip */
    punch->active = 1; punch->hit = 0; punch_left = punch_time;
    ds_fn_update_game();
    near(enemy->hp, 999 - ebuc_damage);         /* the damage reached the enemy */
    near(arr_get(turret_hp, 0), 9);             /* the dispenser is intact */
    assert(punch->hit == 1);
    puts("punch: ebuC's own despenser never eats his punch, damage reaches the foe");
}

static void test_enemy_shield_front_only(void) {
    /* An enemy ebuC's dispenser only guards the front: a turret behind his back
     * does not take a punch to the face, so the enemy takes the damage. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    player_class = CLASS_ORDINARY;
    enemy_class = CLASS_EBUC;
    player->x = 500; player->y = 400; player->size = 25; player->angle = 0;
    enemy->x = 600; enemy->y = 400; enemy->size = 25;
    player->hp = 999; player->max_hp = 999;
    enemy->hp = 50; enemy->max_hp = 50;
    /* With a turret between the player and the enemy the shield takes the punch. */
    foe_turret(0, 560, 400, 10);
    assert(ds_fn_enemy_punch_turret_target(500, 400, 1, 0) == 1);
    /* A turret on the punch strip but behind the body, past the 30 px tolerance,
     * is no shield: there is no direct chip and no counter hit, and the punch lands
     * on the enemy's face, where the class rule makes absorb take the damage. */
    arr_set(enemy_turret_x, 0, 632);
    assert(ds_fn_enemy_punch_turret_target(500, 400, 1, 0) == -1);
    enemy->cooldown = 5; enemy->think = 5;
    punch->x = 500; punch->y = 400; punch->dx = 1; punch->dy = 0;
    punch->active = 1; punch->hit = 0; punch_left = punch_time;
    ds_fn_update_game();
    assert(punch->hit == 1);                    /* the punch to the face landed */
    near(arr_get(enemy_turret_hp, 0), 9);       /* absorb took the damage */
    near(player->hp, 999);                      /* there is no counter hit */
    /* A turret in front gives a direct chip into the shield plus a counter hit on the player. */
    arr_set(enemy_turret_x, 0, 560);
    punch->active = 1; punch->hit = 0; punch_left = punch_time;
    ds_fn_update_game();
    near(arr_get(enemy_turret_hp, 0), 8);
    near(player->hp, 999 - turret_counter_damage);
    puts("shield: enemy despenser covers only from the front, face punches land");
}

static void test_online_turret_punch_death(void) {
    /* Online ebuC: a remote punch hits a turret directly through damage_turret,
     * while a punch at the player is absorbed by the shield of turrets. Either way
     * the turret loses HP and dies in the end, with the explosion queued. */
    ds_fn_reset_battle();
    game_state = ST_ONLINE;
    player_class = CLASS_EBUC;
    dt = 0.05;
    player->x = 500; player->y = 400; player->size = 25;
    player->hp = 20; player->max_hp = 20;
    /* Slot 1 holds a live remote: an ordinary class with a damage of 1. */
    arr_set(remotes, 1*remote_fields+5, 1);
    arr_set(remotes, 1*remote_fields+10, CLASS_ORDINARY);
    double rb = 1*punch_fields;
    /* 1) A punch right at the player, with the turret far from the strip: the shield absorbs it. */
    own_turret(0, 500, 560, 9);
    arr_set(remote_punches, rb, 1);
    arr_set(remote_punches, rb+1, 420);
    arr_set(remote_punches, rb+2, 400);
    arr_set(remote_punches, rb+3, 1);
    arr_set(remote_punches, rb+4, 0);
    ds_fn_update_remote_punches();
    near(player->hp, 20);                      /* the player is unharmed */
    near(arr_get(turret_hp, 0), 8);            /* the shield took the punch */
    assert(arr_get(remote_punches, rb) == 0);  /* the punch was handled */
    /* 2) Direct hits on the turret: chip after chip, dead after nine punches. */
    arr_set(turret_x, 0, 500); arr_set(turret_y, 0, 460); arr_set(turret_hp, 0, 9);
    arr_set(remote_punches, rb, 1);
    ds_fn_update_remote_punches();
    near(arr_get(turret_hp, 0), 8);
    for (int i = 0; i < 8; i++) {
        arr_set(remote_punches, rb, 1);
        ds_fn_update_remote_punches();
    }
    near(arr_get(turret_hp, 0), 0);
    assert(arr_len(station_boom_ts) == 1);
    puts("online: friend punch chips and kills ebuC turret (shield absorb + direct hit)");
}

static void test_punch_sprite_never_empty(void) {
    /* tex() draws nothing for a missing texture, so an empty swing sprite meant an
     * invisible fighter who left the arena while punching. The swing pose now falls
     * back to the ordinary sprite of the class. */
    ds_fn_reset_battle();
    punch_tex_broken = 1;
    ds_fn_load_textures();
    assert(ordinary_punch_tex_ok == 0 && azum_punch_tex_ok == 0);
    assert(santa_punch_tex_ok == 0 && ebuc_punch_tex_ok == 0);
    assert(strcmp(ds_fn_fighter_sprite(CLASS_ORDINARY, 1, SKIN_NORMAL), "ordinary.png") == 0);
    assert(strcmp(ds_fn_fighter_sprite(CLASS_AZUM, 1, SKIN_NORMAL), "azum.png") == 0);
    assert(strcmp(ds_fn_fighter_sprite(CLASS_SANTA, 1, SKIN_NORMAL), "santa.png") == 0);
    assert(strcmp(ds_fn_fighter_sprite(CLASS_EBUC, 1, SKIN_NORMAL), "ebuc.png") == 0);
    /* The zombie skin has a fallback too: zombie swing, then Azum, then ordinary. */
    azum_zombie_punch_tex_ok = 0;
    assert(strcmp(ds_fn_fighter_sprite(CLASS_AZUM, 1, SKIN_ZOMBIE), "azum.png") == 0);
    azum_zombie_punch_tex_ok = 1;
    punch_tex_broken = 0;
    ds_fn_load_textures();
    /* Everything is in place, so the real swing sprites are used as before. */
    assert(ordinary_punch_tex_ok == 1 && azum_punch_tex_ok == 1);
    assert(strcmp(ds_fn_fighter_sprite(CLASS_ORDINARY, 1, SKIN_NORMAL), "ordinary_punch.png") == 0);
    assert(strcmp(ds_fn_fighter_sprite(CLASS_AZUM, 1, SKIN_NORMAL), "azum_punch.png") == 0);
    assert(strcmp(ds_fn_fighter_sprite(CLASS_SANTA, 1, SKIN_NORMAL), "santa_punch.png") == 0);
    assert(strcmp(ds_fn_fighter_sprite(CLASS_EBUC, 1, SKIN_NORMAL), "ebuc_punch.png") == 0);
    assert(strcmp(ds_fn_fighter_sprite(CLASS_AZUM, 1, SKIN_ZOMBIE), "zombie_azum_punch.png") == 0);
    assert(strcmp(ds_fn_fighter_sprite(CLASS_ORDINARY, 0, SKIN_NORMAL), "ordinary.png") == 0);
    puts("sprite: an unloaded punch texture falls back to the idle one, the fighter stays visible");
}

static void test_remote_punch_pose_expires(void) {
    /* A remote swing pose lives punch_time and fades by itself. Only
     * update_remote_punches() used to clear remote_punches, and neither the end of
     * the battle nor a lost slot reaches it, so a remote stayed mid swing forever,
     * on the results screen and beyond. */
    ds_fn_reset_battle();
    game_state = ST_ONLINE;
    dt = 1.0 / 60.0;
    stub_slot = 0;
    stub_online[1] = 1; stub_alive[1] = 1; stub_punch[1] = 0;
    arr_set(remotes, 1*remote_fields, 1);          /* the slot is already visible */
    arr_set(remotes, 1*remote_fields+5, 1);
    stub_punch[1] = 3;                             /* the remote punched */
    ds_fn_read_remotes();
    assert(arr_get(remote_punches, 1*punch_fields) == 1);
    finished = 1;                                  /* the battle ended in the same frame */
    int frames = 0;
    while (arr_get(remote_punches, 1*punch_fields) == 1 && frames < 120) {
        ds_fn_tick_remote_punches();
        frames++;
    }
    assert(arr_get(remote_punches, 1*punch_fields) == 0);
    assert(frames > 5 && frames < 30);             /* punch_time rather than forever */
    finished = 0;
    /* finish_game() drops remote punches at once, as it does with the other effects. */
    cups_awarded = 1;                              /* no rewards and no saving */
    stub_punch[1] = 5;
    ds_fn_read_remotes();
    assert(arr_get(remote_punches, 1*punch_fields) == 1);
    arr_set(rpunch_a, 1, 1);
    ds_fn_finish_game(1);
    assert(arr_get(remote_punches, 1*punch_fields) == 0);
    assert(arr_get(rpunch_a, 1) == 0);
    stub_online[1] = 0; stub_alive[1] = 0; stub_punch[1] = 0; stub_slot = -1;
    puts("online: the punch pose expires on its own, finished match included");
}

static void test_no_phantom_punch_on_join(void) {
    /* The punch counter lives in the room and never resets, while our last seen
     * value is zero after reset_battle(), so the first snapshot of a new game used
     * to count as a punch and the remote was drawn mid swing, with his punch from
     * the last match even hurting the player. The snapshot is compared now. */
    ds_fn_reset_battle();
    game_state = ST_ONLINE;
    online_ready = 1;
    dt = 1.0 / 60.0;
    stub_slot = 0;
    stub_online[1] = 1; stub_alive[1] = 1;
    stub_punch[1] = 7;                             /* the counter from the last match */
    player->x = 400; player->y = 360; player->size = 25;
    player->hp = 10; player->max_hp = 10;
    finished = 0;
    ds_fn_read_remotes();
    assert(arr_get(remote_punches, 1*punch_fields) == 0);   /* no swing pose */
    assert(arr_get(remotes, 1*remote_fields+6) == 7);       /* the counter is remembered */
    ds_fn_update_remote_punches();
    near(player->hp, 10);                                   /* no remote damage */
    /* The real next punch still shows up as before. */
    stub_punch[1] = 8;
    ds_fn_read_remotes();
    assert(arr_get(remote_punches, 1*punch_fields) == 1);
    stub_online[1] = 0; stub_alive[1] = 0; stub_punch[1] = 0; stub_slot = -1;
    puts("online: a stale punch counter is not a new punch, the next one still is");
}

static void test_turret_shadow_square(void) {
    /* The dispenser shadow is the dispenser sprite through tex_tint, with no
     * circles and no rings under the turret. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    show_hitboxes = 1;
    own_turret(0, 500, 460, 10);
    call_count = 0; tint_calls = 0;
    ds_fn_draw_turret_shadows();
    assert(tint_calls == 1);
    assert(strcmp(tint_last, "despenser.png") == 0);
    assert(call_count == 0);                    /* the shadow is a sprite, not a circle */
    arr_set(turret_box_a, 0, 1);
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    assert(call_count == 0);   /* the zone square around the dispenser is gone */
    puts("turret: square sprite shadow stays, no hit square around the despenser");
}

static void test_station_beam_stable(void) {
    /* The ebuC beam is solid and never blinks: it is drawn even right next to the
     * turret, where it used to be cut off below d<40, its alpha is always opaque,
     * and at the end of the battle it fades once and never returns. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    player_class = CLASS_EBUC;
    enemy_class = CLASS_EBUC;
    dt = 0.05;
    player->x = 500; player->y = 400; player->hp = 10; player->max_hp = 10;
    enemy->x = 900; enemy->y = 400; enemy->hp = 10; enemy->max_hp = 10;
    uint32_t outer = (255u << 24) | 0x0000E676;   /* station_beam_alpha + rgb */
    uint32_t core = (255u << 24) | 0x00C8FAD6;    /* the solid beam core */
    finished = 0;
    own_turret(0, 512, 416, 6);                   /* d=20, right next to the ebuC */
    last_turret_slot = 0;
    call_count = 0;
    ds_fn_draw_station();
    assert(count_kind_color('l', outer) == 1);
    assert(count_kind_color('l', core) == 1);
    own_turret(0, 700, 100, 6);                   /* far away: the same pair of lines */
    call_count = 0;
    ds_fn_draw_station();
    assert(count_kind_color('l', outer) == 1);
    assert(count_kind_color('l', core) == 1);
    player->hp = 0;                               /* a dead ebuC has no beam */
    call_count = 0;
    ds_fn_draw_station();
    assert(count_kind_color('l', outer) == 0 && count_kind_color('l', core) == 0);
    player->hp = 10;
    finished = 1;                                 /* end of the battle: the beam is gone for good */
    call_count = 0;
    ds_fn_draw_station();
    assert(count_kind_color('l', outer) == 0 && count_kind_color('l', core) == 0);
    finished = 0;
    foe_turret(0, 905, 405, 6);
    enemy_last_turret_slot = 0;
    call_count = 0;
    ds_fn_draw_enemy_turrets();
    assert(count_kind_color('l', outer) == 1 && count_kind_color('l', core) == 1);
    finished = 2;
    call_count = 0;
    ds_fn_draw_enemy_turrets();
    assert(count_kind_color('l', outer) == 0 && count_kind_color('l', core) == 0);
    puts("beam: ebuC beam is solid at any range and gone for good at battle end");
}

static void test_universe_fade(void) {
    /* The universe flash does not stop the moment it collapses. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    dt = 0.05;
    universe_active = 1; universe_t = 0.5; universe_x = 400; universe_y = 300;
    for (int i = 0; i < 10; i++) ds_fn_tick_hitbox_fades();
    near(universe_fx_a, 1);
    universe_active = 0; universe_t = ebuc_universe_collapse_time;
    ds_fn_tick_hitbox_fades();
    assert(universe_fx_a > 0 && universe_fx_a < 1);
    call_count = 0;
    ds_fn_draw_universe();
    assert(call_count >= 1);                  /* the point and the rings are still playing out */
    for (int i = 0; i < 40; i++) ds_fn_tick_hitbox_fades();
    near(universe_fx_a, 0);
    call_count = 0;
    ds_fn_draw_universe();
    assert(call_count == 0);
    puts("universe: the collapse flash fades out instead of snapping away");
}

static void test_enemy_class_chances(void) {
    /* The ebuC rarely drops as an enemy, 8 per cent against 20 for Azum and 30 for Santa. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    near(enemy_class_ebuc_chance, 8);
    int counts[4] = {0, 0, 0, 0};
    const int n = 40000;
    for (int i = 0; i < n; i++) {
        ds_fn_enemy_pick_random_class();
        counts[(int)enemy_class]++;
    }
    double ebuc = (double)counts[(int)CLASS_EBUC] / n;
    double azum = (double)counts[(int)CLASS_AZUM] / n;
    double santa = (double)counts[(int)CLASS_SANTA] / n;
    assert(ebuc > 0.065 && ebuc < 0.095);
    assert(azum > 0.18 && azum < 0.22);
    assert(santa > 0.28 && santa < 0.32);
    puts("enemy: ebuC shows up in 8% of solo battles");
}

int main(void) {
    setbuf(stdout, NULL);
    ds_main();
    test_shield_player();
    test_shield_enemy();
    test_snow_pierce_enemy_turrets();
    test_snow_pierce_player_turrets();
    test_dash_real_length();
    test_hitbox_fades();
    test_hitbox_drawing();
    test_punch_hitbox_fades();
    test_status_circles();
    test_dash_zone_narrow();
    test_own_punch_spares_own_turret();
    test_enemy_shield_front_only();
    test_online_turret_punch_death();
    test_punch_sprite_never_empty();
    test_remote_punch_pose_expires();
    test_no_phantom_punch_on_join();
    test_turret_shadow_square();
    test_station_beam_stable();
    test_universe_fade();
    test_enemy_class_chances();
    test_poison_green();
    test_splash_screens();
    test_quest_cooldown();
    return 0;
}
'''


def check_firebase_rules_cover_request_bodies():
    """The RTDB rules must declare EVERY key of a client request body.

    Rule nodes have "$other": {".validate": false}, so one new field in a
    PUT or PATCH without a rule rejects the whole write, which is how online
    once hung on an endless "connecting" while the slot claim failed
    validation. This check catches drift between firebase.rules.json and native/net.
    """
    rules = json.loads((ROOT / "firebase.rules.json").read_text(encoding="utf-8"))["rules"]

    def declared(node):
        return {k for k in node if not k.startswith((".", "$"))}

    slot = declared(rules["rooms"]["$room"]["players"]["$slot"])
    user = declared(rules["users"]["$nick"])
    banner = declared(rules["banner"])
    msg = declared(rules["rooms"]["$room"]["chat"]["$msg"])

    def body_keys(name):
        text = (ROOT / "native/net" / name).read_text(encoding="utf-8")
        return set(re.findall(r'\\"([a-z0-9_]+)\\":', text))

    room = (body_keys("room_sync.inc") | body_keys("room_chat.inc")
            | body_keys("room_threads.inc"))
    missing = room - slot
    assert not missing, f"rooms/$room/players/$slot rules miss: {sorted(missing)}"
    # auth_session.inc also sends login bodies to identitytoolkit, whose keys
    # are not part of the database and are skipped here.
    auth_only = {"email", "password", "grant_type", "refresh_token"}
    profile = (body_keys("state_storage.inc") | body_keys("cloud_patch.inc")
               | body_keys("settings_storage.inc")
               | body_keys("promo.inc")
               | (body_keys("auth_session.inc") - auth_only))
    missing = profile - user
    assert not missing, f"users/$nick rules miss: {sorted(missing)}"
    missing = body_keys("player_api.inc") - msg
    assert not missing, f"rooms/$room/chat/$msg rules miss: {sorted(missing)}"
    missing = body_keys("room_control.inc") - banner
    assert not missing, f"banner rules miss: {sorted(missing)}"


def check_sprite_assets_exist():
    """Every texture the scripts refer to exists in game/assets.

    After the rollback to 76ed6fc azum.png and azum_punch.png fell out of the
    assets: the Azum class was invisible in battle and only the hit effects gave
    him away. The check reads every *.png string name from the scripts and
    requires a file for each of them.
    """
    names = set()
    for ds in (ROOT / "game" / "scripts").rglob("*.ds"):
        names |= set(re.findall(r'"([A-Za-z0-9_./-]+\.png)"', ds.read_text(encoding="utf-8")))
    assert names, "no png reference found in the scripts"
    missing = [n for n in sorted(names) if not (ROOT / "game" / "assets" / n.split("/")[-1]).exists()]
    assert not missing, f"game/assets is missing textures: {missing}"


def main():
    check_firebase_rules_cover_request_bodies()
    check_sprite_assets_exist()
    with tempfile.TemporaryDirectory(prefix="cubic-fixes-") as directory:
        temp = Path(directory)
        compiler = DimScriptCompiler()
        assert compiler.compile(find_ds_files(str(ROOT / "game/scripts")), str(temp / "game.c"))
        assert not compiler.errors and not compiler.warnings

        # ── Wiring checks inside the compiled script modules ──
        fns = compiler.functions
        for name in ("update_game", "update_online"):
            body = fns[name][2]
            for hook in ("tick_hitbox_fades()", "tick_status_fades()"):
                assert body.count(hook) == 1, f"{name} must call {hook} once"
        # A remote swing pose fades by itself: its tick sits before the finished branch
        # and the slot check, otherwise a remote stayed mid swing forever.
        online_upd = fns["update_online"][2]
        assert online_upd.count("tick_remote_punches()") == 1, \
            "update_online must tick remote punches exactly once"
        assert online_upd.index("tick_remote_punches()") < online_upd.index("if finished!=0 then"), \
            "tick_remote_punches must run before the finished early-return"
        # The first slot snapshot does not count as a punch (the counter lives in the room).
        assert "arr_set(remotes,b+6,net_player_punch(s))" in "".join(fns["read_remotes"][2]), \
            "read_remotes must prime the punch counter of a freshly seen slot"
        # The end of the battle drops remote punches with the other effects.
        finish_body = "".join(fns["finish_game"][2])
        assert "clear_remote_punch(i)" in finish_body, \
            "finish_game must clear remote punches"
        # Effects the enemy applied are drawn around the fighter too (solo).
        solo_body = "".join(fns["draw_game"][2])
        for fn in ("draw_player_freeze()", "draw_player_poison()", "draw_player_stun()"):
            assert solo_body.count(fn) == 1, f"draw_game must call {fn}"
        # The enemy class chances come from the config rather than hard-coded numbers.
        pick_body = "".join(fns["enemy_pick_random_class"][2])
        for name in ("enemy_class_azum_chance", "enemy_class_ebuc_chance",
                     "enemy_class_santa_chance"):
            assert name in pick_body, f"enemy_pick_random_class must use {name}"
        for name in ("draw_game", "draw_online"):
            body = fns[name][2]
            assert body.count("draw_ability_hitboxes()") == 1, f"{name} must draw ability hitboxes"
        # Snow pierce helpers are wired into both flight paths.
        assert "snow_chip_enemy_turrets()" in "".join(fns["update_gift"][2])
        assert "snow_chip_player_turrets()" in "".join(fns["tick_enemy_gift"][2])
        # Pierce mask resets on every throw (launch happens at windup end).
        assert "gift.pierce=0" in "".join(fns["tick_super_windup"][2])
        assert "enemy_gift.pierce=0" in "".join(fns["enemy_start_snow"][2])
        # Shield funnels sit in the damage entry points.
        assert "player_shield_absorb(dmg)" in "".join(fns["take_damage"][2])
        assert "enemy_shield_absorb(dmg)" in "".join(fns["enemy_apply_damage"][2])
        assert "enemy_shield_absorb(dmg)" in "".join(fns["universe_collapse"][2])
        assert "player_shield_absorb(dmg)" in "".join(fns["universe_collapse_remote"][2])
        # Dash resolution takes the traveled length.
        params = [p[1] for p in fns["dash_resolve_target"][1]]
        assert params == ["sx", "sy", "dx", "dy", "len"]
        # Dash hitbox: one layer of big rounded cells, no small trailing cube
        # chain and no stripe; cells are reused from one shared array.
        assert "draw_hit_dash_cube" not in fns and "draw_hit_dash_zone" not in fns
        dash_body = "".join(fns["draw_hit_dash_path"][2])
        assert "rect(" in dash_body and "roundrect(" not in dash_body
        assert "line(" not in dash_body
        assert "hitbox_corner" not in dash_body, "dash cells must be sharp squares"
        assert "arr_new()" not in dash_body, "dash cells must reuse one array"
        # Sharp-cornered zones: the punch and every strip use rect_rot, not line().
        punch_body = "".join(fns["draw_punch_box"][2])
        assert "draw_hit_strip(" in punch_body and "line(" not in punch_body
        strip_body = "".join(fns["draw_hit_strip"][2])
        assert "rect_rot(" in strip_body and "line(" not in strip_body
        # One alpha for every zone: hb_zone_col supplies it.
        zone_body = "".join(fns["hb_zone_col"][2])
        assert "hitbox_zone_alpha" in zone_body
        # Own turret can never intercept the owner's punch.
        assert "punch_turret_target" not in fns
        # Turret shadow is the despenser sprite itself (square, tinted).
        shadow_body = "".join(fns["draw_turret_shadow_at"][2])
        assert "tex_tint(" in shadow_body and "DESPENSER_TEX" in shadow_body
        # The visibility of the ebuC beam comes from one predicate in all three places.
        for name in ("draw_station", "draw_remote_station", "draw_enemy_turrets"):
            assert "station_beam_visible(" in "".join(fns[name][2]), \
                f"{name} must gate the ebuC beam through station_beam_visible"
        for name in ("draw_game", "draw_online"):
            assert "".join(fns[name][2]).count("draw_turret_shadows()") == 1, \
                f"{name} must draw turret shadows in the shadow layer"
        # Enemy shield covers from the front only (turret behind the body
        # never substitutes for it).
        assert "tf<=bw+30" in "".join(fns["enemy_punch_turret_target"][2])
        # Poison is green (fill 0x00C853), freeze stays blue.
        assert "0x0000C853" in "".join(fns["draw_poison"][2])
        assert "0x0000C853" in "".join(fns["draw_player_poison"][2])
        # Splash: warning keeps its black screen; studio bg fades with the logo.
        assert "0xFF000000" in "".join(fns["draw_warning"][2])
        assert "studio_bg_rgb(0)" in "".join(fns["draw_studio"][2])
        studio_body = "".join(fns["update_studio"][2]).replace(" ", "")
        assert "studio_bg_a=studio_a" in studio_body

        # -- Chat: message bubbles above the fighters and a close button in the centre --
        online_body = "".join(fns["draw_online"][2])
        assert online_body.count("draw_chat_bubbles()") == 1, \
            "draw_online must draw chat bubbles once"
        upd_body = "".join(fns["update_online"][2])
        assert upd_body.count("chat_bubbles_watch()") == 1, \
            "update_online must tick chat bubbles once"
        assert "chat_bubbles_reset()" in "".join(fns["init_online"][2])
        # The chat closes where draw_back sits: top centre.
        close_x = "".join(fns["chat_close_x"][2]).replace(" ", "")
        assert "(screen_w-btn_w)/2" in close_x, \
            "chat close button must sit centered like every other close button"
        assert "back_y" in "".join(fns["chat_top_y"][2])
        # New messages are found by their server key, which survives trimming.
        watch_body = "".join(fns["chat_bubbles_watch"][2])
        assert "net_chat_key(" in watch_body and "chat_seen_key" in watch_body

        # -- Settings: music volume --
        settings_body = "".join(fns["draw_settings"][2])
        assert "tr_music_vol()" in settings_body and "music_half_w()" in settings_body
        touch_body = "".join(fns["touch_settings"][2])
        assert touch_body.count("music_volume_step(") == 2, \
            "touch_settings must handle both - and + buttons"
        assert "net_load_music_volume()" in "".join(fns["settings_from_storage"][2])
        assert "net_save_music_volume(" in "".join(fns["music_volume_step"][2])

        # -- Cards and promo codes: solo drops only, a random code per card --
        solo_upd = "".join(fns["update_game"][2])
        assert solo_upd.count("card_update()") == 1, \
            "update_game must tick cards once"
        draw_game_body = "".join(fns["draw_game"][2])
        assert draw_game_body.count("draw_card_item()") == 1, \
            "draw_game must draw the field card once"
        assert draw_game_body.count("draw_card_open()") == 1, \
            "draw_game must draw the card overlay once"
        assert "card_roll()" in "".join(fns["init_game"][2]), \
            "init_game must roll the card chance"
        reset_body = "".join(fns["reset_battle"][2])
        assert "card_active=0" in reset_body and "card_open=0" in reset_body, \
            "reset_battle must clear the card state"
        # Online never sees cards: the tick and the roll are solo only.
        assert "card_update()" not in "".join(fns["update_online"][2])
        assert "card_roll()" not in "".join(fns["init_online"][2])
        assert "card_open!=0" in "".join(fns["touch_game"][2]), \
            "touch_game must be blocked while the card overlay is open"
        # The promo screen: a menu button and the input routing.
        assert "tr_promo()" in "".join(fns["draw_modes"][2])
        assert "start_transition(ST_PROMO)" in "".join(fns["touch_modes"][2])
        assert "touch_promo(" in "".join(fns["touch_menu"][2])
        # The code is derived from the nick in C, not in the config or on the client.
        promo_c = (ROOT / "native/net/promo.inc").read_text(encoding="utf-8")
        # new format 3 letters + 1 digit, e.g. ABC1, deterministic from nick
        assert "code[4] = 0" in promo_c
        assert "'A' + (h % 26)" in promo_c or "promo_code_for_nick" in promo_c
        assert "promo_sync_with_cloud(resp)" in (
            ROOT / "native/net/profile_apply.inc").read_text(encoding="utf-8")

        # -- Azum dash: faster at the same distance (300 * 1.275 = 382.5) --
        config_text = (ROOT / "game/scripts/core/config.ds").read_text(encoding="utf-8")
        assert "azum_dash_time=1.275" in config_text and "azum_dash_speed=300" in config_text

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
