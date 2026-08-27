/* Мини-харнесс для визуальных снимков: гоняет игру headless и сохраняет
 * BMP-кадры (лобби, бой с пылью позади игрока, магазин, лидерборд).
 * Сборка:
 *   gcc -O1 -D_POSIX_C_SOURCE=200809L -I. -Ig -DNO_NET \
 *       test/visual_dump.c runtime.c graphics.c net.c sound.c game/game.c \
 *       -lpthread -lm -o test/visual_dump
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "runtime.h"
#include "net.h"

extern double game_state, t_dir, t_fade, player_class, cups, candies, santa_owned, dust_back_off, dust_max, warn_open;
extern void *player;
extern DSArray *dust;
#define DUST_FIELDS 5

static uint32_t *g_pixels;
static int g_w = 1280, g_h = 720;
static long g_frame = 0;

static void p_init(void *u) { (void)u; init(NULL); }
static void p_update(void *u) { (void)u; update(); }
static void p_draw(void *u) { draw((Buffer *)u); }
typedef struct { float x, y; int action, id; } TouchCall;
static void p_touch(void *u) { TouchCall *c = (TouchCall *)u; touch(c->x, c->y, c->action, c->id); }

static void run_frames(int n) {
    for (int i = 0; i < n; i++) {
        dt = 1.0f / 60.0f;
        if (!ds_call_protected(p_update, NULL, "update")) { fprintf(stderr, "update failed\n"); exit(1); }
        Buffer buf = { g_pixels, g_w, g_h, g_w };
        if (!ds_graphics_begin_frame(&buf)) { fprintf(stderr, "frame failed\n"); exit(1); }
        ds_call_protected(p_draw, &buf, "draw");
        ds_graphics_end_frame();
        g_frame++;
    }
}
static void do_tap(float x, float y) {
    TouchCall c = { x, y, 0, 1 };
    ds_call_protected(p_touch, &c, "touch");
    c.action = 1;
    ds_call_protected(p_touch, &c, "touch");
}
static void do_hold(float x, float y) {
    TouchCall c = { x, y, 0, 7 };
    ds_call_protected(p_touch, &c, "touch");
}
static void do_release(void) {
    TouchCall c = { 0, 0, 1, 7 };
    ds_call_protected(p_touch, &c, "touch");
}
static int wait_state(double want, int iters) {
    for (int i = 0; i < iters; i++) {
        run_frames(2);
        if (game_state == want && t_dir == 0) return 1;
    }
    return 0;
}
static void dump_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    int row = g_w * 3, pad = (4 - (row % 4)) % 4, size = 54 + (row + pad) * g_h;
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=size&0xff; hdr[3]=(size>>8)&0xff; hdr[4]=(size>>16)&0xff; hdr[5]=(size>>24)&0xff;
    hdr[10]=54;
    hdr[14]=40; hdr[18]=g_w&0xff; hdr[19]=(g_w>>8)&0xff; hdr[20]=(g_w>>16)&0xff;
    hdr[22]=g_h&0xff; hdr[23]=(g_h>>8)&0xff; hdr[24]=(g_h>>16)&0xff;
    hdr[26]=1; hdr[28]=24;
    fwrite(hdr, 1, 54, f);
    for (int y = g_h - 1; y >= 0; y--) {
        for (int x = 0; x < g_w; x++) {
            uint32_t p = g_pixels[y * g_w + x];
            uint8_t px[3] = { (uint8_t)(p & 0xff), (uint8_t)((p >> 8) & 0xff), (uint8_t)((p >> 16) & 0xff) };
            fwrite(px, 1, 3, f);
        }
        for (int i = 0; i < pad; i++) fputc(0, f);
    }
    fclose(f);
    printf("saved %s (frame %ld)\n", path, g_frame);
}

int main(void) {
    setbuf(stdout, NULL);
    screen_w = g_w; screen_h = g_h;
    g_pixels = (uint32_t *)calloc((size_t)g_w * g_h, 4);
    if (!g_pixels) return 2;
    remove("auth.dat"); remove("progress.dat");

    ds_clear_runtime_error(); ds_clear_script_restart(); ds_string_pool_reset();
    if (!ds_call_protected(p_init, NULL, "init")) { fprintf(stderr, "init failed\n"); return 2; }

    /* Лобби с тремя валютами. */
    run_frames(10);
    /* Предупреждение об эпилепсии держит экран 2.5 с и глотает тапы —
     * ждём, пока оно полностью не растает. */
    while (warn_open != 0 && g_frame < 600) run_frames(5);
    dump_bmp("shot_lobby.bmp");

    /* Соло-бой: Дед Мороз с ультой, движение вправо — след пыли позади. */
    player_class = 2; santa_owned = 1;
    do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 - 88));      /* Играть */
    wait_state(2, 30);
    do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 - 8));      /* Соло */
    if (!wait_state(1, 30)) { fprintf(stderr, "solo did not start\n"); return 3; }
    run_frames(30);
    do_hold(190, (float)(g_h - 150));                                  /* джойстик вправо */
    run_frames(55);
    /* Численная проверка следа: игрок идёт вправо, значит вся живая пыль
     * должна лежать ПОЗАДИ (левее) центра игрока. */
    {
        double *pl = (double *)player;
        int alive = 0, behind = 0;
        double max_x = -1e9;
        for (int i = 0; i < (int)dust_max; i++) {
            double life = arr_get(dust, i * DUST_FIELDS + 4);
            if (life <= 0) continue;
            alive++;
            double dx = arr_get(dust, i * DUST_FIELDS);
            if (dx < pl[0] - 10) behind++;
            if (dx > max_x) max_x = dx;
        }
        printf("dust check: alive=%d behind=%d player_x=%g max_dust_x=%g\n",
               alive, behind, pl[0], max_x);
        if (alive < 3 || behind != alive) {
            printf("!! dust trail is not behind the player (alive=%d behind=%d)\n", alive, behind);
            return 4;
        }
    }
    dump_bmp("shot_trail.bmp");
    /* Замах ульты: кадр в середине «пустого» удара. */
    do_tap((float)(g_w - 140), (float)(g_h - 310));
    run_frames(6);
    dump_bmp("shot_super_windup.bmp");
    run_frames(20);
    dump_bmp("shot_super_throw.bmp");
    do_release();

    /* Магазин: карточки классов, уровни и кнопка прайм-обмена. */
    do_tap((float)(g_w - 280) / 2 + 140, 32.0f + 32.0f);               /* Назад */
    wait_state(0, 40);
    do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 + 72));      /* Магазин */
    if (!wait_state(8, 30)) { fprintf(stderr, "shop did not open\n"); return 3; }
    run_frames(5);
    dump_bmp("shot_shop.bmp");

    /* Лидерборд: ник, место и кубки справа. */
    net_leaderboard_fetch("http://127.0.0.1:18765");
    do_tap((float)(g_w - 280) / 2 + 140, 32.0f + 32.0f);               /* Назад */
    wait_state(0, 40);
    do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 - 88));      /* Играть */
    wait_state(2, 30);
    do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 + 152));     /* Таблица лидеров */
    if (!wait_state(10, 30)) { fprintf(stderr, "leaderboard did not open\n"); return 3; }
    run_frames(40);
    dump_bmp("shot_leaderboard.bmp");

    reset();
    printf("done\n");
    return 0;
}
