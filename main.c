#include <android_native_app_glue.h>
#include "runtime.h"
#include <string.h>
#include <math.h>
#include <arm_neon.h>

// Глобальный буфер для рисования
static Buffer current_buffer = {0};

// === РЕАЛИЗАЦИЯ ГРАФИКИ ===

void cls(uint32_t color) {
    if (!current_buffer.pixels) return;
    uint32x4_t v_color = vdupq_n_u32(color);
    int total = current_buffer.stride * current_buffer.height;
    int i = 0;
    for (; i <= total - 4; i += 4) {
        vst1q_u32(&current_buffer.pixels[i], v_color);
    }
    for (; i < total; i++) {
        current_buffer.pixels[i] = color;
    }
}

void rect(float x, float y, float w, float h, uint32_t color) {
    if (!current_buffer.pixels) return;
    int x1 = (int)x;
    int y1 = (int)y;
    int x2 = (int)(x + w);
    int y2 = (int)(y + h);
    
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > current_buffer.width) x2 = current_buffer.width;
    if (y2 > current_buffer.height) y2 = current_buffer.height;
    
    for (int row = y1; row < y2; row++) {
        uint32_t* line = current_buffer.pixels + row * current_buffer.stride;
        for (int col = x1; col < x2; col++) {
            line[col] = color;
        }
    }
}

void circle(float cx, float cy, float r, uint32_t color) {
    if (!current_buffer.pixels) return;
    int rad = (int)r;
    int r2 = rad * rad;
    for (int y = -rad; y <= rad; y++) {
        int sy = (int)cy + y;
        if (sy < 0 || sy >= current_buffer.height) continue;
        uint32_t* line = current_buffer.pixels + sy * current_buffer.stride;
        int y2 = y * y;
        for (int x = -rad; x <= rad; x++) {
            int sx = (int)cx + x;
            if (sx < 0 || sx >= current_buffer.width) continue;
            if (x*x + y2 <= r2) {
                line[sx] = color;
            }
        }
    }
}

void ring(float cx, float cy, float r, float t, uint32_t color) {
    if (!current_buffer.pixels) return;
    int rad = (int)r;
    int thick = (int)t;
    int r_out2 = rad * rad;
    int r_in2 = (rad - thick) * (rad - thick);
    for (int y = -rad; y <= rad; y++) {
        int sy = (int)cy + y;
        if (sy < 0 || sy >= current_buffer.height) continue;
        uint32_t* line = current_buffer.pixels + sy * current_buffer.stride;
        int y2 = y * y;
        for (int x = -rad; x <= rad; x++) {
            int sx = (int)cx + x;
            if (sx < 0 || sx >= current_buffer.width) continue;
            int d2 = x*x + y2;
            if (d2 <= r_out2 && d2 >= r_in2) {
                line[sx] = color;
            }
        }
    }
}

void tex(float x, float y, const char* name, float angle, float scale) {
    // Загрузка текстуры из ассетов через stb_image
}

void text(const char* str, float x, float y, uint32_t color) {
    if (!current_buffer.pixels || !str) return;
    // Простой рендеринг через символы
    int px = (int)x;
    int py = (int)y;
    for (const char* p = str; *p; p++) {
        // Рисуем простые пиксели как заглушку для текста
        // В реальном проекте используем stb_truetype
        px += 8;
    }
}

// === СТРУКТУРА ДВИЖКА ===
struct engine { struct android_app* app; };

static void handle_cmd(struct android_app* app, int32_t cmd) {
    struct engine* e = (struct engine*)app->userData;
    if (cmd == APP_CMD_INIT_WINDOW) {
        screen_w = ANativeWindow_getWidth(app->window);
        screen_h = ANativeWindow_getHeight(app->window);
        ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
        init(app->activity->assetManager);
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
    struct engine e = {0};
    app->userData = &e;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    
    while (1) {
        struct android_poll_source* source;
        while (ALooper_pollOnce(0, 0, 0, (void**)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) return;
        }
        
        if (app->window) {
            update();
            ANativeWindow_Buffer buf;
            if (ANativeWindow_lock(app->window, &buf, 0) == 0) {
                current_buffer.pixels = (uint32_t*)buf.bits;
                current_buffer.width = buf.width;
                current_buffer.height = buf.height;
                current_buffer.stride = buf.stride;
                
                draw(&current_buffer);
                ANativeWindow_unlockAndPost(app->window);
            }
        }
    }
}
