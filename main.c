#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <android_native_app_glue.h>
#include "runtime.h"
#include "net.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_activity.h>

static int init_done = 0, script_active = 0;
static AAssetManager *script_assets = NULL;
static uint64_t restart_after_ns = 0, prev_frame_ns = 0;
static unsigned int restart_failures = 0;
static struct android_app *g_app = NULL;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ? (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec : 0;
}
static void protected_init(void *u) { init((AAssetManager *)u); }
static void protected_reset(void *u) { (void)u; reset(); }
static void protected_update(void *u) { (void)u; update(); }
static void protected_draw(void *u) { draw((Buffer *)u); }
typedef struct { float x, y; int action, id; } TouchCall;
static void protected_touch(void *u) { TouchCall *c = (TouchCall *)u; touch(c->x, c->y, c->action, c->id); }

static void mark_script_failed(const char *hook) {
    const char *msg = ds_runtime_error_message();
    ds_console_log(1, "script error in '%s': %s", hook ? hook : "?", msg);
    unsigned int shift = restart_failures < 5 ? restart_failures : 5;
    script_active = 0; ds_request_script_restart();
    restart_after_ns = monotonic_ns() + (1000000000ull << shift);
    ++restart_failures;
}

static int start_script(int rst) {
    ds_clear_runtime_error(); ds_clear_script_restart(); ds_string_pool_reset();
    if (rst && !ds_call_protected(protected_reset, NULL, "reset")) { mark_script_failed("reset"); return 0; }
    if (!ds_call_protected(protected_init, script_assets, "init")) { mark_script_failed("init"); return 0; }
    restart_failures = 0; script_active = 1; return 1;
}

static void handle_cmd(struct android_app *app, int32_t cmd) {
    if (!app) return;
    g_app = app;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (!app->window) { init_done = 0; return; }
            screen_w = ANativeWindow_getWidth(app->window); screen_h = ANativeWindow_getHeight(app->window);
            if (screen_w <= 0 || screen_h <= 0) { init_done = 0; return; }
            script_assets = app->activity ? app->activity->assetManager : NULL;
            ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
            ds_set_activity(app->activity);
            if (!ds_graphics_init(script_assets)) { init_done = 0; return; }
            init_done = 1; script_active = restart_failures = 0;
            ds_clear_script_restart(); start_script(0); break;
        case APP_CMD_TERM_WINDOW: init_done = script_active = 0; ds_graphics_shutdown(); break;
        default: break;
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    (void)app;
    if (!event) return 0;
    int32_t type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_MOTION && script_active) {
        TouchCall call; size_t count = AMotionEvent_getPointerCount(event); if (!count) return 0;
        int raw = AMotionEvent_getAction(event), action = raw & AMOTION_EVENT_ACTION_MASK;
        if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) action = AMOTION_EVENT_ACTION_DOWN;
        else if (action == AMOTION_EVENT_ACTION_POINTER_UP) action = AMOTION_EVENT_ACTION_UP;
        size_t idx = (size_t)((raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (idx >= count) idx = 0;
        size_t i = (action == AMOTION_EVENT_ACTION_MOVE) ? 0 : idx;
        count = (action == AMOTION_EVENT_ACTION_MOVE) ? count : idx + 1;
        for (; i < count; i++) {
            call.x = AMotionEvent_getX(event, i); call.y = AMotionEvent_getY(event, i);
            call.action = action; call.id = AMotionEvent_getPointerId(event, i);
            if (!ds_call_protected(protected_touch, &call, "touch")) { mark_script_failed("touch"); break; }
        }
        return 1;
    } else if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t act = AKeyEvent_getAction(event), key = AKeyEvent_getKeyCode(event), meta = AKeyEvent_getMetaState(event);
        if ((act == AKEY_EVENT_ACTION_DOWN || act == AKEY_EVENT_ACTION_MULTIPLE) && keyboard_handle_key(key, act, meta)) return 1;
        if (key == AKEYCODE_BACK && act == AKEY_EVENT_ACTION_UP) return 0;
        return 1;
    }
    return 0;
}

void android_main(struct android_app *app) {
    Buffer frame = {0}; if (!app) return;
    app->onAppCmd = handle_cmd; app->onInputEvent = handle_input;
    net_set_java_vm(app->activity->vm); ds_set_activity(app->activity);
    for (;;) {
        struct android_poll_source *source = NULL; int ident;
        while ((ident = ALooper_pollOnce(script_active ? 0 : 10, NULL, NULL, (void **)&source)) >= 0) {
            if (source && source->process) source->process(app, source);
            if (app->destroyRequested) { init_done = script_active = 0; ds_graphics_shutdown(); return; }
        }
        if (!app->window || !init_done || app->destroyRequested) continue;
        if (!script_active && ds_script_restart_requested() && monotonic_ns() >= restart_after_ns) start_script(1);
        if (script_active) {
            uint64_t now = monotonic_ns();
            dt = prev_frame_ns ? (double)(now - prev_frame_ns) / 1000000000.0 : 0.0;
            if (dt < 0.0) dt = 0.0; if (dt > 0.1) dt = 0.1;
            prev_frame_ns = now;
            if (!ds_call_protected(protected_update, NULL, "update")) mark_script_failed("update");
        }
        ANativeWindow_Buffer nb;
        if (ANativeWindow_lock(app->window, &nb, NULL) == 0) {
            frame.pixels = (uint32_t *)nb.bits; frame.width = nb.width; frame.height = nb.height; frame.stride = nb.stride;
            if (frame.pixels && frame.width > 0 && frame.height > 0 && ds_graphics_begin_frame(&frame)) {
                if (script_active) {
                    if (!ds_call_protected(protected_draw, &frame, "draw")) mark_script_failed("draw");
                    ds_graphics_end_frame();
                } else {
                    ds_graphics_error_screen(ds_runtime_error_message());
                    ds_graphics_cancel_frame();
                }
            }
            ANativeWindow_unlockAndPost(app->window);
        }
    }
}

#include "graphics.c"
#include "net.c"
