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
#include <unistd.h>
static int init_done = 0;
static int script_active = 0;
/* init() already ran once in this process: a later start has to reset() the
 * globals first (new activity after a destroy, or a script that had failed). */
static int script_started_once = 0;
static AAssetManager *script_assets = NULL;
static uint64_t restart_after_ns = 0;
static unsigned int restart_failures = 0;
static uint64_t prev_frame_ns = 0;
static uint64_t prev_loop_ns = 0;
static struct android_app *g_app = NULL;
static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}
/* A frame is always drawn at the full window size: the upscale and the fps cap
 * settings were removed at the player's request, since both only hurt the picture
 * and made frames wait for nothing. */
static int phys_w = 0, phys_h = 0;
/* screen_w and screen_h are what the script sees: always the full window size. */
static void apply_screen_size(void) {
    if (phys_w < 1 || phys_h < 1) return;
    screen_w = phys_w;
    screen_h = phys_h;
}
static void protected_init(void *userdata) { init((AAssetManager *)userdata); }
static void protected_reset(void *userdata) { (void)userdata; reset(); }
static void protected_update(void *userdata) { (void)userdata; update(); }
static void protected_draw(void *userdata) { draw((Buffer *)userdata); }
typedef struct { float x; float y; int action; int id; } TouchCall;
static void protected_touch(void *userdata) {
    TouchCall *call = (TouchCall *)userdata;
    touch(call->x, call->y, call->action, call->id);
}
static int back_consumed = 0;
typedef struct { int handled; } BackCall;
static void protected_back(void *userdata) {
    BackCall *call = (BackCall *)userdata;
    call->handled = back_pressed();
}
static void mark_script_failed(const char *hook) {
    const char *message = ds_runtime_error_message();
    __android_log_print(ANDROID_LOG_ERROR, "DimScript","script hook '%s' stopped: %s; scheduling a restart",hook?hook:"unknown",message);
    ds_console_log(1, "script error: hook '%s' stopped: %s; restarting", hook?hook:"unknown", message);
    unsigned int shift = restart_failures < 5 ? restart_failures : 5;
    uint64_t delay = 1000000000ull << shift;
    script_active = 0;
    ds_request_script_restart();
    restart_after_ns = monotonic_ns() + delay;
    ++restart_failures;
}
static int start_script(int reset_state) {
    int ok;
    ds_clear_runtime_error(); ds_clear_script_restart(); ds_string_pool_reset();
    if (reset_state) { ok = ds_call_protected(protected_reset, NULL, "reset"); if (!ok) { mark_script_failed("reset"); return 0; } }
    ds_clear_runtime_error();
    ok = ds_call_protected(protected_init, script_assets, "init");
    if (!ok) { mark_script_failed("init"); return 0; }
    ds_clear_runtime_error(); restart_failures = 0; script_active = 1; script_started_once = 1; return 1;
}
static void restart_script_if_due(void) {
    uint64_t now; if (script_active || !ds_script_restart_requested()) return;
    now = monotonic_ns(); if (now < restart_after_ns) return; (void)start_script(1);
}
static void handle_cmd(struct android_app *app, int32_t command) {
    if (!app) { ds_runtime_error("no app"); return; }
    g_app = app;
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (!app->window) { init_done = 0; return; }
            phys_w = ANativeWindow_getWidth(app->window);
            phys_h = ANativeWindow_getHeight(app->window);
            if (phys_w <= 0 || phys_h <= 0) { init_done = 0; return; }
            apply_screen_size();
            script_assets = app->activity ? app->activity->assetManager : NULL;
            ds_set_activity(app->activity);
            /* Returning from the background keeps the Vulkan device and the
             * textures: only a surface for the new window is made. The very
             * first window builds the whole renderer. */
            if (!ds_graphics_init(script_assets, app->window)) { init_done = 0; return; }
            /* Sounds sit in the same assets folder (sounds/...) and play through
             * an AudioTrack; on a return the loaded sounds are still there and
             * only the output starts again. */
            ds_sound_init(script_assets);
            ds_sound_resume();
            init_done = 1;
            /* The long pause in the background is not a frame: without this the
             * first frame after the return would count as a huge vsync miss. */
            prev_frame_ns = 0; prev_loop_ns = 0;
            if (script_active) {
                /* Back from the background: the script simply goes on where the
                 * player left it. It used to start over here - init() again,
                 * every texture and the 4 MB of music decoded again and a
                 * blocking autologin request on the game thread, right while
                 * the renderer was being rebuilt from zero - and the game
                 * crashed on re-entry on budget phones (TECNO Spark Go 1). */
                ds_log("window is back (%dx%d): the game continues", phys_w, phys_h);
                break;
            }
            restart_failures = 0; ds_clear_script_restart();
            (void)start_script(script_started_once); break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED:
        case APP_CMD_CONFIG_CHANGED:
            /* adjustResize changes the surface while the keyboard is open, and the
             * graphics layer resizes the swapchain and the offscreen target at the
             * start of the next frame. */
            if (app->window) {
                int w = ANativeWindow_getWidth(app->window);
                int h = ANativeWindow_getHeight(app->window);
                if (w > 0 && h > 0) { phys_w = w; phys_h = h; apply_screen_size(); }
            }
            break;
        case APP_CMD_TERM_WINDOW:
            /* The game goes to the background. The script is paused, not
             * dropped: no frames run until the next window (init_done = 0),
             * and the room threads keep matching the state the script still
             * has, so on return an online match reconnects instead of the
             * whole game restarting. Only the window half of the renderer and
             * the audio output go; this has to be quick, since the UI thread
             * waits in onSurfaceDestroyed until it returns. */
            init_done = 0; keyboard_hide();
            ds_graphics_window_lost(); ds_sound_suspend(); break;
        case APP_CMD_GAINED_FOCUS: ds_sound_resume(); break;
        case APP_CMD_LOST_FOCUS: ds_sound_pause(); break;
        default: break;
    }
}
static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    (void)app;
    if (!event) return 0;
    int32_t type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        if (!script_active) return 0;
        TouchCall call; size_t count, index, i; int raw, action;
        count = AMotionEvent_getPointerCount(event); if (count == 0) return 0;
        raw = AMotionEvent_getAction(event); action = raw & AMOTION_EVENT_ACTION_MASK;
        if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) action = AMOTION_EVENT_ACTION_DOWN;
        else if (action == AMOTION_EVENT_ACTION_POINTER_UP) action = AMOTION_EVENT_ACTION_UP;
        index = (size_t)((raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (index >= count) index = 0;
        i = (action == AMOTION_EVENT_ACTION_MOVE) ? 0 : index;
        count = (action == AMOTION_EVENT_ACTION_MOVE) ? count : index + 1;
        for (; i < count; i++) {
            /* Window coordinates are the pixels the script works in, since the
             * removed upscale no longer affects them. */
            call.x = AMotionEvent_getX(event, i);
            call.y = AMotionEvent_getY(event, i);
            /* Window edge: clamped to the virtual screen, so a touch on the very
             * rim does not land outside it. */
            if (screen_w > 0) {
                if (call.x < 0) call.x = 0;
                if (call.x > (float)(screen_w - 1)) call.x = (float)(screen_w - 1);
            }
            if (screen_h > 0) {
                if (call.y < 0) call.y = 0;
                if (call.y > (float)(screen_h - 1)) call.y = (float)(screen_h - 1);
            }
            call.action = action; call.id = AMotionEvent_getPointerId(event, i);
            if (!ds_call_protected(protected_touch, &call, "touch")) { mark_script_failed("touch"); break; }
        }
        return 1;
    } else if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t action = AKeyEvent_getAction(event);
        int32_t key = AKeyEvent_getKeyCode(event);
        int32_t meta = AKeyEvent_getMetaState(event);
        if (key == AKEYCODE_BACK && action == AKEY_EVENT_ACTION_DOWN &&
            (keyboard_visible() || keyboard_uses_editor())) {
            keyboard_hide();
            return 1;
        }
        if (keyboard_visible() &&
            (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_MULTIPLE)) {
            if (keyboard_handle_key(key, action, meta)) return 1;
        }
        if (key == AKEYCODE_BACK) {
            /* System Back goes to the script first, which closes the chat or
             * returns to the previous screen. If the script does not take it, in
             * the lobby for instance, both DOWN and UP reach the system and
             * Android backgrounds the game. */
            if (action == AKEY_EVENT_ACTION_DOWN) {
                BackCall call = {0};
                back_consumed = 0;
                if (!script_active) return 0;
                if (!ds_call_protected(protected_back, &call, "back_pressed")) { mark_script_failed("back_pressed"); return 1; }
                back_consumed = call.handled ? 1 : 0;
                return back_consumed;
            }
            if (action == AKEY_EVENT_ACTION_UP) { int c = back_consumed; back_consumed = 0; return c; }
            return back_consumed;
        }
        /* While a system EditText owns the text, keys including Backspace have to
         * reach it, otherwise a deletion only applies to the game buffer while the
         * editor keeps the old text and appends it to the new input. The check asks
         * the editor rather than a visibility flag, because the "keyboard is up"
         * heuristic can be wrong in landscape and keys must not be lost. */
        if (keyboard_uses_editor()) return 0;
        return 1;
    }
    return 0;
}
void android_main(struct android_app *app) {
    Buffer frame = {0}; if (!app) return;
/* rand() in scripts uses the libc generator, which does not seed itself: without
 * srand() the candy spawns, their flight directions and other "random" throws
 * would follow one and the same sequence every run. */
    srand((unsigned)(time(NULL) * 2654435761u) ^ ((unsigned)getpid() * 0x9E3779B9u));
    app->onAppCmd = handle_cmd; app->onInputEvent = handle_input;
    net_set_java_vm(app->activity->vm);
    ds_sound_set_java_vm((void *)app->activity->vm);
    net_set_data_path(app->activity->internalDataPath);
    ds_set_activity(app->activity);
    ds_log("DimScript Android + Vulkan renderer + system keyboard (JNI)");
    for (;;) {
        struct android_poll_source *source = NULL; int ident;
        /* Without a window nothing is drawn, so the loop sleeps in the looper
         * until the next event instead of spinning: a busy loop in the
         * background burns the CPU, and Android kills such a cached app. */
        int drawing = app->window && init_done;
        int timeout = drawing ? (script_active ? 0 : 10) : 250;
        while ((ident = ALooper_pollOnce(timeout, NULL, NULL, (void **)&source)) >= 0) {
            if (source && source->process) source->process(app, source);
            if (app->destroyRequested) {
                /* The activity itself ends. A new one in the same process gets
                 * a fresh script start (with reset()), so the room threads of
                 * this one are stopped here as well. */
                init_done = 0; script_active = 0; keyboard_hide(); net_disconnect();
                ds_graphics_shutdown(); ds_sound_shutdown(); return;
            }
            timeout = 0; /* drain the rest of the queued events at once */
        }
        if (!app->window || !init_done || app->destroyRequested) continue;
        restart_script_if_due();
        uint64_t frame_start = monotonic_ns();
        /* The real frame period, vsync wait included, goes to the graphics layer,
         * where the automatic internal resolution judges whether the device holds
         * 60 fps at the full window size. */
        if (prev_loop_ns) ds_graphics_report_frame_interval((double)(frame_start - prev_loop_ns) / 1e9);
        prev_loop_ns = frame_start;
        apply_screen_size();
        if (script_active) {
            uint64_t now = frame_start;
            dt = prev_frame_ns ? (double)(now - prev_frame_ns) / 1000000000.0 : 0.0;
            if (dt < 0.0) dt = 0.0; if (dt > 0.1) dt = 0.1;
            prev_frame_ns = now;
            if (!ds_call_protected(protected_update, NULL, "update")) mark_script_failed("update");
            else if (ds_script_restart_requested()) { script_active = 0; restart_after_ns = monotonic_ns(); }
        }
        /* A frame is Vulkan: acquire a swapchain image, let the script collect its
         * commands, and in end_frame the GPU draws them into the offscreen target
         * and presents. */
        frame.pixels = NULL;
        frame.width = screen_w;
        frame.height = screen_h;
        frame.stride = screen_w;
        if (frame.width > 0 && frame.height > 0 && ds_graphics_begin_frame(&frame)) {
            int draw_failed = 0;
            if (script_active) {
                if (!ds_call_protected(protected_draw, &frame, "draw")) { mark_script_failed("draw"); draw_failed = 1; }
                else if (ds_script_restart_requested()) { script_active = 0; restart_after_ns = monotonic_ns(); }
            }
            if (!script_active) {
                if (draw_failed || ds_script_has_error()) {
                    /* The error screen goes through the same commands and the same
                     * pipeline, so the frame is finished rather than abandoned. */
                    ds_graphics_error_screen(ds_runtime_error_message());
                    ds_graphics_end_frame();
                } else ds_graphics_cancel_frame();
            } else ds_graphics_end_frame();
        }
    }
}
#include "graphics.c"
#include "net.c"
#include "sound.c"