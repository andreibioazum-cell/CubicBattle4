/* Headless Linux harness for the DimScript game.
 * Drives the real script (init/update/draw/touch) against a fake Firebase
 * HTTP server so the full online path (nick/pass entry, claim slot, push/read,
 * chat) runs on real sockets and threads. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <strings.h>
#include "runtime.h"
#include "net.h"

/* globals generated into game.c (non-static) — read them for debugging */
extern double game_state, chat_open, login_field, login_status, t_dir;
extern double player_class, azum_revived, finished, cups, candies, azum_owned, santa_owned, cups_awarded, player_level, candy_count;
extern DSArray *candy_x, *candy_y;
extern const char *login_nick, *login_pass, *chat_input;
extern void *player, *enemy, *punch;
extern DSArray *remotes, *remote_punches;
extern double enemy_cooldown_min, enemy_cooldown_max;
/* Поля Enemy идут в объявленном в entities.ds порядке: x,y,size,hp,max_hp,
 * angle,state,state_time,cooldown,... — читаем их как массив double. */
#define ENEMY_ANGLE 5
#define ENEMY_STATE 6
#define ENEMY_COOLDOWN 8
#define ENEMY_FREEZE 23
#define ENEMY_FREEZE_SLOW 24

static uint32_t *g_pixels = NULL;
static int g_w = 1280, g_h = 720;

/* ------------------------------------------------------------------ */
/* test HTTP client: plain HTTP against 127.0.0.1:PORT                */
/* ------------------------------------------------------------------ */
#define TEST_PORT 18765
int test_http_impl(const char *method, const char *url, const char *body,
                   char *out, size_t cap,
                   const char *header, const char *value,
                   char *etag, size_t etag_cap) {
    char host[256]; int port = TEST_PORT; const char *path = NULL;
    if (out && cap) out[0] = '\0';
    if (etag && etag_cap) etag[0] = '\0';
    if (!url || strncmp(url, "http://", 7) != 0) return 0;
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    if (!hl || hl >= sizeof(host)) return 0;
    memcpy(host, p, hl); host[hl] = 0;
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); if (port <= 0) port = TEST_PORT; }
    path = slash ? slash : "/";
    if (strcmp(host, "127.0.0.1") != 0 && strcmp(host, "localhost") != 0) return 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return 0; }
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[4096];
    int rl = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nContent-Type: application/json\r\n",
                      method, path, host, port);
    if (header && value) rl += snprintf(req + rl, sizeof(req) - (size_t)rl, "%s: %s\r\n", header, value);
    if (body && *body) rl += snprintf(req + rl, sizeof(req) - (size_t)rl, "Content-Length: %zu\r\n", strlen(body));
    rl += snprintf(req + rl, sizeof(req) - (size_t)rl, "\r\n");
    if (body && *body) rl += snprintf(req + rl, sizeof(req) - (size_t)rl, "%s", body);

    ssize_t sent = send(fd, req, (size_t)rl, 0);
    if (sent != rl) { close(fd); return 0; }

    char resp[65536]; size_t rt = 0;
    for (;;) {
        ssize_t n = recv(fd, resp + rt, sizeof(resp) - 1 - rt, 0);
        if (n <= 0) break;
        rt += (size_t)n;
        if (rt >= sizeof(resp) - 1) break;
    }
    close(fd);
    resp[rt] = '\0';
    if (rt < 12) return 0;
    int code = 0;
    if (sscanf(resp, "HTTP/1.%*d %d", &code) != 1) return 0;
    char *hdr_end = strstr(resp, "\r\n\r\n");
    if (!hdr_end) return 0;
    *hdr_end = '\0';
    const char *rb = hdr_end + 4;
    if (etag && etag_cap) {
        const char *e = strstr(resp, "\r\nETag:");
        if (!e) e = strstr(resp, "ETag:");
        if (e) {
            const char *v = strchr(e, ':');
            if (v) { v++; while (*v == ' ') v++; size_t n = strcspn(v, "\r\n"); if (n && n < etag_cap) { memcpy(etag, v, n); etag[n] = 0; } }
        }
    }
    if (out && cap) {
        size_t n = strlen(rb);
        if (n >= cap) n = cap - 1;
        memcpy(out, rb, n); out[n] = '\0';
    }
    return code;
}

/* ------------------------------------------------------------------ */
/* harness loop                                                        */
/* ------------------------------------------------------------------ */
static int script_active = 0;
static void protected_init(void *u) { (void)u; init(NULL); }
static void protected_reset(void *u) { (void)u; reset(); }
static void protected_update(void *u) { (void)u; update(); }
static void protected_draw(void *u) { draw((Buffer *)u); }
typedef struct { float x, y; int action, id; } TouchCall;
static void protected_touch(void *u) { TouchCall *c = (TouchCall *)u; touch(c->x, c->y, c->action, c->id); }

static void feed_text(const char *s) { keyboard_type(s); }

static void do_tap(float x, float y) {
    TouchCall c;
    c.x = x; c.y = y; c.action = 0; c.id = 1;
    ds_call_protected(protected_touch, &c, "touch");
    c.action = 1;
    ds_call_protected(protected_touch, &c, "touch");
}

static long g_frame = 0;
static void run_frames(int n) {
    for (int i = 0; i < n; i++) {
        dt = 1.0 / 60.0;
        if (script_active) {
            if (!ds_call_protected(protected_update, NULL, "update")) {
                printf("[frame %ld] UPDATE FAILED: %s\n", g_frame, ds_runtime_error_message());
                script_active = 0; return;
            }
        }
        Buffer buf = { g_pixels, g_w, g_h, g_w };
        if (!ds_graphics_begin_frame(&buf)) { printf("[frame %ld] begin_frame failed\n", g_frame); return; }
        if (script_active) {
            if (!ds_call_protected(protected_draw, &buf, "draw")) {
                printf("[frame %ld] DRAW FAILED: %s\n", g_frame, ds_runtime_error_message());
                script_active = 0;
            }
        }
        ds_graphics_end_frame();
        g_frame++;
    }
}

static int start_script(void) {
    ds_clear_runtime_error(); ds_clear_script_restart(); ds_string_pool_reset();
    if (!ds_call_protected(protected_init, NULL, "init")) return 0;
    script_active = 1;
    return 1;
}

static int wait_state(double want, int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        run_frames(2);
        if (game_state == want && t_dir == 0) return 1;
    }
    return 0;
}

#define REMOTE_FIELDS 11
static int remote_count(void) {
    int count = 0;
    for (int slot = 0; slot < 4; slot++) if (arr_get(remotes, slot * REMOTE_FIELDS) == 1) count++;
    return count;
}
static int first_remote_slot(void) {
    for (int slot = 0; slot < 4; slot++) if (arr_get(remotes, slot * REMOTE_FIELDS) == 1) return slot;
    return -1;
}
static int wait_remotes(int want_players, int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        run_frames(2);
        if (remote_count() == want_players) return 1;
        { struct timespec ts = { 0, 20 * 1000 * 1000 }; nanosleep(&ts, NULL); }
    }
    return 0;
}

static int wait_slot(int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        run_frames(5);
        if (net_slot() >= 0 && net_status() == NET_PLAYING) return 1;
    }
    return 0;
}

/* Экран аккаунта (1280x720): ник fy = 240, пароль py = 326, кнопка yb = 410 */
#define LOGIN_FY (g_h / 2 - 100 - 20)
#define LOGIN_PY (LOGIN_FY + 50 + 36)
#define LOGIN_YB (LOGIN_PY + 50 + 34)

static void tap_nick(void) { do_tap((float)(g_w / 2), (float)(LOGIN_FY + 25)); }
static void tap_pass(void) { do_tap((float)(g_w / 2), (float)(LOGIN_PY + 25)); }
static void tap_login_btn(void) { do_tap((float)(g_w / 2), (float)(LOGIN_YB + 32)); }

/* Заполнить поле заново: тап (переключение поля чистит клавиатуру), затем текст */
static void fill_field(void (*tap)(void), const char *text) {
    tap();
    run_frames(5);
    keyboard_clear();
    feed_text(text);
    run_frames(5);
}

/* Лобби: общий столбец начинается с screen_h/2-120. */
static void tap_play(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 - 88)); }
/* Моды: «Назад» на месте Play, Solo, Online, Leaderboard. */
static void tap_solo(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 - 8)); }
static void tap_online(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 + 72)); }
static void tap_leaderboard(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 + 152)); }
static void tap_back(void) { do_tap((float)(g_w - 280) / 2 + 140, 32 + 32); }
static void tap_account(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 + 152)); }
/* Лобби: «Классы» третья кнопка (my+160). */
static void tap_classes(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 + 72)); }
/* Экран классов: три карточки 260x300 с зазором 26 */
static void tap_class_ordinary(void) { do_tap(354.0f, 262.0f); }
static void tap_class_azum(void) { do_tap(640.0f, 262.0f); }
static void tap_class_santa(void) { do_tap(926.0f, 262.0f); }
static void tap_levels_btn(void) { do_tap((float)(g_w / 2), 470.0f); }
static void tap_level_row(int n) {
    float y = 32.0f + 64.0f + 52.0f + (float)(n - 1) * 88.0f + 39.0f;
    do_tap((float)(g_w / 2), y);
}

int main(void) {
    setbuf(stdout, NULL);
    screen_w = g_w; screen_h = g_h;
    g_pixels = (uint32_t *)calloc((size_t)g_w * g_h, 4);
    if (!g_pixels) { printf("OOM\n"); return 2; }

    char nick[32], pass[32] = "pass12345";
    snprintf(nick, sizeof(nick), "User%ld", (long)(time(NULL) % 100000));

    remove("auth.dat");
    remove("progress.dat");

    if (!start_script()) { printf("init failed: %s\n", ds_runtime_error_message()); return 2; }
    printf("=== init ok (frame %ld)\n", g_frame);
    run_frames(10);

    /* --- 1. Без сохранённого аккаунта «Онлайн» открывает экран входа --- */
    if (net_login_status() != 0) { printf("!! expected idle login status on fresh start, got %g\n", net_login_status()); return 3; }
    tap_play(); wait_state(2, 30);
    tap_online(); wait_state(7, 30);
    printf("=== nick/pass screen opened (frame %ld)\n", g_frame);

    /* Поле ника в фокусе сразу */
    feed_text("TypeMe");
    run_frames(5);
    if (!login_nick || strcmp(login_nick, "TypeMe") != 0) {
        printf("!! cannot type into nick field without tapping it: '%s'\n",
               login_nick ? login_nick : "(null)");
        return 3;
    }
    printf("=== nick field accepts typing without a tap\n");

    keyboard_hide();
    run_frames(5);
    if (keyboard_visible()) { printf("!! keyboard did not hide\n"); return 3; }
    tap_nick();
    run_frames(5);
    if (!keyboard_visible()) { printf("!! tap on the field did not reopen the keyboard\n"); return 3; }
    feed_text("StillHere");
    run_frames(5);
    if (!login_nick || strcmp(login_nick, "TypeMeStillHere") != 0) {
        printf("!! typed text lost after keyboard reopen: '%s' (expected 'TypeMeStillHere')\n",
               login_nick ? login_nick : "(null)");
        return 3;
    }
    printf("=== keyboard reopen on tap keeps the typed text\n");
    keyboard_clear();
    run_frames(5);

    /* --- 2. Короткий ник отклоняется --- */
    fill_field(tap_nick, "ab");
    fill_field(tap_pass, pass);
    tap_login_btn();
    run_frames(10);
    if (login_status != 5) { printf("!! short nick was not rejected, status=%g\n", login_status); return 3; }
    if (game_state != 7) { printf("!! left login screen despite bad nick\n"); return 3; }
    printf("=== short nick rejected locally (frame %ld)\n", g_frame);

    /* --- 3. Ник с запрещёнными символами отклоняется --- */
    fill_field(tap_nick, "bad nick!");
    fill_field(tap_pass, pass);
    tap_login_btn();
    run_frames(10);
    if (login_status != 5) { printf("!! invalid-char nick was not rejected, status=%g\n", login_status); return 3; }
    printf("=== invalid-char nick rejected locally (frame %ld)\n", g_frame);

    /* --- 3b. Короткий пароль отклоняется --- */
    fill_field(tap_nick, nick);
    fill_field(tap_pass, "1");
    tap_login_btn();
    run_frames(10);
    if (login_status != 7) { printf("!! short pass was not rejected, status=%g\n", login_status); return 3; }
    printf("=== short password rejected (frame %ld)\n", g_frame);

    /* --- 4. Регистрация и вход с валидным ником и паролем --- */
    fill_field(tap_nick, nick);
    fill_field(tap_pass, pass);
    tap_login_btn();
    if (!wait_state(5, 40)) { printf("!! did not enter online after auth, state=%g status=%g\n", game_state, login_status); return 3; }
    if (!wait_slot(80)) { printf("!! online entry failed: net status=%g slot=%g\n", net_status(), net_slot()); return 3; }
    printf("=== online with nick '%s': status=%g slot=%g count=%g (frame %ld)\n", nick, net_status(), net_slot(), net_count(), g_frame);

    /* --- 5. Чат: открыть -> написать -> отправить --- */
    do_tap(86, 174);
    run_frames(10);
    if (chat_open != 1) { printf("!! chat did not open\n"); return 3; }
    do_tap(100, (float)(g_h - 45));
    run_frames(5);
    feed_text("hi from test");
    run_frames(5);
    do_tap((float)(g_w - 60), (float)(g_h - 45)); /* send */
    for (int i = 0; i < 60; i++) {
        run_frames(2);
        if (net_chat_count() >= 1) break;
    }
    if (net_chat_count() < 1) { printf("!! chat message not registered\n"); return 3; }
    printf("=== chat send ok (frame %ld)\n", g_frame);

    /* Закрыть чат */
    do_tap((float)(g_w - 140), 32 + 32);
    run_frames(10);
    if (chat_open != 0) { printf("!! chat did not close\n"); return 3; }

    /* --- 6. Проверка соперника в слоте 1 --- */
    if (!wait_remotes(1, 100)) { printf("!! remote player not seen\n"); return 3; }
    printf("=== remote player visible (remotes=%d)\n", remote_count());

    /* Убираем игрока с сервера */
    {
        int rslot = first_remote_slot();
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/rooms/main/players/%d.json", TEST_PORT, rslot);
        test_http_impl("DELETE", url, NULL, NULL, 0, NULL, NULL, NULL, 0);
        printf("=== remote slot %d deleted on the server\n", rslot);
    }
    if (!wait_remotes(0, 300)) { printf("!! player who left was not removed, remotes=%d\n", remote_count()); return 3; }
    printf("=== player who left removed from the list (frame %ld)\n", g_frame);

    /* Выйти из онлайна */
    tap_back();
    if (!wait_state(0, 60)) { printf("!! did not return to lobby from online\n"); return 3; }
    printf("=== left online (frame %ld)\n", g_frame);

    /* --- 7. Проверка auth.dat --- */
    {
        FILE *f = fopen("auth.dat", "r");
        char saved_n[64] = "", saved_p[64] = "";
        if (!f) { printf("!! auth.dat was not written\n"); return 3; }
        fscanf(f, "%63s %63s", saved_n, saved_p);
        fclose(f);
        if (strcmp(saved_n, nick) != 0 || strcmp(saved_p, pass) != 0) {
            printf("!! auth.dat has '%s %s', expected '%s %s'\n", saved_n, saved_p, nick, pass);
            return 3;
        }
        printf("=== account persisted to auth.dat\n");
    }

    /* --- 8. Неверный пароль к существующему аккаунту должен отклоняться --- */
    {
        double bad_auth = net_auth("http://127.0.0.1:18765", nick, "wrongpass");
        if (bad_auth != (double)NET_LOGIN_WRONG_PASS) {
            printf("!! wrong password was not rejected: result=%g (expected %d)\n", bad_auth, NET_LOGIN_WRONG_PASS);
            return 3;
        }
        printf("=== wrong password rejected by server auth\n");
    }

    /* --- 9. Лидерборд по кубкам --- */
    tap_play(); wait_state(2, 30);
    tap_leaderboard();
    if (!wait_state(10, 30)) { printf("!! leaderboard screen did not open, state=%g\n", game_state); return 3; }
    run_frames(30);
    printf("=== leaderboard opened, count=%g status=%g (frame %ld)\n", net_leaderboard_count(), net_leaderboard_status(), g_frame);
    tap_back();
    if (!wait_state(2, 30)) { printf("!! did not return to modes from leaderboard\n"); return 3; }
    printf("=== returned from leaderboard (frame %ld)\n", g_frame);

    /* --- 10. Соло-бой и бот --- */
    tap_solo(); wait_state(1, 30);
    run_frames(40);
    printf("=== solo battle ok (frame %ld)\n", g_frame);

    {
        double *e = (double *)enemy;
        double *pl = (double *)player;
        double prev = e[ENEMY_STATE], cds[64], locked_ang = 0;
        int attacks = 0, ncd = 0, distinct = 0, snap = 0, have_lock = 0;
        for (int i = 0; i < 900 && finished == 0; i++) {
            run_frames(1);
            double st = e[ENEMY_STATE];
            if (st == 1 && prev != 1) attacks++;
            if (st == 2) {
                if (!have_lock) { locked_ang = e[ENEMY_ANGLE]; have_lock = 1; }
                else {
                    pl[0] += 25;
                    if (e[ENEMY_ANGLE] < locked_ang - 0.08 || e[ENEMY_ANGLE] > locked_ang + 0.08) snap = 1;
                }
            } else have_lock = 0;
            if (st == 3 && prev == 2 && ncd < 64) cds[ncd++] = e[ENEMY_COOLDOWN];
            prev = st;
        }
        if (snap) { printf("!! bot snap-aimed at the player during the punch\n"); return 3; }
        if (attacks < 4) { printf("!! bot attacked only %d times in 15s\n", attacks); return 3; }
        for (int i = 0; i < ncd; i++) {
            if (cds[i] < enemy_cooldown_min - 1e-9 || cds[i] > enemy_cooldown_max + 1e-9) {
                printf("!! bot cooldown %g out of range [%g..%g]\n", cds[i], enemy_cooldown_min, enemy_cooldown_max);
                return 3;
            }
            if (i && cds[i] != cds[0]) distinct = 1;
        }
        if (ncd < 3 || !distinct) { printf("!! bot cooldown is not random (%d samples)\n", ncd); return 3; }
        printf("=== bot attacks fast, cooldown random: %d attacks, %d cooldowns in [%g..%g]\n",
               attacks, ncd, enemy_cooldown_min, enemy_cooldown_max);
    }
    tap_back(); wait_state(0, 40);

    /* --- 11. Классы и сохранение в облако --- */
    tap_classes();
    if (!wait_state(8, 30)) { printf("!! classes tab did not open, state=%g\n", game_state); return 3; }
    cups = 50;
    tap_class_azum();
    run_frames(5);
    if (player_class != 1 || azum_owned != 1 || cups != 0) {
        printf("!! Azum buy failed: class=%g owned=%g cups=%g\n", player_class, azum_owned, cups);
        return 3;
    }
    printf("=== classes tab: Azum bought and selected (frame %ld)\n", g_frame);
    tap_back(); wait_state(0, 30);

    /* --- 12. Повторный вход в онлайн с сохранённым аккаунтом --- */
    tap_play(); wait_state(2, 30);
    tap_online();
    if (!wait_state(5, 30)) { printf("!! saved account did not skip login screen, state=%g\n", game_state); return 3; }
    if (!wait_slot(80)) { printf("!! online re-entry failed\n"); return 3; }
    printf("=== online straight in with saved account: slot=%g (frame %ld)\n", net_slot(), g_frame);
    tap_back(); wait_state(0, 60);

    script_active = 1;
    ds_call_protected(protected_reset, NULL, "reset");
    printf("=== DONE ok\n");
    return 0;
}
