/* Головной тест-стенд игры «Ярик Сафонов: С чистого листа» (Linux, без окна).
 *
 * Гоняет настоящий скрипт (init/update/draw/touch) на программном буфере:
 * лобби -> двор -> уборка мусора -> закрытый день -> срыв -> лобби -> привычки.
 * Управление идёт через настоящие тапы по джойстику и кнопкам, а не напрямую
 * по переменным, поэтому ловятся и ошибки ввода, и падения в отрисовке.
 *
 * Сборка и запуск: ./test/build_test.sh && ./test/game_test
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "runtime.h"

/* Глобалы, сгенерированные из DimScript (в game.c они не static). */
extern double game_state, t_dir, day, cleaned, goal, craving, over, last_reward;
extern double willpower, best_day, shoes_level, bag_owned, calm_owned;
extern double hold_cd, bin_x, bin_y, max_craving, btn_w, btn_h, back_y;
extern DSArray *trash_x, *trash_y, *trash_on, *bot_x, *bot_y, *bot_dx, *bot_dy;
extern void *yarik; /* object Yarik: x, y, angle, bag, hold */
#define Y_X 0
#define Y_Y 1
#define Y_BAG 3

static const int g_w = 1280, g_h = 720;
static uint32_t *g_pixels;
static long g_frame;
static int script_active;

static double yfield(int i) { return ((double *)yarik)[i]; }

static void protected_init(void *u) { (void)u; init(NULL); }
static void protected_reset(void *u) { (void)u; reset(); }
static void protected_update(void *u) { (void)u; update(); }
static void protected_draw(void *u) { draw((Buffer *)u); }
typedef struct { float x, y; int action, id; } TouchCall;
static void protected_touch(void *u) { TouchCall *c = (TouchCall *)u; touch(c->x, c->y, c->action, c->id); }

static void send_touch(float x, float y, int action, int id) {
    TouchCall c; c.x = x; c.y = y; c.action = action; c.id = id;
    ds_call_protected(protected_touch, &c, "touch");
}
static void do_tap(float x, float y) { send_touch(x, y, 0, 1); send_touch(x, y, 1, 1); }

static int run_frames(int n) {
    for (int i = 0; i < n; i++) {
        dt = 1.0 / 60.0;
        if (script_active && !ds_call_protected(protected_update, NULL, "update")) {
            printf("[frame %ld] UPDATE FAILED: %s\n", g_frame, ds_runtime_error_message());
            script_active = 0; return 0;
        }
        Buffer buf = { g_pixels, g_w, g_h, g_w };
        if (!ds_graphics_begin_frame(&buf)) { printf("[frame %ld] begin_frame failed\n", g_frame); return 0; }
        if (script_active && !ds_call_protected(protected_draw, &buf, "draw")) {
            printf("[frame %ld] DRAW FAILED: %s\n", g_frame, ds_runtime_error_message());
            script_active = 0; ds_graphics_end_frame(); return 0;
        }
        ds_graphics_end_frame();
        g_frame++;
    }
    return 1;
}

/* Скриншот текущего кадра в PPM — удобно смотреть глазами. */
static void shot(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "test/shots/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
    for (int i = 0; i < g_w * g_h; i++) {
        /* Кадровый буфер движка — ABGR: красный лежит в младшем байте. */
        uint32_t p = g_pixels[i];
        unsigned char rgb[3] = { (unsigned char)p, (unsigned char)(p >> 8), (unsigned char)(p >> 16) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("    shot -> %s\n", path);
}

static int wait_state(double want, int max_frames) {
    for (int i = 0; i < max_frames; i++) {
        if (!run_frames(1)) return 0;
        if (game_state == want && t_dir == 0) return 1;
    }
    return 0;
}

/* Кнопки лобби: ряд 0 — Играть, 1 — Привычки, 2 — Как играть, 3 — Настройки. */
static void tap_menu_row(int row) {
    float x = (float)g_w / 2.0f;
    float y = (float)(g_h / 2 - 140 + row * 80) + (float)btn_h / 2.0f;
    do_tap(x, y);
}
static void tap_back(void) { do_tap((float)g_w / 2.0f, (float)back_y + (float)btn_h / 2.0f); }
static void tap_hold(void) { do_tap((float)g_w - 140.0f, (float)g_h - 150.0f); }

/* Джойстик: тянем стик в сторону цели настоящими тапами. */
static int joy_pressed_flag;
static void steer_to(double tx, double ty) {
    double dx = tx - yfield(Y_X), dy = ty - yfield(Y_Y);
    double d = sqrt(dx * dx + dy * dy);
    if (d < 1.0) d = 1.0;
    float jx = 130.0f, jy = (float)g_h - 150.0f;
    send_touch(jx + (float)(dx / d * 60.0), jy + (float)(dy / d * 60.0), joy_pressed_flag ? 2 : 0, 7);
    joy_pressed_flag = 1;
}
static void release_joy(void) {
    if (joy_pressed_flag) { send_touch(130.0f, (float)g_h - 150.0f, 1, 7); joy_pressed_flag = 0; }
}

/* Дойти до точки, но не дольше limit кадров. */
static int walk_to(double tx, double ty, int limit) {
    for (int i = 0; i < limit; i++) {
        steer_to(tx, ty);
        if (!run_frames(1)) return 0;
        double dx = tx - yfield(Y_X), dy = ty - yfield(Y_Y);
        if (dx * dx + dy * dy < 20 * 20) { release_joy(); return 1; }
        if (over != 0) { release_joy(); return 1; }
    }
    release_joy();
    return 0;
}

/* Индекс ближайшего мусора, который ещё лежит на земле. */
static int nearest_trash(void) {
    int best = -1; double bd = 1e18;
    for (int i = 0; i < (int)arr_len(trash_on); i++) {
        if (arr_get(trash_on, i) != 1) continue;
        double dx = arr_get(trash_x, i) - yfield(Y_X), dy = arr_get(trash_y, i) - yfield(Y_Y);
        double d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

int main(void) {
    setbuf(stdout, NULL);
    screen_w = g_w; screen_h = g_h;
    g_pixels = (uint32_t *)calloc((size_t)g_w * g_h, 4);
    if (!g_pixels) { printf("OOM\n"); return 2; }
    remove("progress.dat");
    system("mkdir -p test/shots");

    ds_clear_runtime_error(); ds_clear_script_restart(); ds_string_pool_reset();
    if (!ds_call_protected(protected_init, NULL, "init")) {
        printf("init failed: %s\n", ds_runtime_error_message()); return 2;
    }
    script_active = 1;
    if (!run_frames(5)) return 2;
    printf("=== init ok: lobby state %g, willpower %g\n", game_state, willpower);
    shot("01_lobby");
    if (game_state != 0) { printf("!! expected lobby\n"); return 3; }

    /* --- 1. «Как играть» и «Настройки» открываются и закрываются --- */
    tap_menu_row(2);
    if (!wait_state(3, 60)) { printf("!! how-to screen did not open\n"); return 3; }
    shot("02_howto");
    tap_back();
    if (!wait_state(0, 60)) { printf("!! how-to back failed\n"); return 3; }

    /* --- 2. Двор: день 1 --- */
    tap_menu_row(0);
    if (!wait_state(1, 60)) { printf("!! yard did not open\n"); return 3; }
    if (day != 1 || cleaned != 0 || over != 0) { printf("!! bad day start: day %g cleaned %g\n", day, cleaned); return 3; }
    printf("=== day 1 started: goal %g, trash on map %g\n", goal, arr_len(trash_on));
    run_frames(20);
    shot("03_yard");

    /* --- 3. Убираем двор: мусор -> урна, пока день не закрыт --- */
    int guard = 0;
    while (over == 0 && guard++ < 60) {
        int t = nearest_trash();
        if (t >= 0 && yfield(Y_BAG) < 4) {
            if (!walk_to(arr_get(trash_x, t), arr_get(trash_y, t), 900)) { printf("!! cannot reach trash %d\n", t); return 3; }
        } else {
            if (!walk_to(bin_x, bin_y, 900)) { printf("!! cannot reach bin\n"); return 3; }
            run_frames(2);
        }
        /* Тяга не должна успеть добить: жмём «Держись», когда набралась. */
        if (craving > max_craving * 0.6 && hold_cd <= 0) tap_hold();
    }
    if (over != 2) { printf("!! day was not finished, over=%g cleaned=%g craving=%g\n", over, cleaned, craving); return 3; }
    printf("=== day 1 done: cleaned %g/%g, reward %g, willpower %g, best %g\n",
           cleaned, goal, last_reward, willpower, best_day);
    if (best_day != 1) { printf("!! best day not recorded\n"); return 3; }
    if (willpower <= 0) { printf("!! no willpower earned\n"); return 3; }
    run_frames(2);
    shot("04_day_done");

    /* --- 4. Следующий день сложнее --- */
    double goal1 = goal;
    do_tap((float)g_w / 2.0f, (float)g_h / 2.0f);
    run_frames(5);
    if (day != 2 || over != 0) { printf("!! day 2 did not start (day %g over %g)\n", day, over); return 3; }
    if (goal <= goal1) { printf("!! day 2 is not harder: %g -> %g\n", goal1, goal); return 3; }
    printf("=== day 2 started: goal %g\n", goal);

    /* --- 5. «Держись» сбивает тягу --- */
    run_frames(240);
    double before = craving;
    if (before < 5) { printf("!! craving does not grow: %g\n", before); return 3; }
    tap_hold();
    run_frames(1);
    if (craving >= before) { printf("!! hold on did not cut craving: %g -> %g\n", before, craving); return 3; }
    printf("=== hold on works: craving %g -> %g (cooldown %g)\n", before, craving, hold_cd);
    shot("05_hold");

    /* --- 6. Ничего не делаем: тяга добивает -> срыв --- */
    double will_before = willpower;
    for (int i = 0; i < 60 * 240 && over == 0; i++) run_frames(1);
    if (over != 1) { printf("!! relapse did not happen, craving %g\n", craving); return 3; }
    if (willpower != will_before) { printf("!! willpower changed on relapse: %g -> %g\n", will_before, willpower); return 3; }
    printf("=== relapse on day %g, willpower kept: %g\n", day, willpower);
    run_frames(2);
    shot("06_relapse");

    /* Тап после срыва возвращает в лобби. */
    do_tap((float)g_w / 2.0f, (float)g_h / 2.0f);
    if (!wait_state(0, 120)) { printf("!! relapse tap did not return to lobby\n"); return 3; }

    /* --- 7. Новый забег начинается с чистого листа (день 1) --- */
    tap_menu_row(0);
    if (!wait_state(1, 60)) { printf("!! second run did not start\n"); return 3; }
    if (day != 1 || craving > 5 || cleaned != 0) { printf("!! run did not reset: day %g craving %g\n", day, craving); return 3; }
    printf("=== new run starts from day 1 with a clean slate\n");
    tap_back();
    if (!wait_state(0, 60)) { printf("!! back to lobby failed\n"); return 3; }

    /* --- 8. Привычки покупаются за силу воли и сохраняются --- */
    tap_menu_row(1);
    if (!wait_state(2, 60)) { printf("!! habits screen did not open\n"); return 3; }
    shot("07_habits");
    willpower = 500;
    double card_w = 300, gap = 26, total = card_w * 3 + gap * 2;
    double cx0 = (g_w - total) / 2, cy = back_y + btn_h + 46 + 296 - 70 + 25;
    do_tap((float)(cx0 + card_w / 2), (float)cy);                     /* кроссовки */
    run_frames(2);
    if (shoes_level != 1) { printf("!! sneakers not bought: %g\n", shoes_level); return 3; }
    do_tap((float)(cx0 + card_w + gap + card_w / 2), (float)cy);      /* большой мешок */
    run_frames(2);
    if (bag_owned != 1) { printf("!! big bag not bought\n"); return 3; }
    do_tap((float)(cx0 + 2 * (card_w + gap) + card_w / 2), (float)cy);/* дыхание */
    run_frames(2);
    if (calm_owned != 1) { printf("!! breathing not bought\n"); return 3; }
    printf("=== habits bought: shoes %g, bag %g, calm %g, willpower left %g\n",
           shoes_level, bag_owned, calm_owned, willpower);
    shot("08_habits_bought");

    /* Прогресс переживает перезапуск скрипта. */
    double saved_will = willpower, saved_best = best_day;
    ds_call_protected(protected_reset, NULL, "reset");
    /* reset() оставляет свежие пустые массивы — их пересоздаст init(), поэтому
     * освобождаем сами, иначе ASAN справедливо ругается на утечку. */
    arr_free(trash_x); arr_free(trash_y); arr_free(trash_on);
    arr_free(bot_x); arr_free(bot_y); arr_free(bot_dx); arr_free(bot_dy);
    script_active = 0;
    ds_clear_runtime_error(); ds_string_pool_reset();
    if (!ds_call_protected(protected_init, NULL, "init")) { printf("!! restart failed\n"); return 3; }
    script_active = 1;
    run_frames(3);
    if (willpower != saved_will || best_day != saved_best || shoes_level != 1 || bag_owned != 1 || calm_owned != 1) {
        printf("!! progress not restored: will %g/%g best %g/%g shoes %g bag %g calm %g\n",
               willpower, saved_will, best_day, saved_best, shoes_level, bag_owned, calm_owned);
        return 3;
    }
    printf("=== progress restored after restart: willpower %g, best day %g\n", willpower, best_day);

    ds_call_protected(protected_reset, NULL, "reset");
    /* reset() заново создаёт пустые массивы — освобождаем их, чтобы ASAN
     * заканчивал прогон без «утечек» и любой ненулевой код выхода был бедой. */
    arr_free(trash_x); arr_free(trash_y); arr_free(trash_on);
    arr_free(bot_x); arr_free(bot_y); arr_free(bot_dx); arr_free(bot_dy);
    ds_string_pool_reset();
    ds_release_assets();
    free(g_pixels);
    printf("=== DONE ok (%ld frames)\n", g_frame);
    return 0;
}
