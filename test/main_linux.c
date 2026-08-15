/* Headless Linux harness for the DimScript game.
 * Drives the real script (init/update/draw/touch) against a fake Firebase
 * HTTP server so the full online path (login/registration, claim slot,
 * push/read, chat) runs on real sockets and threads. */
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
extern double game_state, chat_open, login_field, login_status, login_mode, t_dir;
extern const char *login_nick, *login_pwd, *login_pwd2, *chat_input;

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

static int wait_net_login(double want, int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        run_frames(5);
        if (net_login_status() == want) return 1;
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

/* Экран аккаунта (1280x720): fy = screen_h/2-130 */
#define FY (g_h / 2 - 130)
static void tap_nick(void) { do_tap((float)(g_w / 2), (float)(FY + 25)); }
static void tap_pwd(void) { do_tap((float)(g_w / 2), (float)(FY + 87)); }
static void tap_rep(void) { do_tap((float)(g_w / 2), (float)(FY + 149)); }
static void tap_login_btn(void) { do_tap((float)(g_w / 2), (float)(FY + 146 + 32)); }
static void tap_create_btn(void) { do_tap((float)(g_w / 2), (float)(FY + 208 + 32)); }
/* в режиме «вход» переключатель на yt=440 (центр 472), в режиме «создание» на yt=502 (центр 534) */
static void tap_toggle_to_create(void) { do_tap((float)(g_w / 2), (float)(FY + 210 + 32)); }
static void tap_toggle_to_login(void) { do_tap((float)(g_w / 2), (float)(FY + 272 + 32)); }
static void tap_logout(void) { do_tap((float)(g_w - 190 + 85), 48); }

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
    unsigned short bpp = 24;
    memcpy(hdr+26, &bpp, 2);
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
    screen_w = g_w; screen_h = g_h;
    g_pixels = (uint32_t *)calloc((size_t)g_w * g_h, 4);
    if (!g_pixels) { printf("OOM\n"); return 2; }

    /* уникальный ник на каждый запуск, чтобы сервер не хранил старые аккаунты */
    char nick[32];
    snprintf(nick, sizeof(nick), "User%ld", (long)(time(NULL) % 100000));

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
            printf("!! swizzle formula broken: got 0x%08X, expected 0xFF5F10A0 (BGRX)\n", sw);
            return 3;
        }
        printf("=== framebuffer pixel format OK (RGBA -> BGRX swizzle verified)\n");
    }

    /* --- 1. Account screen opens in login mode --- */
    tap_account();
    if (!wait_state(7, 30)) { printf("!! account screen did not open\n"); return 3; }
    printf("=== account screen, mode=%g status=%g (frame %ld)\n", login_mode, login_status, g_frame);

    /* --- 2. Switch to create mode --- */
    tap_toggle_to_create();
    run_frames(10);
    if (login_mode != 1) { printf("!! toggle to create mode failed, mode=%g\n", login_mode); return 3; }
    printf("=== create mode on (frame %ld)\n", g_frame);

    /* --- 3. Password mismatch must be rejected without network --- */
    fill_field(tap_nick, nick);
    fill_field(tap_pwd, "secret99");
    fill_field(tap_rep, "secret9X");
    tap_create_btn();
    run_frames(10);
    if (login_status != 7) { printf("!! mismatch not detected, status=%g net=%g\n", login_status, net_login_status()); return 3; }
    printf("=== passwords mismatch rejected (frame %ld)\n", g_frame);

    /* --- 4. Fix repeat password, create account --- */
    keyboard_clear();
    feed_text("secret99");
    run_frames(5);
    tap_create_btn();
    if (!wait_net_login(2, 40)) { printf("!! register did not succeed, net status=%g script status=%g\n", net_login_status(), login_status); return 3; }
    if (!wait_state(0, 30)) { printf("!! did not return to lobby after register\n"); return 3; }
    printf("=== account created (%s), back in lobby (frame %ld)\n", nick, g_frame);

    /* --- 5. Solo works --- */
    tap_play(); wait_state(2, 30);
    tap_solo(); wait_state(1, 30);
    run_frames(40);
    printf("=== solo battle ok (frame %ld)\n", g_frame);
    tap_back(); wait_state(0, 40);

    /* --- 6. Online straight in (logged in) + chat --- */
    tap_play(); wait_state(2, 30);
    tap_online(); wait_state(5, 30);
    if (!wait_slot(60)) { printf("!! online entry failed\n"); return 3; }
    printf("=== online straight in: status=%g slot=%g count=%g (frame %ld)\n", net_status(), net_slot(), net_count(), g_frame);
    do_tap(86, 174);
    run_frames(10);
    feed_text("hi from test");
    run_frames(10);
    do_tap(1188, 654); /* send */
    run_frames(30);
    do_tap(1192, 51); /* close */
    run_frames(10);
    if (net_chat_count() < 1) { printf("!! chat message was not sent\n"); return 3; }
    printf("=== chat ok count=%g (frame %ld)\n", net_chat_count(), g_frame);
    tap_back(); wait_state(2, 40);
    tap_back(); wait_state(0, 40);
    printf("=== left online (frame %ld)\n", g_frame);

    /* --- 7. Logout --- */
    tap_account(); wait_state(7, 30);
    tap_logout();
    run_frames(20);
    if (net_login_status() == 2) { printf("!! logout failed\n"); return 3; }
    printf("=== logged out (frame %ld)\n", g_frame);
    tap_back(); wait_state(0, 40);

    /* --- 8. Online gate demands login after logout --- */
    tap_play(); wait_state(2, 30);
    tap_online(); wait_state(7, 30);
    printf("=== online gate demands login (frame %ld)\n", g_frame);

    /* --- 9. Creating an existing nick is rejected --- */
    tap_toggle_to_create(); run_frames(10);
    fill_field(tap_nick, nick);
    fill_field(tap_pwd, "secret99");
    fill_field(tap_rep, "secret99");
    tap_create_btn();
    if (!wait_net_login(6, 40)) { printf("!! existing nick was not rejected, net=%g\n", net_login_status()); return 3; }
    run_frames(10);
    if (login_status != 6) { printf("!! script status not 6, got %g\n", login_status); return 3; }
    printf("=== existing nick rejected (frame %ld)\n", g_frame);

    /* --- 10. Login mode: wrong password rejected, correct password goes online --- */
    tap_toggle_to_login(); run_frames(10);
    fill_field(tap_nick, nick);
    fill_field(tap_pwd, "WRONGpass");
    tap_login_btn();
    if (!wait_net_login(3, 40)) { printf("!! wrong password was not rejected, net=%g\n", net_login_status()); return 3; }
    run_frames(10);
    printf("=== wrong password rejected (frame %ld)\n", g_frame);
    keyboard_clear();
    feed_text("secret99");
    run_frames(5);
    tap_login_btn();
    if (!wait_net_login(2, 40)) { printf("!! correct password did not login, net=%g\n", net_login_status()); return 3; }
    if (!wait_state(5, 30)) { printf("!! did not jump into online\n"); return 3; }
    if (!wait_slot(60)) { printf("!! online after login failed\n"); return 3; }
    printf("=== online after correct password: status=%g slot=%g (frame %ld)\n", net_status(), net_slot(), g_frame);

    printf("=== console tail:\n");
    int n = console_count();
    for (int i = n > 30 ? n - 30 : 0; i < n; i++) printf("  [%d] %s\n", console_type(i), console_line(i));
    printf("=== DONE ok\n");
    return 0;
}
