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
extern double game_state, chat_open, login_field, login_status, t_dir, t_target, t_fade;
extern const char *login_nick, *login_pwd, *chat_input;

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
    fprintf(stderr, "HTTP %s %s -> %d etag=[%s] body=[%.80s]\n", method, path, code, etag && etag[0] ? etag : "-", out && out[0] ? out : "");
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

static int start_script(void) {
    ds_clear_runtime_error(); ds_clear_script_restart(); ds_string_pool_reset();
    if (!ds_call_protected(protected_init, NULL, "init")) return 0;
    script_active = 1;
    return 1;
}

static void feed_text(const char *s) { keyboard_type(s); }

static void do_tap(float x, float y) {
    TouchCall c;
    c.x = x; c.y = y; c.action = 0; c.id = 1;
    ds_call_protected(protected_touch, &c, "touch");
    c.action = 1;
    ds_call_protected(protected_touch, &c, "touch");
}

static long g_frame = 0;

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

/* Login screen geometry (same math as draw_login/touch_login). */
static double lg_fy(void) { return (double)screen_h / 2 - 95; }
static void tap_nick_field(void) { do_tap((float)(screen_w / 2), (float)(lg_fy() + 29)); }
static void tap_pwd_field(void) { do_tap((float)(screen_w / 2), (float)(lg_fy() + 101)); }
static void tap_login_go(void) { do_tap((float)(screen_w / 2), (float)(lg_fy() + 156 + 32)); }
static void tap_logout(void) { do_tap((float)(screen_w / 2), (float)(lg_fy() + 156 + 80 + 32)); }

static int wait_login_status(double want, int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        run_frames(5);
        if (net_login_status() == want) return 1;
    }
    return 0;
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

static int wait_slot(int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        run_frames(5);
        if (net_slot() >= 0 && net_status() == NET_PLAYING) return 1;
    }
    return 0;
}

static void login_as(const char *nick, const char *pwd) {
    tap_nick_field();
    run_frames(5);
    feed_text(nick);
    run_frames(5);
    tap_pwd_field();
    run_frames(5);
    feed_text(pwd);
    run_frames(5);
    tap_login_go();
}

int main(void) {
    screen_w = g_w; screen_h = g_h;
    g_pixels = (uint32_t *)calloc((size_t)g_w * g_h, 4);
    if (!g_pixels) { printf("OOM\n"); return 2; }

    if (!start_script()) { printf("init failed: %s\n", ds_runtime_error_message()); return 2; }
    printf("=== init ok (frame %ld)\n", g_frame);
    run_frames(10);

    /* --- 1. Lobby -> Account -> login screen, register new nick --- */
    do_tap((float)(screen_w - 280) / 2 + 140, (float)screen_h / 2 + 152); /* Account */
    wait_state(7, 30);
    printf("=== account screen open (state=%g) login_status=%g (frame %ld)\n", game_state, net_login_status(), g_frame);
    save_bmp("/home/user/Gig1.0/test/shot_login.bmp");

    login_as("TestUser", "pass1234");
    printf("  [probe] after login_as: nick=[%s] pwd=[%s] ds_login_status=%g c_login_status=%g t_dir=%g state=%g (frame %ld)\n",
           login_nick, login_pwd, login_status, net_login_status(), t_dir, game_state, g_frame);
    if (!wait_login_status(2, 40)) { printf("!! login (register) did not succeed, status=%g\n", net_login_status()); return 3; }
    printf("  [probe] c-status=2 reached, ds_login_status=%g t_dir=%g state=%g (frame %ld)\n", login_status, t_dir, game_state, g_frame);
    wait_state(0, 30); /* после входа уходим в лобби */
    printf("=== registered+logged in as TestUser (state=%g, frame %ld)\n", game_state, g_frame);

    /* --- 2. Solo must work without any login gate --- */
    do_tap((float)(screen_w - 280) / 2 + 140, (float)screen_h / 2 - 8); /* Play */
    wait_state(2, 30);
    do_tap((float)(screen_w - 280) / 2 + 140, (float)screen_h / 2 - 8); /* Solo */
    wait_state(1, 30);
    run_frames(40);
    printf("=== solo battle ok (frame %ld)\n", g_frame);
    do_tap((float)(screen_w - 280) / 2 + 140, 32 + 32); /* Back */
    wait_state(0, 30);

    /* --- 3. Online while logged in: goes straight in --- */
    do_tap((float)(screen_w - 280) / 2 + 140, (float)screen_h / 2 - 8); /* Play */
    wait_state(2, 30);
    do_tap((float)(screen_w - 280) / 2 + 140, (float)screen_h / 2 + 72); /* Online */
    wait_state(5, 30);
    if (!wait_slot(60)) { printf("!! online entry failed\n"); return 3; }
    printf("=== online straight in: status=%g slot=%g count=%g (frame %ld)\n", net_status(), net_slot(), net_count(), g_frame);

    /* chat: open, type, send, close */
    do_tap(16 + 70, 150 + 24);
    run_frames(10);
    printf("  [probe] after open tap: chat_open=%g state=%g\n", chat_open, game_state);
    feed_text("hi from test");
    run_frames(10);
    printf("  [probe] after feed: chat_open=%g chat_input=[%s]\n", chat_open, chat_input);
    do_tap(1188, 654); /* send */
    run_frames(30);
    printf("  [probe] after send tap: chat_open=%g chat_input=[%s] state=%g\n", chat_open, chat_input, game_state);
    if (net_chat_count() < 1) { printf("!! chat message was not sent\n"); return 3; }
    do_tap(1192, 51); /* close */
    run_frames(10);
    save_bmp("/home/user/Gig1.0/test/shot_chat.bmp");
    printf("=== chat ok: count=%g (frame %ld)\n", net_chat_count(), g_frame);

    /* leave online */
    do_tap((float)(screen_w - 280) / 2 + 140, 32 + 32);
    wait_state(2, 40);
    printf("=== left online status=%g (frame %ld)\n", net_status(), g_frame);

    /* --- 4. Logout, then online must demand login --- */
    do_tap((float)(screen_w - 280) / 2 + 140, 32 + 32); /* back to lobby */
    wait_state(0, 40);
    do_tap((float)(screen_w - 280) / 2 + 140, (float)screen_h / 2 + 152); /* Account */
    wait_state(7, 30);
    tap_logout();
    wait_state(0, 40);
    printf("=== logged out, login_status=%g (frame %ld)\n", net_login_status(), g_frame);

    do_tap((float)(screen_w - 280) / 2 + 140, (float)screen_h / 2 - 8); /* Play */
    wait_state(2, 30);
    do_tap((float)(screen_w - 280) / 2 + 140, (float)screen_h / 2 + 72); /* Online -> gate */
    wait_state(7, 30);
    printf("=== online gate demands login after logout (frame %ld)\n", g_frame);

    /* --- 5. Wrong password must NOT let in --- */
    login_as("TestUser", "WRONGpass");
    wait_login_status(3, 40);
    printf("=== wrong password rejected (frame %ld)\n", g_frame);

    /* fix the password: clear field, type the right one, log in -> straight online */
    tap_pwd_field();
    run_frames(5);
    keyboard_clear();
    feed_text("pass1234");
    run_frames(5);
    tap_login_go();
    wait_login_status(2, 40);
    wait_state(5, 30);
    if (!wait_slot(60)) { printf("!! online after relogin failed\n"); return 3; }
    printf("=== online after correct password: status=%g slot=%g (frame %ld)\n", net_status(), net_slot(), g_frame);

    printf("=== console tail:\n");
    int n = console_count();
    for (int i = n > 30 ? n - 30 : 0; i < n; i++) printf("  [%d] %s\n", console_type(i), console_line(i));
    printf("=== DONE ok\n");
    return 0;
}
