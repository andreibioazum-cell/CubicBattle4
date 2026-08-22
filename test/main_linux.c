/* Головной тест-стенд игры «Ярик Сафонов: С чистого листа» (Linux, без окна).
 *
 * Гоняет настоящий скрипт (init/update/draw/touch) на программном буфере:
 * лобби -> день во дворе -> ночь у холодильника -> срыв -> лобби -> привычки.
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
extern double willpower, best_day, up_speed, up_bag, up_hold, up_night, up_food;
extern double hold_cd, bin_x, bin_y, max_craving, btn_w, btn_h, back_y;
extern double phase, eaten, fridge_x, fridge_y, bed_x, bed_y, drinks, drink_limit, combo;
extern DSArray *trash_x, *trash_y, *trash_on;
extern DSArray *bud_x, *bud_y, *bud_step, *bud_flee, *bud_say, *bud_say_t;
extern DSArray *food_x, *food_y, *food_on;
extern DSArray *pop_x, *pop_y, *pop_t, *pop_kind, *pop_id;
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

/* Скриншот текущего кадра в PPM - удобно смотреть глазами. */
static void shot(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "test/shots/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
    for (int i = 0; i < g_w * g_h; i++) {
        /* Кадровый буфер движка - ABGR: красный лежит в младшем байте. */
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

/* Кнопки лобби: ряд 0 - Играть, 1 - Привычки, 2 - Как играть, 3 - Настройки. */
static void tap_menu_row(int row) {
    float x = (float)g_w / 2.0f;
    float y = (float)(g_h / 2 - 140 + row * 80) + (float)btn_h / 2.0f;
    do_tap(x, y);
}
static void tap_back(void) { do_tap((float)g_w / 2.0f, (float)back_y + (float)btn_h / 2.0f); }
static void tap_hold(void) { do_tap((float)g_w - 140.0f, (float)g_h - 150.0f); }
static void tap_center(void) { do_tap((float)g_w / 2.0f, (float)g_h / 2.0f); }

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
static int walk_to(double tx, double ty, double reach, int limit) {
    for (int i = 0; i < limit; i++) {
        steer_to(tx, ty);
        if (!run_frames(1)) return 0;
        double dx = tx - yfield(Y_X), dy = ty - yfield(Y_Y);
        if (dx * dx + dy * dy < reach * reach) { release_joy(); return 1; }
        if (over != 0) { release_joy(); return 1; }
    }
    release_joy();
    return 0;
}

static int nearest_on(DSArray *on, DSArray *xs, DSArray *ys) {
    int best = -1; double bd = 1e18;
    for (int i = 0; i < (int)arr_len(on); i++) {
        if (arr_get(on, i) != 1) continue;
        double dx = arr_get(xs, i) - yfield(Y_X), dy = arr_get(ys, i) - yfield(Y_Y);
        double d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Полный трудовой день: мусор -> урна, пока день не закрыт. */
static int clean_the_day(void) {
    int guard = 0;
    while (over == 0 && guard++ < 80) {
        int t = nearest_on(trash_on, trash_x, trash_y);
        if (t >= 0 && yfield(Y_BAG) < 4) {
            if (!walk_to(arr_get(trash_x, t), arr_get(trash_y, t), 20, 900)) return 0;
        } else {
            if (!walk_to(bin_x, bin_y, 20, 900)) return 0;
            run_frames(2);
        }
        if (craving > max_craving * 0.6 && hold_cd <= 0) tap_hold();
    }
    return over == 2;
}

/* Пройти день целиком из лобби и оказаться в ночи. */
static int reach_the_night(void) {
    tap_menu_row(0);
    if (!wait_state(1, 60)) { printf("!! yard did not open\n"); return 0; }
    if (!clean_the_day()) { printf("!! day not finished (over=%g)\n", over); return 0; }
    tap_center();
    run_frames(5);
    if (phase != 1 || over != 0) { printf("!! night did not start (phase %g over %g)\n", phase, over); return 0; }
    return 1;
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

    /* --- 1. «Как играть» открывается и закрывается --- */
    tap_menu_row(2);
    if (!wait_state(3, 60)) { printf("!! how-to screen did not open\n"); return 3; }
    shot("02_howto");
    tap_back();
    if (!wait_state(0, 60)) { printf("!! how-to back failed\n"); return 3; }

    /* --- 2. День 1 во дворе --- */
    tap_menu_row(0);
    if (!wait_state(1, 60)) { printf("!! yard did not open\n"); return 3; }
    if (day != 1 || cleaned != 0 || over != 0 || phase != 0) {
        printf("!! bad day start: day %g cleaned %g phase %g\n", day, cleaned, phase); return 3;
    }
    printf("=== day 1 started: goal %g, trash on map %g, buddies %g\n",
           goal, arr_len(trash_on), arr_len(bud_x));
    run_frames(20);
    shot("03_yard");

    /* --- 2a. Наступил на собутыльника - взял у него бутылку --- */
    {
        double taken0 = drinks;
        int got = 0;
        for (int i = 0; i < 1200 && !got; i++) {
            steer_to(arr_get(bud_x, 0), arr_get(bud_y, 0));
            if (!run_frames(1)) return 3;
            if (drinks > taken0) got = 1;
        }
        release_joy();
        if (!got) { printf("!! stepping on a buddy did not take a bottle\n"); return 3; }
        printf("=== took a bottle from a buddy: drinks %g\n", drinks);
        shot("03b_buddy");
    }

    if (!clean_the_day()) { printf("!! day was not finished, over=%g craving=%g\n", over, craving); return 3; }
    printf("=== day 1 done: cleaned %g/%g, drinks %g, reward %g, willpower %g, best %g\n",
           cleaned, goal, drinks, last_reward, willpower, best_day);
    if (best_day != 1 || willpower <= 0) { printf("!! day rewards missing\n"); return 3; }
    if (drinks < 1) { printf("!! drinks counter reset before the alcohol test\n"); return 3; }
    if (last_reward != 5) { printf("!! drunk day must not get the sober bonus: %g\n", last_reward); return 3; }
    run_frames(2);
    shot("04_alcotest");

    /* --- 3. Ночь: холодильник тянет, еда лечит, кровать спасает --- */
    tap_center();
    run_frames(5);
    if (phase != 1 || over != 0) { printf("!! night did not start (phase %g)\n", phase); return 3; }
    printf("=== night 1 started: craving %g, food on floor %g\n", craving, arr_len(food_on));
    run_frames(20);
    shot("05_night");

    double d0 = hypot(fridge_x - yfield(Y_X), fridge_y - yfield(Y_Y));
    run_frames(90); /* стоим и ничего не делаем - должно тянуть к холодильнику */
    double d1 = hypot(fridge_x - yfield(Y_X), fridge_y - yfield(Y_Y));
    if (d1 >= d0 - 5) { printf("!! fridge does not pull: %g -> %g\n", d0, d1); return 3; }
    printf("=== fridge pulls Yarik in: %g -> %g\n", d0, d1);

    /* «Держись» ночью отпускает хватку холодильника. */
    tap_hold();
    double d2 = hypot(fridge_x - yfield(Y_X), fridge_y - yfield(Y_Y));
    run_frames(30);
    double d3 = hypot(fridge_x - yfield(Y_X), fridge_y - yfield(Y_Y));
    if (d3 < d2 - 1) { printf("!! hold on did not stop the pull: %g -> %g\n", d2, d3); return 3; }
    printf("=== hold on stops the pull for a moment (%g -> %g)\n", d2, d3);

    /* Еда сбивает тягу и считается. */
    int f = nearest_on(food_on, food_x, food_y);
    if (f < 0) { printf("!! no food at night\n"); return 3; }
    double craving_before_food = craving;
    if (!walk_to(arr_get(food_x, f), arr_get(food_y, f), 20, 900)) { printf("!! cannot reach food\n"); return 3; }
    run_frames(2);
    if (eaten < 1) { printf("!! food not eaten\n"); return 3; }
    if (craving >= craving_before_food) { printf("!! food did not cut craving: %g -> %g\n", craving_before_food, craving); return 3; }
    printf("=== ate a portion: eaten %g, craving %g -> %g\n", eaten, craving_before_food, craving);
    shot("06_night_eat");

    if (!walk_to(bed_x, bed_y, 40, 1800)) { printf("!! cannot reach the bed\n"); return 3; }
    run_frames(3);
    if (over != 3) { printf("!! night not survived (over=%g)\n", over); return 3; }
    printf("=== night 1 survived: +%g willpower (total %g)\n", last_reward, willpower);
    shot("07_night_done");

    /* --- 4. Следующий день сложнее --- */
    double goal1 = goal;
    tap_center();
    run_frames(5);
    if (day != 2 || phase != 0 || over != 0) { printf("!! day 2 did not start (day %g phase %g)\n", day, phase); return 3; }
    if (goal <= goal1) { printf("!! day 2 is not harder: %g -> %g\n", goal1, goal); return 3; }
    printf("=== day 2 started: goal %g\n", goal);

    /* --- 5. «Держись» сбивает тягу днём --- */
    run_frames(240);
    double before = craving;
    if (before < 5) { printf("!! craving does not grow: %g\n", before); return 3; }
    tap_hold();
    run_frames(1);
    if (craving >= before) { printf("!! hold on did not cut craving: %g -> %g\n", before, craving); return 3; }
    printf("=== hold on works: craving %g -> %g (cooldown %g)\n", before, craving, hold_cd);

    /* --- 6. Ничего не делаем: тяга добивает -> срыв --- */
    double will_before = willpower;
    for (int i = 0; i < 60 * 240 && over == 0; i++) run_frames(1);
    if (over != 1) { printf("!! relapse did not happen, craving %g\n", craving); return 3; }
    if (willpower != will_before) { printf("!! willpower changed on relapse: %g -> %g\n", will_before, willpower); return 3; }
    printf("=== relapse on day %g, willpower kept: %g\n", day, willpower);
    run_frames(2);
    shot("08_relapse");

    tap_center();
    if (!wait_state(0, 120)) { printf("!! relapse tap did not return to lobby\n"); return 3; }

    /* --- 7. Новый забег: холодильник - это срыв --- */
    if (!reach_the_night()) return 3;
    if (day != 1) { printf("!! run did not reset to day 1: %g\n", day); return 3; }
    printf("=== new run starts from day 1 with a clean slate\n");
    if (!walk_to(fridge_x, fridge_y, 60, 1200)) { printf("!! cannot reach the fridge\n"); return 3; }
    run_frames(3);
    if (over != 5) { printf("!! opening the fridge is not a relapse (over=%g)\n", over); return 3; }
    printf("=== fridge opened -> relapse\n");
    shot("09_fridge");
    tap_center();
    if (!wait_state(0, 120)) { printf("!! fridge relapse tap did not return to lobby\n"); return 3; }

    /* --- 8. Ещё забег: лишняя порция еды - тоже срыв --- */
    if (!reach_the_night()) return 3;
    int guard = 0;
    while (over == 0 && guard++ < 40) {
        int i = nearest_on(food_on, food_x, food_y);
        if (i < 0) break;
        if (!walk_to(arr_get(food_x, i), arr_get(food_y, i), 20, 900)) break;
        run_frames(2);
        if (hold_cd <= 0 && craving > max_craving * 0.6) tap_hold();
    }
    if (over != 4) { printf("!! overeating is not a relapse (over=%g eaten=%g)\n", over, eaten); return 3; }
    printf("=== ate one portion too many (%g) -> relapse\n", eaten);
    shot("10_overeat");
    tap_center();
    if (!wait_state(0, 120)) { printf("!! overeat tap did not return to lobby\n"); return 3; }

    /* --- 8a. Слишком много взял за день - проверка на алкоголь провалена --- */
    tap_menu_row(0);
    if (!wait_state(1, 60)) { printf("!! yard did not open for the alcohol test run\n"); return 3; }
    drinks = drink_limit; /* как будто выпросил три бутылки за день */
    if (!clean_the_day()) {
        if (over != 6) { printf("!! failed alcohol test expected, got over=%g\n", over); return 3; }
    }
    if (over != 6) { printf("!! alcohol test not failed (over=%g drinks=%g)\n", over, drinks); return 3; }
    printf("=== alcohol test failed with %g bottles -> relapse\n", drinks);
    shot("10b_alcotest_failed");
    tap_center();
    if (!wait_state(0, 120)) { printf("!! failed-test tap did not return to lobby\n"); return 3; }

    /* --- 9. Привычки: пять бесконечных веток --- */
    tap_menu_row(1);
    if (!wait_state(2, 60)) { printf("!! habits screen did not open\n"); return 3; }
    shot("11_habits");
    willpower = 5000;
    double cw = 232, gap = 18, total = cw * 5 + gap * 4;
    double x0 = (g_w - total) / 2, cy = back_y + btn_h + 40 + 300 - 66 + 24;
    for (int i = 0; i < 5; i++) do_tap((float)(x0 + i * (cw + gap) + cw / 2), (float)cy);
    run_frames(2);
    if (up_speed != 1 || up_bag != 1 || up_hold != 1 || up_night != 1 || up_food != 1) {
        printf("!! habits not bought: %g %g %g %g %g\n", up_speed, up_bag, up_hold, up_night, up_food);
        return 3;
    }
    /* Прокачка бесконечная: жмём кроссовки ещё пять раз, цена растёт. */
    double will_at_tier1 = willpower;
    for (int i = 0; i < 5; i++) { do_tap((float)(x0 + cw / 2), (float)cy); run_frames(1); }
    if (up_speed != 6) { printf("!! sneakers stopped at tier %g\n", up_speed); return 3; }
    double spent = will_at_tier1 - willpower;
    if (spent < 5 * 29) { printf("!! upgrade cost does not grow: spent %g for 5 tiers\n", spent); return 3; }
    printf("=== habits: speed %g, bag %g, hold %g, grit %g, supper %g (spent %g for tiers 2-6)\n",
           up_speed, up_bag, up_hold, up_night, up_food, spent);
    shot("12_habits_bought");

    /* Прогресс переживает перезапуск скрипта. */
    double saved_will = willpower, saved_best = best_day;
    ds_call_protected(protected_reset, NULL, "reset");
    arr_free(trash_x); arr_free(trash_y); arr_free(trash_on);
    arr_free(bud_x); arr_free(bud_y); arr_free(bud_step);
    arr_free(bud_flee); arr_free(bud_say); arr_free(bud_say_t);
    arr_free(food_x); arr_free(food_y); arr_free(food_on);
    arr_free(pop_x); arr_free(pop_y); arr_free(pop_t); arr_free(pop_kind); arr_free(pop_id);
    script_active = 0;
    ds_clear_runtime_error(); ds_string_pool_reset();
    if (!ds_call_protected(protected_init, NULL, "init")) { printf("!! restart failed\n"); return 3; }
    script_active = 1;
    run_frames(3);
    if (willpower != saved_will || best_day != saved_best || up_speed != 6 || up_bag != 1 || up_food != 1) {
        printf("!! progress not restored: will %g/%g best %g/%g speed %g bag %g supper %g\n",
               willpower, saved_will, best_day, saved_best, up_speed, up_bag, up_food);
        return 3;
    }
    printf("=== progress restored after restart: willpower %g, best day %g, speed tier %g\n",
           willpower, best_day, up_speed);

    ds_call_protected(protected_reset, NULL, "reset");
    /* reset() заново создаёт пустые массивы - освобождаем их, чтобы ASAN
     * заканчивал прогон без «утечек» и любой ненулевой код выхода был бедой. */
    arr_free(trash_x); arr_free(trash_y); arr_free(trash_on);
    arr_free(bud_x); arr_free(bud_y); arr_free(bud_step);
    arr_free(bud_flee); arr_free(bud_say); arr_free(bud_say_t);
    arr_free(food_x); arr_free(food_y); arr_free(food_on);
    arr_free(pop_x); arr_free(pop_y); arr_free(pop_t); arr_free(pop_kind); arr_free(pop_id);
    ds_string_pool_reset();
    ds_release_assets();
    free(g_pixels);
    printf("=== DONE ok (%ld frames)\n", g_frame);
    return 0;
}
