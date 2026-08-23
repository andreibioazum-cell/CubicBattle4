/* main_web.c — браузерная версия ZeroHabit (Emscripten + canvas 2D).
 *
 * Игра рисует кадр в обычный буфер пикселей (R,G,B,A — как на Android), а
 * здесь он просто кладётся в <canvas> через putImageData: ни WebGL, ни SDL
 * не нужны. Ввод: мышь и тач переводятся в те же touch(x,y,action,id), что и
 * на телефоне, плюс WASD/стрелки двигают джойстик, как в Windows-сборке.
 *
 * Прогресс (progress.dat) лежит в IDBFS, примонтированной в /persist, и
 * периодически сбрасывается в IndexedDB, чтобы сила воли не пропадала между
 * заходами на страницу.
 */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "net.h"

#define MOUSE_ID 80
#define KEY_JOY_ID 60

static uint32_t *g_pixels = NULL;
static int g_bw = 0, g_bh = 0;
static int g_mouse_down = 0;
static int g_kb_joy = 0;
static int g_key_left = 0, g_key_right = 0, g_key_up = 0, g_key_down = 0;
static int script_active = 0;
static double g_prev_ms = 0.0;
static double g_sync_at = 0.0;

static void protected_init(void *u) { init((AAssetManager *)u); }
static void protected_reset(void *u) { (void)u; reset(); }
static void protected_update(void *u) { (void)u; update(); }
static void protected_draw(void *u) { draw((Buffer *)u); }

/* Кадр отдаётся в canvas одним putImageData. ImageData кэшируется на модуле,
 * пересоздаётся только при смене размера. */
EM_JS(void, ds_blit, (int ptr, int w, int h), {
    var canvas = document.getElementById('canvas');
    if (!canvas) return;
    if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
        Module.dsImage = null;
    }
    var ctx = Module.dsCtx;
    if (!ctx) { ctx = canvas.getContext('2d'); Module.dsCtx = ctx; }
    if (!ctx) return;
    if (!Module.dsImage) Module.dsImage = ctx.createImageData(w, h);
    Module.dsImage.data.set(HEAPU8.subarray(ptr, ptr + w * h * 4));
    ctx.putImageData(Module.dsImage, 0, 0);
});

/* Размер канваса в CSS-пикселях: игра рисуется ровно в него, без растяжения. */
EM_JS(int, ds_css_width, (void), {
    var c = document.getElementById('canvas');
    return c ? Math.max(320, Math.round(c.clientWidth)) : 1280;
});
EM_JS(int, ds_css_height, (void), {
    var c = document.getElementById('canvas');
    return c ? Math.max(240, Math.round(c.clientHeight)) : 720;
});

EM_JS(void, ds_fs_sync, (void), {
    if (Module.dsFsBusy) return;
    Module.dsFsBusy = 1;
    FS.syncfs(false, function () { Module.dsFsBusy = 0; });
});

static void mark_failed(const char *hook) {
    ds_console_log(1, "script error in '%s': %s", hook, ds_runtime_error_message());
    script_active = 0;
}

static int start_script(int reset_state) {
    ds_clear_runtime_error(); ds_clear_script_restart(); ds_string_pool_reset();
    if (reset_state && !ds_call_protected(protected_reset, NULL, "reset")) {
        mark_failed("reset"); return 0;
    }
    ds_clear_runtime_error();
    if (!ds_call_protected(protected_init, NULL, "init")) { mark_failed("init"); return 0; }
    script_active = 1;
    return 1;
}

static void ensure_buffer(int cw, int ch) {
    if (cw <= 0 || ch <= 0) return;
    if (cw == g_bw && ch == g_bh && g_pixels) return;
    uint32_t *np = (uint32_t *)realloc(g_pixels, (size_t)cw * (size_t)ch * 4u);
    if (!np) return;
    g_pixels = np;
    g_bw = cw;
    g_bh = ch;
    memset(g_pixels, 0, (size_t)cw * (size_t)ch * 4u);
    screen_w = cw;
    screen_h = ch;
}

/* Координаты события приходят в CSS-пикселях канваса, а буфер кадра может
 * отличаться масштабом — приводим к пикселям буфера. */
static void event_xy(double tx, double ty, float *ox, float *oy) {
    double cw = (double)ds_css_width(), ch = (double)ds_css_height();
    double sx = cw > 0 ? (double)g_bw / cw : 1.0;
    double sy = ch > 0 ? (double)g_bh / ch : 1.0;
    *ox = (float)(tx * sx);
    *oy = (float)(ty * sy);
}

static EM_BOOL on_mouse(int type, const EmscriptenMouseEvent *e, void *u) {
    (void)u;
    float x, y;
    event_xy(e->targetX, e->targetY, &x, &y);
    mouse_x = x; mouse_y = y; mouse_in = 1.0;
    if (type == EMSCRIPTEN_EVENT_MOUSEDOWN) { g_mouse_down = 1; touch(x, y, 0, MOUSE_ID); }
    else if (type == EMSCRIPTEN_EVENT_MOUSEUP) { g_mouse_down = 0; touch(x, y, 1, MOUSE_ID); }
    else if (type == EMSCRIPTEN_EVENT_MOUSEMOVE && g_mouse_down) { touch(x, y, 2, MOUSE_ID); }
    return EM_TRUE;
}

static EM_BOOL on_touch(int type, const EmscriptenTouchEvent *e, void *u) {
    (void)u;
    int action = 2;
    mouse_in = 0.0; /* пальцем наводиться нельзя — эффект наведения выключаем */
    if (type == EMSCRIPTEN_EVENT_TOUCHSTART) action = 0;
    else if (type == EMSCRIPTEN_EVENT_TOUCHEND) action = 1;
    else if (type == EMSCRIPTEN_EVENT_TOUCHCANCEL) action = 3;
    for (int i = 0; i < e->numTouches; i++) {
        const EmscriptenTouchPoint *p = &e->touches[i];
        if (!p->isChanged && action != 2) continue;
        float x, y;
        event_xy(p->targetX, p->targetY, &x, &y);
        touch(x, y, action, (int)(p->identifier & 0x3F));
    }
    return EM_TRUE;
}

static int key_match(const char *code, const char *a, const char *b) {
    return strcmp(code, a) == 0 || strcmp(code, b) == 0;
}

static EM_BOOL on_key(int type, const EmscriptenKeyboardEvent *e, void *u) {
    (void)u;
    int down = (type == EMSCRIPTEN_EVENT_KEYDOWN);
    if (key_match(e->code, "KeyA", "ArrowLeft")) g_key_left = down;
    else if (key_match(e->code, "KeyD", "ArrowRight")) g_key_right = down;
    else if (key_match(e->code, "KeyW", "ArrowUp")) g_key_up = down;
    else if (key_match(e->code, "KeyS", "ArrowDown")) g_key_down = down;
    else if (down && (strcmp(e->code, "Space") == 0 || strcmp(e->code, "Enter") == 0)) {
        /* Пробел — кнопка «Держись» / подтверждение на панелях. */
        touch((float)screen_w - 140.0f, (float)screen_h - 150.0f, 0, KEY_JOY_ID);
        touch((float)screen_w - 140.0f, (float)screen_h - 150.0f, 1, KEY_JOY_ID);
    } else return EM_FALSE;
    return EM_TRUE;
}

static void apply_keyboard_joystick(void) {
    float dx = (float)(g_key_right - g_key_left);
    float dy = (float)(g_key_down - g_key_up);
    if (dx != 0.0f || dy != 0.0f) {
        float m = sqrtf(dx * dx + dy * dy);
        if (m > 1.0f) { dx /= m; dy /= m; }
        joy.dx = dx; joy.dy = dy;
        joy.ox = dx * joy.r; joy.oy = dy * joy.r;
        g_kb_joy = 1;
    } else if (g_kb_joy) {
        g_kb_joy = 0;
        joy.dx = 0; joy.dy = 0; joy.ox = 0; joy.oy = 0;
    }
}

static void frame(void) {
    double now = emscripten_get_now();
    double d = g_prev_ms > 0.0 ? (now - g_prev_ms) / 1000.0 : 0.0;
    g_prev_ms = now;
    if (d < 0.0) d = 0.0;
    if (d > 0.1) d = 0.1;
    dt = d;

    ensure_buffer(ds_css_width(), ds_css_height());
    if (!g_pixels) return;
    apply_keyboard_joystick();

    if (script_active) {
        if (!ds_call_protected(protected_update, NULL, "update")) mark_failed("update");
    }
    Buffer fb = { g_pixels, g_bw, g_bh, g_bw };
    if (ds_graphics_begin_frame(&fb)) {
        int draw_failed = 0;
        if (script_active) {
            if (!ds_call_protected(protected_draw, &fb, "draw")) { mark_failed("draw"); draw_failed = 1; }
        }
        if (!script_active) {
            if (draw_failed || ds_script_has_error()) ds_graphics_error_screen(ds_runtime_error_message());
            ds_graphics_cancel_frame();
        } else {
            ds_graphics_end_frame();
        }
    }
    ds_blit((int)(intptr_t)g_pixels, g_bw, g_bh);

    /* Раз в две секунды скидываем прогресс из памяти в IndexedDB. */
    if (now >= g_sync_at) { g_sync_at = now + 2000.0; ds_fs_sync(); }
}

/* Монтируем IDBFS и ждём, пока браузер поднимет уже сохранённый прогресс:
 * до этого запускать скрипт нельзя, иначе он прочитает пустой progress.dat. */
EM_JS(void, ds_fs_init, (void), {
    try {
        FS.mkdir('/persist');
        FS.mount(IDBFS, {}, '/persist');
    } catch (e) { }
    Module.dsFsReady = 0;
    FS.syncfs(true, function () { Module.dsFsReady = 1; });
});
EM_JS(int, ds_fs_ready, (void), { return Module.dsFsReady ? 1 : 0; });

static void boot(void) {
    if (!ds_fs_ready()) return;
    emscripten_cancel_main_loop();
    net_set_data_path("/persist");
    ds_graphics_init(NULL);
    ds_audio_init();
    ensure_buffer(ds_css_width(), ds_css_height());
    start_script(0);
    emscripten_set_main_loop(frame, 0, 0);
}

int main(void) {
    ds_fs_init();
    emscripten_set_mousedown_callback("#canvas", NULL, EM_TRUE, on_mouse);
    emscripten_set_mouseup_callback("#canvas", NULL, EM_TRUE, on_mouse);
    emscripten_set_mousemove_callback("#canvas", NULL, EM_TRUE, on_mouse);
    emscripten_set_touchstart_callback("#canvas", NULL, EM_TRUE, on_touch);
    emscripten_set_touchend_callback("#canvas", NULL, EM_TRUE, on_touch);
    emscripten_set_touchmove_callback("#canvas", NULL, EM_TRUE, on_touch);
    emscripten_set_touchcancel_callback("#canvas", NULL, EM_TRUE, on_touch);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, on_key);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, on_key);
    emscripten_set_main_loop(boot, 0, 0);
    return 0;
}

#include "graphics.c"
#include "audio.c"
#include "net.c"
#else
#include <stdio.h>
int main(void) {
    fprintf(stderr, "main_web.c is the Emscripten build. Use emcc (see build_web.sh).\n");
    return 1;
}
#endif
