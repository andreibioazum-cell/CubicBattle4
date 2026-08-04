#include <android_native_app_glue.h>
#include "runtime.h"

#include <stdarg.h>
#include <stdio.h>

static Buffer current_buffer = {0};
static int frame_count = 0;
static int init_done = 0;
static char log_text[16384] = {0};
static size_t log_len = 0;

/* Keep a copy for diagnostics, but also send every message straight to
 * logcat.  The old implementation only appended to an in-memory buffer and
 * could advance log_len past the end when vsnprintf truncated a message;
 * startup failures then appeared to be silently swallowed. */
void ds_log(const char *format, ...) {
    va_list args;
    va_list buffer_args;

    va_start(args, format);
    va_copy(buffer_args, args);
    __android_log_vprint(ANDROID_LOG_INFO, "DimScript", format, args);

    if (log_len < sizeof(log_text) - 1) {
        int available = (int)(sizeof(log_text) - log_len);
        int written = vsnprintf(log_text + log_len, (size_t)available, format, buffer_args);

        if (written < 0) {
            log_len = sizeof(log_text) - 1;
            log_text[log_len] = '\0';
        } else if (written >= available) {
            log_len = sizeof(log_text) - 1;
            log_text[log_len] = '\0';
        } else {
            log_len += (size_t)written;
        }
    }

    va_end(buffer_args);
    va_end(args);
}

void ds_show_log(void) {
    __android_log_print(ANDROID_LOG_INFO, "DimScript", "%s", log_text);
}

static void clear_current_buffer(void) {
    current_buffer.pixels = NULL;
    current_buffer.width = 0;
    current_buffer.height = 0;
    current_buffer.stride = 0;
}

/* === Graphics === */

void cls(uint32_t color) {
    int row;

    if (!current_buffer.pixels || current_buffer.width <= 0 ||
        current_buffer.height <= 0 || current_buffer.stride <= 0) {
        return;
    }

    for (row = 0; row < current_buffer.height; ++row) {
        uint32_t *line = current_buffer.pixels + row * current_buffer.stride;
        int column;
        for (column = 0; column < current_buffer.stride; ++column) {
            line[column] = color;
        }
    }
}

void rect(float x, float y, float w, float h, uint32_t color) {
    int x1;
    int y1;
    int x2;
    int y2;
    int row;

    if (!current_buffer.pixels || current_buffer.width <= 0 ||
        current_buffer.height <= 0 || w <= 0.0f || h <= 0.0f) {
        return;
    }

    x1 = (int)(x + 0.5f);
    y1 = (int)(y + 0.5f);
    x2 = (int)(x + w + 0.5f);
    y2 = (int)(y + h + 0.5f);

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > current_buffer.width) x2 = current_buffer.width;
    if (y2 > current_buffer.height) y2 = current_buffer.height;
    if (x1 >= x2 || y1 >= y2) return;

    for (row = y1; row < y2; ++row) {
        uint32_t *line = current_buffer.pixels + row * current_buffer.stride;
        int column;
        for (column = x1; column < x2; ++column) {
            line[column] = color;
        }
    }
}

void circle(float cx, float cy, float r, uint32_t color) {
    int radius;
    int cx_int;
    int cy_int;
    int radius_squared;
    int y;

    if (!current_buffer.pixels || r <= 0.0f || current_buffer.width <= 0 ||
        current_buffer.height <= 0) {
        return;
    }

    radius = (int)(r + 0.5f);
    cx_int = (int)(cx + 0.5f);
    cy_int = (int)(cy + 0.5f);
    radius_squared = radius * radius;

    for (y = -radius; y <= radius; ++y) {
        int screen_y = cy_int + y;
        int x;
        int y_squared = y * y;

        if (screen_y < 0 || screen_y >= current_buffer.height) continue;

        for (x = -radius; x <= radius; ++x) {
            int screen_x = cx_int + x;
            if (screen_x < 0 || screen_x >= current_buffer.width) continue;
            if (x * x + y_squared <= radius_squared) {
                current_buffer.pixels[screen_y * current_buffer.stride + screen_x] = color;
            }
        }
    }
}

void ring(float cx, float cy, float r, float thickness, uint32_t color) {
    int radius;
    int thick;
    int cx_int;
    int cy_int;
    int outer_squared;
    int inner_squared;
    int y;

    if (!current_buffer.pixels || r <= 0.0f || thickness <= 0.0f ||
        current_buffer.width <= 0 || current_buffer.height <= 0) {
        return;
    }

    radius = (int)(r + 0.5f);
    thick = (int)(thickness + 0.5f);
    cx_int = (int)(cx + 0.5f);
    cy_int = (int)(cy + 0.5f);
    outer_squared = radius * radius;
    inner_squared = radius - thick;
    inner_squared = inner_squared > 0 ? inner_squared * inner_squared : 0;

    for (y = -radius; y <= radius; ++y) {
        int screen_y = cy_int + y;
        int x;
        int y_squared = y * y;

        if (screen_y < 0 || screen_y >= current_buffer.height) continue;

        for (x = -radius; x <= radius; ++x) {
            int screen_x = cx_int + x;
            int distance_squared;
            if (screen_x < 0 || screen_x >= current_buffer.width) continue;
            distance_squared = x * x + y_squared;
            if (distance_squared <= outer_squared && distance_squared >= inner_squared) {
                current_buffer.pixels[screen_y * current_buffer.stride + screen_x] = color;
            }
        }
    }
}

void tex(float x, float y, const char *name, float angle, float scale) {
    (void)x;
    (void)y;
    (void)name;
    (void)angle;
    (void)scale;
}

void text(const char *string, float x, float y, uint32_t color) {
    (void)string;
    (void)x;
    (void)y;
    (void)color;
}

/* === Android event loop === */

struct engine {
    struct android_app *app;
};

static void handle_cmd(struct android_app *app, int32_t command) {
    if (!app) {
        ds_runtime_error("received an Android command without an app instance");
        return;
    }

    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (!app->window) {
                init_done = 0;
                clear_current_buffer();
                ds_runtime_error("APP_CMD_INIT_WINDOW arrived without a window");
                return;
            }

            screen_w = ANativeWindow_getWidth(app->window);
            screen_h = ANativeWindow_getHeight(app->window);
            if (screen_w <= 0 || screen_h <= 0) {
                init_done = 0;
                ds_runtime_error("Android returned an invalid window size: %dx%d", screen_w, screen_h);
                return;
            }

            ds_log("initialising DimScript window: %dx%d", screen_w, screen_h);
            if (ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888) != 0) {
                ds_runtime_error("could not select RGBA_8888 window buffers");
            }

            /* This is intentionally a normal call: script errors remain
             * visible in logcat instead of being swallowed. */
            init_done = 0;
            init(app->activity ? app->activity->assetManager : NULL);
            frame_count = 0;
            init_done = 1;
            break;

        case APP_CMD_TERM_WINDOW:
            init_done = 0;
            clear_current_buffer();
            break;

        default:
            break;
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    int action;

    (void)app;
    if (!event || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
        return 0;
    }
    if (AMotionEvent_getPointerCount(event) <= 0) {
        return 0;
    }

    action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    touch(AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0), action);
    return 1;
}

void android_main(struct android_app *app) {
    struct engine engine_state = {0};

    if (!app) {
        ds_runtime_error("android_main received a null app instance");
        return;
    }

    engine_state.app = app;
    app->userData = &engine_state;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;

    ds_log("DimScript application started");

    for (;;) {
        struct android_poll_source *source = NULL;
        int ident;

        while ((ident = ALooper_pollOnce(0, NULL, NULL, (void **)&source)) >= 0) {
            if (source && source->process) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                clear_current_buffer();
                return;
            }
        }

        if (app->window && !app->destroyRequested && init_done) {
            ANativeWindow_Buffer buffer;

            update();
            if (ANativeWindow_lock(app->window, &buffer, NULL) == 0) {
                current_buffer.pixels = (uint32_t *)buffer.bits;
                current_buffer.width = buffer.width;
                current_buffer.height = buffer.height;
                current_buffer.stride = buffer.stride;

                if (current_buffer.pixels && current_buffer.width > 0 &&
                    current_buffer.height > 0 && current_buffer.stride >= current_buffer.width) {
                    draw(&current_buffer);
                } else {
                    ds_runtime_error("Android supplied an invalid framebuffer");
                }

                ANativeWindow_unlockAndPost(app->window);
                ++frame_count;
            } else if ((frame_count % 60) == 0) {
                ds_runtime_error("ANativeWindow_lock failed");
            }
        } else {
            /* Avoid a busy loop while Android has no surface. */
            (void)ALooper_pollOnce(10, NULL, NULL, NULL);
        }
    }
}
