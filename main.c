#include <android_native_app_glue.h>
#include "runtime.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static Buffer current_buffer = {0};
static int frame_count = 0;
static int init_done = 0;
static char log_text[16384] = {0};
static int log_len = 0;

// === ЛОГ В БУФЕР ===
void ds_log(const char* fmt, ...) {
    if (log_len >= sizeof(log_text) - 256) return;
    
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(log_text + log_len, sizeof(log_text) - log_len - 1, fmt, args);
    va_end(args);
    
    if (written > 0) {
        log_len += written;
    }
}

// === ПОКАЗАТЬ ЛОГ НА ЭКРАНЕ (временная заглушка) ===
void ds_show_log() {
    // Просто копируем в переменную, потом можно вывести на экран
    ds_log("Log len: %d\n", log_len);
}

// === ГРАФИКА ===

void cls(uint32_t color) {
    if (!current_buffer.pixels) return;
    int total = current_buffer.stride * current_buffer.height;
    if (total <= 0) return;
    
    for (int i = 0; i < total && i < current_buffer.stride * current_buffer.height; i++) {
        current_buffer.pixels[i] = color;
    }
}

void rect(float x, float y, float w, float h, uint32_t color) {
    if (!current_buffer.pixels) return;
    
    int x1 = (int)(x + 0.5f);
    int y1 = (int)(y + 0.5f);
    int x2 = (int)(x + w + 0.5f);
    int y2 = (int)(y + h + 0.5f);
    
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > current_buffer.width) x2 = current_buffer.width;
    if (y2 > current_buffer.height) y2 = current_buffer.height;
    
    if (x1 >= x2 || y1 >= y2) return;
    
    for (int row = y1; row < y2 && row < current_buffer.height; row++) {
        uint32_t* line = current_buffer.pixels + row * current_buffer.stride;
        for (int col = x1; col < x2 && col < current_buffer.width; col++) {
            line[col] = color;
        }
    }
}

void circle(float cx, float cy, float r, uint32_t color) {
    if (!current_buffer.pixels || r <= 0) return;
    
    int rad = (int)(r + 0.5f);
    int cx_int = (int)(cx + 0.5f);
    int cy_int = (int)(cy + 0.5f);
    int r2 = rad * rad;
    
    for (int y = -rad; y <= rad; y++) {
        int sy = cy_int + y;
        if (sy < 0 || sy >= current_buffer.height) continue;
        uint32_t* line = current_buffer.pixels + sy * current_buffer.stride;
        int y2 = y * y;
        for (int x = -rad; x <= rad; x++) {
            int sx = cx_int + x;
            if (sx < 0 || sx >= current_buffer.width) continue;
            if (x*x + y2 <= r2) {
                line[sx] = color;
            }
        }
    }
}

void ring(float cx, float cy, float r, float t, uint32_t color) {
    if (!current_buffer.pixels || r <= 0 || t <= 0) return;
    
    int rad = (int)(r + 0.5f);
    int thick = (int)(t + 0.5f);
    int cx_int = (int)(cx + 0.5f);
    int cy_int = (int)(cy + 0.5f);
    int r_out2 = rad * rad;
    int r_in2 = (rad - thick) * (rad - thick);
    
    if (r_in2 < 0) r_in2 = 0;
    
    for (int y = -rad; y <= rad; y++) {
        int sy = cy_int + y;
        if (sy < 0 || sy >= current_buffer.height) continue;
        uint32_t* line = current_buffer.pixels + sy * current_buffer.stride;
        int y2 = y * y;
        for (int x = -rad; x <= rad; x++) {
            int sx = cx_int + x;
            if (sx < 0 || sx >= current_buffer.width) continue;
            int d2 = x*x + y2;
            if (d2 <= r_out2 && d2 >= r_in2) {
                line[sx] = color;
            }
        }
    }
}

void tex(float x, float y, const char* name, float angle, float scale) {}
void text(const char* str, float x, float y, uint32_t color) {}

// === ДВИЖОК ===

struct engine { struct android_app* app; };

static void handle_cmd(struct android_app* app, int32_t cmd) {
    struct engine* e = (struct engine*)app->userData;
    
    switch(cmd) {
        case APP_CMD_INIT_WINDOW: {
            ds_log("=== INIT WINDOW ===\n");
            screen_w = ANativeWindow_getWidth(app->window);
            screen_h = ANativeWindow_getHeight(app->window);
            ds_log("Screen: %dx%d\n", screen_w, screen_h);
            
            ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
            
            ds_log("Calling init()...\n");
            init(app->activity->assetManager);
            ds_log("init() done!\n");
            init_done = 1;
            break;
        }
        case APP_CMD_TERM_WINDOW: {
            ds_log("=== TERM WINDOW ===\n");
            break;
        }
        default: {
            ds_log("CMD: %d\n", cmd);
            break;
        }
    }
}

static int32_t handle_input(struct android_app* app, AInputEvent* event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        float x = AMotionEvent_getX(event, 0);
        float y = AMotionEvent_getY(event, 0);
        int action = AMotionEvent_getAction(event);
        
        touch(x, y, action);
        return 1;
    }
    return 0;
}

void android_main(struct android_app* app) {
    ds_log("=== ANDROID_MAIN STARTED ===\n");
    ds_log("Log buffer size: %d bytes\n", sizeof(log_text));
    
    struct engine e = {0};
    e.app = app;
    app->userData = &e;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    
    while (1) {
        struct android_poll_source* source;
        int ident;
        
        while ((ident = ALooper_pollOnce(0, NULL, NULL, (void**)&source)) >= 0) {
            if (source) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                ds_log("=== DESTROY ===\n");
                return;
            }
        }
        
        if (app->window && !app->destroyRequested && init_done) {
            frame_count++;
            
            if (frame_count % 2 == 0) {
                update();
            }
            
            ANativeWindow_Buffer buf;
            if (ANativeWindow_lock(app->window, &buf, NULL) == 0) {
                current_buffer.pixels = (uint32_t*)buf.bits;
                current_buffer.width = buf.width;
                current_buffer.height = buf.height;
                current_buffer.stride = buf.stride;
                
                if (current_buffer.pixels && current_buffer.width > 0 && current_buffer.height > 0) {
                    draw(&current_buffer);
                }
                
                ANativeWindow_unlockAndPost(app->window);
            }
        } else {
            usleep(10000);
        }
    }
}
