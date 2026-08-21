/* Headless Linux harness for the DimScript game.
 * Drives the real script (init/update/draw/touch) against a fake Firebase
 * HTTP server so the full online path (nick entry, claim slot, push/read,
 * chat) runs on real sockets and threads. Вход в онлайн — только ник,
 * без пароля и регистрации. */
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
extern double player_class, azum_revived, finished, cups, candies, azum_owned, santa_owned, cups_awarded, player_level, box_msg_t;
extern const char *box_msg;
extern const char *login_nick, *chat_input;
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
    /* ждём и состояние, и конец фейда перехода (t_dir==0), иначе тапы
     * во время затемнения игнорируются игрой */
    for (int i = 0; i < max_iters; i++) {
        run_frames(2);
        if (game_state == want && t_dir == 0) return 1;
    }
    return 0;
}

/* В remotes лежат четыре постоянных слота по 11 чисел; первое поле говорит,
 * занят ли слот соперником. Поле 10 — класс соперника. */
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

/* Экран ника (1280x720): поле fy = screen_h/2-60 + login_oy, login_oy = -26,
 * высота поля login_fh = 54, кнопка «Играть» на fy+login_fh+40 (см. menu.ds). */
#define FY (g_h / 2 - 60 - 26)
static void tap_nick(void) { do_tap((float)(g_w / 2), (float)(FY + 27)); }
static void tap_nick_go(void) { do_tap((float)(g_w / 2), (float)(FY + 54 + 40 + 32)); }

/* Заполнить поле заново: тап (переключение поля чистит клавиатуру), затем текст */
static void fill_field(void (*tap)(void), const char *text) {
    tap();
    run_frames(5);
    keyboard_clear();
    feed_text(text);
    run_frames(5);
}

/* Лобби: Play (center y 360-40+32=352) */
static void tap_play(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 - 8)); }
/* Моды: Solo (y 360-40+32=352), Online (360+40+32=432) */
static void tap_solo(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 - 8)); }
static void tap_online(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 + 72)); }
static void tap_back(void) { do_tap((float)(g_w - 280) / 2 + 140, 32 + 32); }
static void tap_account(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 + 152)); }
/* Лобби: «Классы» третья кнопка (my+160). Карточки обычного и Азума. */
static void tap_classes(void) { do_tap((float)(g_w - 280) / 2 + 140, (float)(g_h / 2 + 112)); }
/* Экран классов: три карточки 260x300 с зазором 26, ряд на y=112..412.
 * Центры карточек по x: Обычный 354, Азум 640, Дед Мороз 926 (y≈262). */
static void tap_class_ordinary(void) { do_tap(354.0f, 262.0f); }
static void tap_class_azum(void) { do_tap(640.0f, 262.0f); }
static void tap_class_santa(void) { do_tap(926.0f, 262.0f); }
static void tap_levels_btn(void) { do_tap(926.0f, 470.0f); }
/* Ящик в магазине: широкая кнопка под карточками (x 224..770, y 438..502). */
static void tap_box(void) { do_tap(497.0f, 470.0f); }
static void tap_level_row(int n) {
    float y = 32.0f + 64.0f + 52.0f + (float)(n - 1) * 88.0f + 39.0f;
    do_tap((float)(g_w / 2), y);
}

static void save_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int w = g_w, h = g_h;
    int row = (w * 3 + 3) & ~3;
    int img = row * h;
    unsigned char hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    unsigned int fsz = 54 + img;
    memcpy(hdr+2, &fsz, 4);
    unsigned int off = 54;
    memcpy(hdr+10, &off, 4);
    unsigned int dib = 40;
    memcpy(hdr+14, &dib, 4);
    memcpy(hdr+18, &w, 4);
    memcpy(hdr+22, &h, 4);
    unsigned short planes = 1, bpp = 24;
    memcpy(hdr+26, &planes, 2);
    memcpy(hdr+28, &bpp, 2);
    memcpy(hdr+34, &img, 4);
    fwrite(hdr, 1, 54, f);
    unsigned char *line = (unsigned char *)malloc((size_t)row);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t px = g_pixels[(size_t)y * w + x];
            line[x*3+0] = (unsigned char)(px & 0xff);
            line[x*3+1] = (unsigned char)((px >> 8) & 0xff);
            line[x*3+2] = (unsigned char)((px >> 16) & 0xff);
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
    printf("screenshot: %s\n", path);
}

int main(void) {
    setbuf(stdout, NULL); /* прогресс сценария виден сразу, без буфера */
    screen_w = g_w; screen_h = g_h;
    g_pixels = (uint32_t *)calloc((size_t)g_w * g_h, 4);
    if (!g_pixels) { printf("OOM\n"); return 2; }

    /* уникальный ник на каждый запуск */
    char nick[32];
    snprintf(nick, sizeof(nick), "User%ld", (long)(time(NULL) % 100000));

    remove("auth.dat"); /* чистый старт без сохранённой сессии */

    if (!start_script()) { printf("init failed: %s\n", ds_runtime_error_message()); return 2; }
    printf("=== init ok (frame %ld)\n", g_frame);
    run_frames(10);

    /* Проверка формата кадра (Windows-баг «цвета ломаются»):
     * кнопка Play (0x5F10A0) в буфере должна лежать как R,G,B,A ->
     * uint32 0xFFA0105F. После своппинга R<->B для GDI (B,G,R,X) должно
     * получиться 0xFF5FA010. */
    {
        uint32_t px = g_pixels[520 + 340 * g_w];
        if (px != 0xFFA0105Fu) {
            printf("!! pixel format broken: button pixel = 0x%08X, expected 0xFFA0105F\n", px);
            return 3;
        }
        uint32_t sw = ((px & 0xFFu) << 16) | (px & 0xFF00u) | ((px >> 16) & 0xFFu) | 0xFF000000u;
        if (sw != 0xFF5F10A0u) {
            printf("!! swizzle formula broken: got 0x%08X, expected 0xFF5FA010 (BGRX)\n", sw);
            return 3;
        }
        printf("=== framebuffer pixel format OK (RGBA -> BGRX swizzle verified)\n");
    }

    /* --- 1. Без сохранённого ника «Онлайн» открывает экран ника --- */
    if (net_login_status() != 0) { printf("!! expected idle login status on fresh start, got %g\n", net_login_status()); return 3; }
    tap_play(); wait_state(2, 30);
    tap_online(); wait_state(7, 30);
    printf("=== nick screen opened (frame %ld)\n", g_frame);

    /* Поле в фокусе сразу: писать можно без тапа по нему. */
    feed_text("TypeMe");
    run_frames(5);
    if (!login_nick || strcmp(login_nick, "TypeMe") != 0) {
        printf("!! cannot type into nick field without tapping it: '%s'\n",
               login_nick ? login_nick : "(null)");
        return 3;
    }
    printf("=== nick field accepts typing without a tap\n");

    /* Клавиатуру смахнули жестом, пока поле активно (на Android именно так
     * ломался ввод ника: флаг видимости залипал, тап клавиатуру не возвращал).
     * Тап по полю обязан заново открыть IME и не потерять набранное. */
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

    /* --- 2. Короткий ник отклоняется на месте, без сети --- */
    fill_field(tap_nick, "ab");
    tap_nick_go();
    run_frames(10);
    if (login_status != 5) { printf("!! short nick was not rejected, status=%g\n", login_status); return 3; }
    if (game_state != 7) { printf("!! left nick screen despite bad nick\n"); return 3; }
    printf("=== short nick rejected locally (frame %ld)\n", g_frame);

    /* --- 3. Ник с запрещёнными символами тоже отклоняется --- */
    fill_field(tap_nick, "bad nick!");
    tap_nick_go();
    run_frames(10);
    if (login_status != 5) { printf("!! invalid-char nick was not rejected, status=%g\n", login_status); return 3; }
    printf("=== invalid-char nick rejected locally (frame %ld)\n", g_frame);

    /* --- 4. Корректный ник сразу пускает в онлайн --- */
    fill_field(tap_nick, nick);
    tap_nick_go();
    if (!wait_state(5, 30)) { printf("!! did not enter online after nick, state=%g status=%g\n", game_state, login_status); return 3; }
    if (!wait_slot(80)) { printf("!! online entry failed: net status=%g slot=%g\n", net_status(), net_slot()); return 3; }
    printf("=== online with nick '%s': status=%g slot=%g count=%g (frame %ld)\n", nick, net_status(), net_slot(), net_count(), g_frame);

    /* --- 5. Чат: открыть -> написать -> отправить --- */
    do_tap(86, 174);
    run_frames(10);
    if (chat_open != 1) { printf("!! chat did not open\n"); return 3; }
    {
        uint32_t edge = g_pixels[2 + 400 * g_w];
        if (edge != 0xFF2F1E1Eu) {
            printf("!! chat is not fullscreen: edge pixel=0x%08X expected 0xFF2F1E1E\n", edge);
            return 3;
        }
        printf("=== chat covers the whole screen\n");
    }

    /* 5a. Удалённое из поля не возвращается: "Q" + Backspace + "qwerty"
     *     должно дать ровно "qwerty", а не "Qqwerty". */
    feed_text("Q");
    run_frames(5);
    keyboard_backspace();
    run_frames(5);
    if (chat_input && chat_input[0]) {
        printf("!! backspace left text in the field: '%s'\n", chat_input); return 3;
    }
    feed_text("qwerty");
    run_frames(5);
    if (!chat_input || strcmp(chat_input, "qwerty") != 0) {
        printf("!! deleted char came back: '%s' (expected 'qwerty')\n", chat_input ? chat_input : "(null)");
        return 3;
    }
    printf("=== field delete ok: '%s' (frame %ld)\n", chat_input, g_frame);
    keyboard_clear();
    run_frames(5);
    if (chat_input && chat_input[0]) {
        printf("!! clear left text in the field: '%s'\n", chat_input); return 3;
    }

    feed_text("hi from test");
    run_frames(10);
    do_tap(1188, 654); /* send */
    /* Чат специально опрашивается реже боевого состояния, поэтому здесь ждём
     * реальные часы, а не прогоняем мгновенно 30 кадров. */
    for (int i = 0; i < 100 && net_chat_count() < 1; i++) {
        run_frames(1);
        { struct timespec ts = { 0, 20 * 1000 * 1000 }; nanosleep(&ts, NULL); }
    }
    if (net_chat_count() < 1) { printf("!! chat message was not sent\n"); return 3; }
    {
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/rooms/main/chat.json", TEST_PORT);
        for (int i = 0; i < 24; i++) {
            char body[96];
            snprintf(body, sizeof(body), "{\"uid\":\"flood\",\"nick\":\"flood\",\"text\":\"m%d\"}", i);
            test_http_impl("POST", url, body, NULL, 0, NULL, NULL, NULL, 0);
        }
        for (int i = 0; i < 150; i++) {
            run_frames(1);
            { struct timespec ts = { 0, 20 * 1000 * 1000 }; nanosleep(&ts, NULL); }
            if (net_chat_count() >= 8) break;
        }
        run_frames(5);
        if (net_chat_count() > 18) {
            printf("!! old chat messages were not auto-deleted, count=%g\n", net_chat_count());
            return 3;
        }
        printf("=== chat auto-trim ok count=%g\n", net_chat_count());
    }
    do_tap(1124, 64); /* close: размер как у «Играть», справа сверху */
    run_frames(10);
    if (chat_open != 0) { printf("!! chat did not close\n"); return 3; }
    if (game_state != 5) { printf("!! close left online, state=%g\n", game_state); return 3; }
    printf("=== chat ok count=%g (frame %ld)\n", net_chat_count(), g_frame);

    /* --- 6. Вышедший игрок удаляется из списка --- */
    if (!wait_remotes(1, 200)) { printf("!! remote player never appeared, remotes=%g\n", arr_len(remotes)); return 3; }
    printf("=== remote player visible (remotes=%d)\n", remote_count());

    /* Класс соперника приходит полем cls: обычный по умолчанию, Азум после PATCH. */
    {
        int rslot = first_remote_slot(), seen = 0;
        if (arr_get(remotes, rslot * REMOTE_FIELDS + 10) != 0) {
            printf("!! remote class should start as Ordinary, got %g\n", arr_get(remotes, rslot * REMOTE_FIELDS + 10));
            return 3;
        }
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/rooms/main/players/%d.json", TEST_PORT, rslot);
        test_http_impl("PATCH", url, "{\"cls\":1}", NULL, 0, NULL, NULL, NULL, 0);
        for (int i = 0; i < 100 && !seen; i++) {
            run_frames(1);
            seen = arr_get(remotes, rslot * REMOTE_FIELDS + 10) == 1;
            { struct timespec ts = { 0, 20 * 1000 * 1000 }; nanosleep(&ts, NULL); }
        }
        if (!seen) { printf("!! remote class was not applied\n"); return 3; }
        printf("=== remote Azum class visible on slot %d\n", rslot);
    }

    /* У события намеренно нет короткого active=1. Клиент обязан заметить
     * изменившийся счётчик punch и всё равно показать анимацию с хитбоксом. */
    {
        int rslot = first_remote_slot(), seen = 0;
        /* Точка (710,360) лежит внутри будущего хитбокса, но уже за спрайтом. */
        uint32_t before_hitbox = g_pixels[710 + 360 * g_w];
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/rooms/main/players/%d.json", TEST_PORT, rslot);
        test_http_impl("PATCH", url,
                       "{\"px\":0.5,\"py\":0.5,\"pdx\":1,\"pdy\":0,\"punch\":101}",
                       NULL, 0, NULL, NULL, NULL, 0);
        for (int i = 0; i < 100 && !seen; i++) {
            run_frames(1);
            seen = arr_get(remote_punches, rslot * 6) == 1;
            { struct timespec ts = { 0, 20 * 1000 * 1000 }; nanosleep(&ts, NULL); }
        }
        if (!seen) { printf("!! remote punch event was lost\n"); return 3; }
        /* Чёрный слой с alpha=102 должен одинаково затемнить R, G и B.
         * Красный хитбокс эту проверку не пройдёт. */
        {
            uint32_t after = g_pixels[710 + 360 * g_w];
            for (int shift = 0; shift <= 16; shift += 8) {
                int old_c = (int)((before_hitbox >> shift) & 255u);
                int got = (int)((after >> shift) & 255u);
                int want = (old_c * 153 + 127) / 255;
                if (got < want - 2 || got > want + 2) {
                    printf("!! remote hitbox is not black: before=%08X after=%08X\n",
                           before_hitbox, after);
                    return 3;
                }
            }
        }
        printf("=== remote punch received; animation and black hitbox are visible\n");
    }

    {   /* убираем этого игрока с сервера — как будто он вышел из игры */
        int rslot = first_remote_slot();
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/rooms/main/players/%d.json", TEST_PORT, rslot);
        test_http_impl("DELETE", url, NULL, NULL, 0, NULL, NULL, NULL, 0);
        printf("=== remote slot %d deleted on the server\n", rslot);
    }
    if (!wait_remotes(0, 300)) { printf("!! player who left was not removed, remotes=%d\n", remote_count()); return 3; }
    printf("=== player who left removed from the list (frame %ld)\n", g_frame);

    /* --- 7. Немного боя в онлайне (движение + удар) --- */
    do_tap(130, (float)(g_h - 150)); run_frames(5);
    do_tap((float)(g_w - 140), (float)(g_h - 150)); run_frames(30);
    tap_back();
    if (!wait_state(0, 60)) { printf("!! did not return to lobby from online\n"); return 3; }
    printf("=== left online (frame %ld)\n", g_frame);

    /* --- 8. Ник сохранён в auth.dat --- */
    {
        FILE *f = fopen("auth.dat", "r");
        char saved[64] = "";
        if (!f) { printf("!! auth.dat was not written\n"); return 3; }
        if (fscanf(f, "%63s", saved) != 1) saved[0] = 0;
        fclose(f);
        if (strcmp(saved, nick) != 0) { printf("!! auth.dat has '%s', expected '%s'\n", saved, nick); return 3; }
        printf("=== nick persisted to auth.dat\n");
    }

    /* --- 9. Соло-бой работает --- */
    tap_play(); wait_state(2, 30);
    tap_solo(); wait_state(1, 30);
    run_frames(40);
    printf("=== solo battle ok (frame %ld)\n", g_frame);

    /* Бот бьёт быстро, а перезарядка удара каждый раз случайная и не короче
     * enemy_cooldown_min. */
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

    /* --- 9b. Классы: вкладка, Азум за 50 кубков, сохранение, одно возрождение --- */
    tap_classes();
    if (!wait_state(8, 30)) { printf("!! classes tab did not open, state=%g\n", game_state); return 3; }
    tap_class_azum();
    run_frames(5);
    if (player_class != 0 || azum_owned != 0) {
        printf("!! Azum must stay locked without cups, class=%g owned=%g\n", player_class, azum_owned);
        return 3;
    }
    cups = 50;
    tap_class_azum();
    run_frames(5);
    if (player_class != 1 || azum_owned != 1 || cups != 0) {
        printf("!! Azum buy failed: class=%g owned=%g cups=%g\n", player_class, azum_owned, cups);
        return 3;
    }
    {
        /* Формат прогресса: cups cls azum santa candies (5 чисел). */
        FILE *f = fopen("progress.dat", "r");
        int pc=0, cl=0, az=0, sn=0, cd=0;
        if (!f) { printf("!! progress.dat was not written after buying Azum\n"); return 3; }
        if (fscanf(f, "%d %d %d %d %d", &pc, &cl, &az, &sn, &cd) != 5) { fclose(f); printf("!! progress.dat is unreadable\n"); return 3; }
        fclose(f);
        if (pc != 0 || cl != 1 || az != 1 || sn != 0) { printf("!! progress.dat has %d %d %d %d %d, expected 0 1 1 0 0\n", pc, cl, az, sn, cd); return 3; }
    }
    tap_class_ordinary();
    run_frames(5);
    if (player_class != 0) { printf("!! Ordinary class was not selected, class=%g\n", player_class); return 3; }
    tap_class_azum();
    run_frames(4);
    tap_back();
    if (!wait_state(0, 30)) { printf("!! did not return from classes\n"); return 3; }
    if (player_class != 1) { printf("!! class was lost after leaving the tab\n"); return 3; }
    printf("=== classes tab: Azum bought and selected (frame %ld)\n", g_frame);

    tap_play(); wait_state(2, 30);
    tap_solo(); wait_state(1, 30);
    {
        double *pl = (double *)player;
        pl[4] = 0;
        run_frames(4);
        if (azum_revived != 1 || pl[4] < 9) {
            printf("!! Azum did not revive: revived=%g hp=%g\n", azum_revived, pl[4]);
            return 3;
        }
        printf("=== Azum revived once, hp=%g\n", pl[4]);
        pl[4] = 0;
        run_frames(4);
        if (finished != 2) { printf("!! Azum revived a second time, finished=%g hp=%g\n", finished, pl[4]); return 3; }
        if (cups_awarded != 0 || cups != 0) { printf("!! defeat awarded cups: awarded=%g cups=%g\n", cups_awarded, cups); return 3; }
        printf("=== second death stays dead (Azum revive is once per match)\n");
    }
    do_tap((float)(g_w / 2), (float)(g_h / 2));
    wait_state(0, 40);
    tap_classes(); wait_state(8, 30);
    tap_class_ordinary();
    run_frames(3);
    tap_levels_btn();
    if (!wait_state(9, 30)) { printf("!! levels screen did not open, state=%g\n", game_state); return 3; }
    tap_level_row(3);
    run_frames(4);
    if (player_level != 3) { printf("!! level 3 was not selected, level=%g\n", player_level); return 3; }
    tap_level_row(1);
    run_frames(4);
    if (player_level != 1) { printf("!! level 1 was not selected, level=%g\n", player_level); return 3; }
    printf("=== levels: absorb 10%% at level 1, selectable (frame %ld)\n", g_frame);
    tap_back(); wait_state(8, 30);
    tap_back(); wait_state(0, 30);

    /* --- 9c. Дед Мороз: покупка за 100 ЛЕДЕНЦОВ, посох (+2 урона, заморозка 1с),
     * суператака-подарок (+3 урона, заморозка 3с), и ящик за 30 кубков. --- */
    tap_classes(); wait_state(8, 30);
    /* Без леденцов Дед Мороз должен остаться закрытым. */
    candies = 0;
    tap_class_santa();
    run_frames(5);
    if (player_class != 0 || santa_owned != 0) {
        printf("!! Santa must stay locked without candies, class=%g owned=%g\n", player_class, santa_owned);
        return 3;
    }
    candies = 100;
    tap_class_santa();
    run_frames(5);
    if (player_class != 2 || santa_owned != 1 || candies != 0) {
        printf("!! Santa buy failed: class=%g owned=%g candies=%g\n", player_class, santa_owned, candies);
        return 3;
    }
    {
        FILE *f = fopen("progress.dat", "r");
        int pc=0, cl=0, az=0, sn=0, cd=0;
        if (!f) { printf("!! progress.dat was not written after buying Santa\n"); return 3; }
        if (fscanf(f, "%d %d %d %d %d", &pc, &cl, &az, &sn, &cd) != 5) { fclose(f); printf("!! progress.dat is unreadable\n"); return 3; }
        fclose(f);
        if (pc != 0 || cl != 2 || az != 1 || sn != 1 || cd != 0) { printf("!! progress.dat has %d %d %d %d %d, expected 0 2 1 1 0\n", pc, cl, az, sn, cd); return 3; }
    }
    printf("=== shop: Santa bought for candies and selected (frame %ld)\n", g_frame);

    /* Ящик: покупается за 30 кубков и всегда что-то выбивает (леденцы/кубки/класс).
     * Азум уже куплен, поэтому его дроп превращается в леденцы; проверяем, что
     * кубки списались и награда появилась, а результат показан сообщением. */
    cups = 30; candies = 0;
    tap_box();
    run_frames(6);
    if (!box_msg || !box_msg[0]) { printf("!! box result message missing\n"); return 3; }
    if (cups > 40) { printf("!! box refunded too many cups: cups=%g\n", cups); return 3; }
    if (!(candies > 0 || cups >= 20)) { printf("!! box gave nothing: cups=%g candies=%g\n", cups, candies); return 3; }
    printf("=== box opened (cost 30): '%s' -> cups=%g candies=%g\n", box_msg, cups, candies);
    /* Ещё несколько ящиков: каждая награда обязана дать сообщение и не увести
     * валюты в минус — это покрывает ветки «леденцы» и «класс уже куплен». */
    {
        int opens = 0;
        for (int i = 0; i < 8; i++) {
            cups = 30;
            tap_box();
            run_frames(3);
            if (!box_msg || !box_msg[0]) { printf("!! box %d gave no message\n", i); return 3; }
            if (cups < 0 || candies < 0) { printf("!! box %d drove currency negative: cups=%g candies=%g\n", i, cups, candies); return 3; }
            opens++;
        }
        printf("=== box opened %d more times without errors (last: '%s')\n", opens, box_msg);
    }
    printf("=== shop: box + Santa buy tested (frame %ld)\n", g_frame);
    /* Выйти с экрана классов обратно в лобби, откуда уже начинаем бой. */
    tap_back();
    if (!wait_state(0, 30)) { printf("!! did not return to lobby after buying Santa\\n"); return 3; }

    tap_play(); wait_state(2, 30);
    tap_solo(); wait_state(1, 30);
    run_frames(20);
    {
        double *e = (double *)enemy, *pl = (double *)player;
        /* Ставим врага прямо перед игроком и обездвиживаем его AI, чтобы
         * попадание посоха и подарка было детерминированным. */
        pl[0] = g_w / 2; pl[1] = g_h / 2; pl[3] = 0; pl[4] = 10;
        e[0] = pl[0] + 60; e[1] = pl[1]; e[5] = 3.14159265; e[6] = 0; e[8] = 99; e[11] = 99; e[23] = 0;
        /* Посох: обычный удар бьёт на 2 (вместо 1) и морозит на 1 секунду. */
        do_tap((float)(g_w - 140), (float)(g_h - 150));
        run_frames(14);
        if (e[3] > 8.5 || e[3] < 7.5) { printf("!! staff dealt wrong damage: enemy hp=%g (expected 8)\\n", e[3]); return 3; }
        if (e[23] <= 0.3) { printf("!! staff did not freeze: freeze=%g\\n", e[23]); return 3; }
        if (e[24] < 0.3 || e[24] > 0.4) { printf("!! staff freeze factor wrong: %g\\n", e[24]); return 3; }
        printf("=== staff: enemy hp=%g freeze=%g slow=%g\\n", e[3], e[23], e[24]);
        /* Подарок: вторая кнопка кидает бомбу, взрыв бьёт на 3 и морозит на 3с. */
        e[3] = 10; e[23] = 0; e[0] = pl[0] + 80; e[1] = pl[1];
        do_tap((float)(g_w - 140), (float)(g_h - 300));
        run_frames(30);
        if (e[3] > 7.5 || e[3] < 6.5) { printf("!! super dealt wrong damage: enemy hp=%g (expected 7)\\n", e[3]); return 3; }
        if (e[23] <= 2.0) { printf("!! super freeze too short: freeze=%g\\n", e[23]); return 3; }
        if (e[24] < 0.05 || e[24] > 0.2) { printf("!! super freeze factor wrong: %g\\n", e[24]); return 3; }
        printf("=== super (gift): enemy hp=%g freeze=%g slow=%g\\n", e[3], e[23], e[24]);
    }
    /* Выход из боя (кнопкой «Назад») возвращает в лобби. */
    tap_back();
    if (!wait_state(0, 40)) { printf("!! did not return to lobby from Santa solo\\n"); return 3; }
    /* Возвращаем Обычный класс, чтобы дальше тест шёл как раньше. */
    tap_classes(); wait_state(8, 30);
    tap_class_ordinary();
    run_frames(3);
    tap_back(); wait_state(0, 30);

    /* --- 10. Повторный вход в онлайн: ник уже сохранён, экран ника не нужен --- */
    tap_play(); wait_state(2, 30);
    tap_online();
    if (!wait_state(5, 30)) { printf("!! saved nick did not skip the nick screen, state=%g\n", game_state); return 3; }
    if (!wait_slot(80)) { printf("!! online re-entry failed\n"); return 3; }
    printf("=== online straight in with saved nick: slot=%g (frame %ld)\n", net_slot(), g_frame);
    tap_back(); wait_state(0, 60);

    printf("=== console tail:\n");
    int n = console_count();
    for (int i = n > 30 ? n - 30 : 0; i < n; i++) printf("  [%d] %s\n", console_type(i), console_line(i));
    /* reset() освобождает игровые объекты, чтобы ASAN не ругался на утечку
     * глобалов (они живут всё время работы игры). */
    script_active = 1;
    int rok = ds_call_protected(protected_reset, NULL, "reset");
    printf("=== reset ok=%d err='%s' player=%p enemy=%p punch=%p\n", rok, ds_runtime_error_message(), player, enemy, punch);
    printf("=== DONE ok\n");
    return 0;
}
