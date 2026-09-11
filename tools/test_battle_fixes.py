#!/usr/bin/env python3
"""Regression checks for the battle fixes batch, without Android or Firebase.

Covers: real dash hitbox (traveled length), turret shield of the buk absorbing
all damage, snowflakes piercing turrets, smooth hitbox fades, filled snowflake
hitbox, poison drawn green next to blue freeze, and the warning/studio splash
timing (black screen never jumps). Compiles the real DimScript sources with a
host C harness and asserts on the actual logic/draw calls.
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
int png_load(const char *name) { (void)name; return 1; }
void tex(float x, float y, const char *name, float a, float sc) { (void)x; (void)y; (void)name; (void)a; (void)sc; }
void tex_tint(float x, float y, const char *name, float a, float sc, uint32_t c) { (void)x; (void)y; (void)name; (void)a; (void)sc; (void)c; }
void roundrect(float x, float y, float w, float h, float r, uint32_t color) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)color; }
int text_ink_width(const char *s) { (void)s; return 10; }
int text_ink_height(const char *s) { (void)s; return 10; }
int text_ink_top(const char *s) { (void)s; return 2; }
void text_scaled(const char *s, float x, float y, uint32_t c, float scale) { (void)s; (void)x; (void)y; (void)c; (void)scale; }

/* Net stubs (same prototypes as net.h): purchases/saves never reach the cloud
 * in these tests, they only need to not crash. */
void net_save_progress_all(double, double, double, double, double, double,
    double, double, double, double, double, double,
    double, double, double, double, double, double) {}
void net_save_progress(double, double, double, double, double, double, double, double) {}
double net_slot(void) { return -1; }
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
void net_set_mode(double v) { (void)v; }
void net_set_room(double v) { (void)v; }
void net_open(double v) { (void)v; }
void net_connect(const char *a, const char *b) { (void)a; (void)b; }
void keyboard_hide(void) {}
void net_event_set(double mode) { (void)mode; }
double net_status(void) { return 0; }
double net_count(void) { return 0; }
double net_player_online(double s) { (void)s; return 0; }
double net_player_x(double s) { (void)s; return 0; }
double net_player_y(double s) { (void)s; return 0; }
double net_player_angle(double s) { (void)s; return 0; }
double net_player_hp(double s) { (void)s; return 0; }
double net_player_alive(double s) { (void)s; return 0; }
const char *net_player_nick(double s) { (void)s; return ""; }
double net_player_punch_x(double s) { (void)s; return 0; }
double net_player_punch_y(double s) { (void)s; return 0; }
double net_player_punch_dx(double s) { (void)s; return 0; }
double net_player_punch_dy(double s) { (void)s; return 0; }
double net_player_punch(double s) { (void)s; return 0; }
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
    calls[call_count - 1].t = t;   /* толщина полосы — она и есть ширина зоны */
}
void rect(float x, float y, float w, float h, uint32_t c) { record('q', x, y, w, h, c); }

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
    near(player->hp, 20);           /* турель забрала весь урон */
    ds_fn_take_damage(8);
    near(arr_get(turret_hp, 0), 0); /* турель разбита */
    near(player->hp, 18);           /* остаток урона дошёл до бука */
    ds_fn_take_damage(3);
    near(player->hp, 15);           /* без турелей урон идёт напрямую */
    assert(arr_len(station_boom_ts) == 1);
    /* Урон в онлайне (вселенная соперника) идёт через тот же щит. */
    own_turret(0, 500, 460, 9);
    ds_fn_universe_collapse_remote(5);
    near(arr_get(turret_hp, 0), 4);
    near(player->hp, 15);
    puts("shield: buk turret absorbs player damage (solo+remote), overflow hits buk");
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
    /* Ульта бука в соло: урон уходит в турели врага и свои. */
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
    near(arr_get(turret_hp, 0), 2);      /* своя вселенная бьёт и свой щит */
    near(player->hp, 30);                /* урон поглощён полностью */
    near(arr_get(enemy_turret_hp, 0), 2);
    near(enemy->hp, 30);
    ds_fn_universe_collapse(10);
    near(arr_get(turret_hp, 0), 0);
    near(player->hp, 22);                /* остаток (10-2) дошёл до бука */
    near(arr_get(enemy_turret_hp, 0), 0);
    near(enemy->hp, 22);
    puts("shield: enemy buk turret absorbs damage, own universe absorbed by own shield");
}

static void test_snow_pierce_enemy_turrets(void) {
    /* Снежинка игрока летит к врагу-буку и проходит сквозь его турель. */
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    player_class = CLASS_SANTA;
    enemy_class = CLASS_EBUC;
    player->x = 300; player->y = 400; player->size = 25;
    enemy->x = 900; enemy->y = 400; enemy->size = 25;
    player->hp = 999; player->max_hp = 999;
    enemy->hp = 500; enemy->max_hp = 500;
    foe_turret(0, 600, 400, 100);  /* прямо на траектории, HP больше урона */
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
    assert(exploded == 1);                 /* в итоге долетела и взорвалась */
    /* Турель чинится ровно один раз за полёт (pierce-бит), не каждый кадр. */
    near(before - arr_get(enemy_turret_hp, 0), santa_super_damage);
    puts("snow: pierces enemy turret (single chip) and keeps flying to the buk");
}

static void test_snow_pierce_player_turrets(void) {
    /* Снежинка врага летит к игроку и проходит сквозь турель игрока. */
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
    /* Онлайн-рывок бьёт только по уже проеханному отрезку. */
    ds_fn_reset_battle();
    game_state = ST_ONLINE;
    player_class = CLASS_ORDINARY;
    player->y = 500; player->size = 25;
    player->hp = 999; player->max_hp = 999;
    enemy->x = 100; enemy->y = 100; enemy->size = 25;
    double total = azum_dash_speed * azum_dash_time;  /* весь путь рывка */
    assert(total > 0);
    /* Турель на полпути, игрок чуть раньше конца пути. */
    player->x = 200 + total - 40;
    own_turret(0, 200 + total * 0.5, 500, 50);
    assert(ds_fn_dash_resolve_target(200, 500, 1, 0, total * 0.2) == -1);
    double got = ds_fn_dash_resolve_target(200, 500, 1, 0, total * 0.7);
    assert(got == 1);                                  /* турель на пути */
    got = ds_fn_dash_resolve_target(200, 500, 1, 0, total);
    assert(got == 1);                                  /* щит впереди игрока */
    arr_set(turret_hp, 0, 0);                          /* турель мертва */
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
    /* Рывок: альфа плавно набирается. */
    dash_active = 1; dash_box_a = 0;
    for (int i = 0; i < 4; i++) ds_fn_tick_hitbox_fades();
    near(dash_box_a, 1);
    /* После конца — плавно гаснет (0.05/0.2 за кадр). */
    dash_active = 0;
    ds_fn_tick_hitbox_fades();
    near(dash_box_a, 0.75);
    ds_fn_tick_hitbox_fades();
    near(dash_box_a, 0.5);
    for (int i = 0; i < 20; i++) ds_fn_tick_hitbox_fades();
    near(dash_box_a, 0);
    /* Снежинка: снаряд и взрыв набираются и гаснут. */
    gift->active = 1; snow_ball_a = 0;
    boom_t = 1; boom_x = 300; boom_y = 300; snow_boom_a = 0;
    for (int i = 0; i < 4; i++) ds_fn_tick_hitbox_fades();
    near(snow_ball_a, 1); near(snow_boom_a, 1);
    gift->active = 0; boom_t = 0;
    for (int i = 0; i < 20; i++) ds_fn_tick_hitbox_fades();
    near(snow_ball_a, 0); near(snow_boom_a, 0);
    /* Турель набирает альфу, после смерти — гаснет. */
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
    uint32_t gray = 0x00808080;
    uint32_t solid = (140u << 24) | gray;   /* hitbox_solid_alpha=140 */
    uint32_t line_c = (170u << 24) | gray;  /* hitbox_line_alpha=170 */
    /* Снежинка: вся зона залита (сплошной круг), линия пути и взрыв тоже. */
    gift->active = 1; snow_ball_a = 1;
    gift->x = 400; gift->y = 400; gift->dx = 1; gift->dy = 0; gift->t = 0;
    boom_t = 1; boom_x = 700; boom_y = 400; snow_boom_a = 1;
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    assert(count_kind_color('c', solid) >= 2);   /* снаряд + взрыв залиты */
    assert(count_kind_color('r', line_c) >= 2);  /* контуры поверх заливки */
    assert(count_kind_color('l', line_c) >= 1);  /* линия пути */
    /* При альфе 0.5 заливка полупрозрачная (плавное появление работает). */
    snow_ball_a = 0.5;
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    assert(count_kind_color('c', (70u << 24) | gray) >= 1);
    gift->active = 0; boom_t = 0;
    /* Рывок: зона урона рисуется как полоса удара (та же альфа aim_max_alpha)
     * плюс плотная цепочка кубиков по проеханному пути. Серого круга под
     * бойцом больше нет — зона рывка это отрезок, а не пятно под кубиком. */
    dash_active = 1; dash_box_a = 1;
    dash_x0 = 300; dash_y0 = 400; dash_dx = 1; dash_dy = 0;
    player->x = 700; player->y = 400;
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    uint32_t aim_c = (102u << 24) | gray;        /* aim_max_alpha=102 */
    double pr = ds_fn_dash_hit_radius_solo();    /* тот же радиус, что и урон */
    int cubes = count_kind_color('q', aim_c);
    double want_step = pr * dash_hitbox_step;
    double travel = azum_dash_speed * azum_dash_time;   /* 382.5 из 400 пути */
    /* Кубиков больше, чем шагов: они ставятся каждые dash_hitbox_step радиусов. */
    assert(cubes >= (int)(travel / want_step));
    assert(cubes >= 15);
    /* Полоса зоны — от старта до текущего положения, шириной в диаметр зоны. */
    int zone = count_kind_color('l', aim_c);
    assert(zone >= 1);
    int zi = -1;
    for (int i = 0; i < call_count; i++)
        if (calls[i].kind == 'l' && calls[i].color == aim_c) { zi = i; break; }
    near(calls[zi].x, 300); near(calls[zi].y, 400);
    near(calls[zi].w, 300 + travel);
    near(calls[zi].t, pr * dash_hitbox_zone_scale);
    /* Ни одного залитого круга рывка: пятно под бойцом убрано. */
    assert(count_kind_color('c', aim_c) == 0);
    assert(count_kind_color('c', solid) == 0);
    /* Рывок бота в соло рисуется тем же радиусом, которым он бьёт. */
    dash_active = 0;
    enemy_dash_active = 1; edash_box_a = 1;
    enemy_dash_x0 = 1100; enemy_dash_y0 = 400; enemy_dash_dx = -1; enemy_dash_dy = 0;
    enemy->x = 800; enemy->y = 400;
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    assert(count_kind_color('l', aim_c) >= 1);
    assert(count_kind_color('q', aim_c) >= 10);
    assert(count_kind_color('c', aim_c) == 0);
    enemy_dash_active = 0;
    /* Турель: контурный круг своего радиуса. */
    dash_active = 0;
    own_turret(0, 500, 460, 10);
    arr_set(turret_box_a, 0, 1);
    call_count = 0;
    ds_fn_draw_ability_hitboxes();
    assert(count_kind_color('r', line_c) >= 1);
    assert(count_kind_color('c', (36u << 24) | gray) >= 1); /* тонкая заливка */
    puts("hitboxes: real geometry drawn, snowflake/dash area solid-filled, fade-aware");
}

static void test_poison_green(void) {
    ds_fn_reset_battle();
    game_state = ST_SOLO;
    enemy->x = 100; enemy->y = 100; enemy->size = 25;
    player->x = 300; player->y = 100; player->size = 25;
    /* Яд зелёный — не сливается с голубой заморозкой. */
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
    /* Заморозка осталась голубой. */
    freeze_a = 1;
    call_count = 0;
    ds_fn_draw_freeze();
    assert(count_kind_color('c', (76u << 24) | 0x1E88E5) == 1);
    assert(count_kind_color('r', (178u << 24) | 0x4FC3F7) == 1);
    puts("poison: green vs blue freeze — the two effects no longer look doubled");
}

static void test_splash_screens(void) {
    /* Предупреждение: текст тает, чёрный экран остаётся и сразу же на нём
     * проявляется заставка студии. */
    ds_fn_reset_battle();
    warn_open = 1; warn_t = 0; warn_a = 1; warn_hold = 2.5; warn_fade = 0.6;
    studio_open = 0;
    dt = 0.1;
    for (int i = 0; i < 40; i++) ds_fn_update_warning();  /* 4.0 c */
    assert(warn_open == 0 && studio_open == 1 && studio_bg_a == 1);
    /* Заставка: логотип проявляется и висит на цельном чёрном фоне... */
    studio_t = 0; studio_a = 0; studio_in = 0.5; studio_hold = 1.6; studio_fade = 0.7;
    for (int i = 0; i < 10; i++) ds_fn_update_studio();   /* 1.0 c */
    near(studio_a, 1); near(studio_bg_a, 1);
    for (int i = 0; i < 12; i++) ds_fn_update_studio();   /* +1.2 c = 2.2 c */
    /* Логотип висел на цельном чёрном фоне до 2.1 c; после 2.1 c чёрный фон
     * начал гаснуть вместе с логотипом — сейчас середина затухания. */
    assert(studio_a > 0 && studio_a < 1);
    near(studio_bg_a, studio_a);
    for (int i = 0; i < 20; i++) ds_fn_update_studio();   /* +2.0 c */
    assert(studio_open == 0 && studio_bg_a == 0);
    /* Отрисовка: фон предупреждения непрозрачно-чёрный даже при тающем
     * тексте; фон заставки прозрачнеет только вместе с логотипом. */
    warn_open = 1; warn_a = 0.5;
    call_count = 0;
    ds_fn_draw_warning();
    assert(calls[0].kind == 'q' && calls[0].color == 0xFF000000);
    studio_open = 1; studio_a = 0.4; studio_bg_a = 0.4;
    call_count = 0;
    ds_fn_draw_studio();
    assert(calls[0].kind == 'q' && calls[0].color == 0x66000000);
    studio_a = 1; studio_bg_a = 1;
    call_count = 0;
    ds_fn_draw_studio();
    assert(calls[0].kind == 'q' && calls[0].color == 0xFF000000);
    puts("splash: warning keeps black screen, studio logo fades out with it");
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
    test_poison_green();
    test_splash_screens();
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="cubic-fixes-") as directory:
        temp = Path(directory)
        compiler = DimScriptCompiler()
        assert compiler.compile(find_ds_files(str(ROOT / "game/scripts")), str(temp / "game.c"))
        assert not compiler.errors and not compiler.warnings

        # ── Wiring checks inside the compiled script modules ──
        fns = compiler.functions
        for name, hook in (("update_game", "tick_hitbox_fades()"),
                           ("update_online", "tick_hitbox_fades()")):
            body = fns[name][1]
            assert body.count(hook) == 1, f"{name} must tick hitbox fades once"
        for name in ("draw_game", "draw_online"):
            body = fns[name][1]
            assert body.count("draw_ability_hitboxes()") == 1, f"{name} must draw ability hitboxes"
        # Snow pierce helpers are wired into both flight paths.
        assert "snow_chip_enemy_turrets()" in "".join(fns["update_gift"][1])
        assert "snow_chip_player_turrets()" in "".join(fns["tick_enemy_gift"][1])
        # Pierce mask resets on every throw (launch happens at windup end).
        assert "gift.pierce=0" in "".join(fns["tick_super_windup"][1])
        assert "enemy_gift.pierce=0" in "".join(fns["enemy_start_snow"][1])
        # Shield funnels sit in the damage entry points.
        assert "player_shield_absorb(dmg)" in "".join(fns["take_damage"][1])
        assert "enemy_shield_absorb(dmg)" in "".join(fns["enemy_apply_damage"][1])
        assert "enemy_shield_absorb(dmg)" in "".join(fns["universe_collapse"][1])
        assert "player_shield_absorb(dmg)" in "".join(fns["universe_collapse_remote"][1])
        # Dash resolution takes the traveled length.
        params = [p[1] for p in fns["dash_resolve_target"][0]]
        assert params == ["sx", "sy", "dx", "dy", "len"]
        # Poison is green (fill 0x00C853), freeze stays blue.
        assert "0x0000C853" in "".join(fns["draw_poison"][1])
        assert "0x0000C853" in "".join(fns["draw_player_poison"][1])
        # Splash: warning keeps its black screen; studio bg fades with the logo.
        assert "0xFF000000" in "".join(fns["draw_warning"][1])
        assert "studio_bg_rgb(0)" in "".join(fns["draw_studio"][1])
        studio_body = "".join(fns["update_studio"][1]).replace(" ", "")
        assert "studio_bg_a=studio_a" in studio_body

        android = temp / "android"
        android.mkdir()
        (android / "asset_manager.h").write_text("typedef struct AAssetManager AAssetManager;\n")
        (android / "log.h").touch()
        (android / "native_window.h").touch()
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
