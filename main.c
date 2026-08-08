#include <android_native_app_glue.h>
#include "runtime.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"
#include "third_party/font_library.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

static Buffer current_buffer = {0};
static int frame_count = 0;
static int init_done = 0;
static char log_text[16384] = {0};
static size_t log_len = 0;

typedef struct Texture Texture;
struct Texture {
    Texture *next;
    char *name;
    int width;
    int height;
    stbi_uc *pixels;
};

static AAssetManager *asset_manager = NULL;
static Texture *textures = NULL;

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

static char *copy_string(const char *source) {
    size_t length;
    char *copy;

    if (!source) return NULL;
    length = strlen(source) + 1;
    copy = (char *)malloc(length);
    if (copy) memcpy(copy, source, length);
    return copy;
}

static const char *normalise_asset_name(const char *name) {
    const char *cursor;

    if (!name) return NULL;
    while (strncmp(name, "./", 2) == 0) name += 2;
    if (strncmp(name, "game/assets/", 12) == 0) {
        name += 12;
    } else if (strncmp(name, "assets/", 7) == 0) {
        name += 7;
    }
    if (!*name || *name == '/' || strchr(name, '\\')) return NULL;

    cursor = name;
    while (*cursor) {
        const char *segment = cursor;
        while (*cursor && *cursor != '/') ++cursor;
        if (cursor - segment == 2 && segment[0] == '.' && segment[1] == '.') {
            return NULL;
        }
        if (*cursor == '/') ++cursor;
    }
    return name;
}

static Texture *find_texture(const char *name) {
    Texture *texture = textures;
    while (texture) {
        if (strcmp(texture->name, name) == 0) return texture;
        texture = texture->next;
    }
    return NULL;
}

void ds_release_assets(void) {
    Texture *texture = textures;
    while (texture) {
        Texture *next = texture->next;
        stbi_image_free(texture->pixels);
        free(texture->name);
        free(texture);
        texture = next;
    }
    textures = NULL;
    asset_manager = NULL;
}

void ds_set_asset_manager(AAssetManager *assets) {
    if (asset_manager != assets) {
        ds_release_assets();
        asset_manager = assets;
    }
    if (!asset_manager) {
        ds_runtime_error("Android asset manager is unavailable; PNG files cannot be loaded");
    }
}

static Texture *load_png_texture(const char *requested_name) {
    const char *name = normalise_asset_name(requested_name);
    Texture *texture;
    AAsset *asset;
    off_t asset_length;
    stbi_uc *encoded;
    size_t offset;
    int channels;

    if (!name) {
        ds_runtime_error("invalid PNG asset path: '%s'", requested_name ? requested_name : "(null)");
        return NULL;
    }

    texture = find_texture(name);
    if (texture) return texture->pixels ? texture : NULL;
    if (!asset_manager) {
        ds_runtime_error("cannot load PNG '%s' without an Android asset manager", name);
        return NULL;
    }

    /* Create the cache entry before opening.  A missing or broken image is
     * remembered too, so a draw loop does not retry and spam logcat each frame. */
    texture = (Texture *)calloc(1, sizeof(*texture));
    if (!texture) {
        ds_runtime_error("out of memory while caching PNG '%s'", name);
        return NULL;
    }
    texture->name = copy_string(name);
    if (!texture->name) {
        free(texture);
        ds_runtime_error("out of memory while caching PNG name '%s'", name);
        return NULL;
    }
    texture->next = textures;
    textures = texture;

    asset = AAssetManager_open(asset_manager, name, AASSET_MODE_BUFFER);
    if (!asset) {
        ds_runtime_error("PNG asset not found: '%s' (put it in game/assets)", name);
        return NULL;
    }

    asset_length = AAsset_getLength(asset);
    if (asset_length <= 0 || asset_length > INT_MAX) {
        ds_runtime_error("PNG asset '%s' has an invalid size", name);
        AAsset_close(asset);
        return NULL;
    }

    encoded = (stbi_uc *)malloc((size_t)asset_length);
    if (!encoded) {
        ds_runtime_error("out of memory while reading PNG '%s'", name);
        AAsset_close(asset);
        return NULL;
    }

    offset = 0;
    while (offset < (size_t)asset_length) {
        int bytes_read = AAsset_read(
            asset, encoded + offset, (size_t)asset_length - offset
        );
        if (bytes_read <= 0) break;
        offset += (size_t)bytes_read;
    }
    AAsset_close(asset);

    if (offset != (size_t)asset_length) {
        ds_runtime_error("could not read the complete PNG asset '%s'", name);
        free(encoded);
        return NULL;
    }

    channels = 0;
    texture->pixels = stbi_load_from_memory(
        encoded,
        (int)asset_length,
        &texture->width,
        &texture->height,
        &channels,
        STBI_rgb_alpha
    );
    free(encoded);

    if (!texture->pixels || texture->width <= 0 || texture->height <= 0) {
        ds_runtime_error(
            "could not decode PNG '%s': %s",
            name,
            stbi_failure_reason() ? stbi_failure_reason() : "unknown PNG error"
        );
        stbi_image_free(texture->pixels);
        texture->pixels = NULL;
        texture->width = 0;
        texture->height = 0;
        return NULL;
    }

    ds_log("loaded PNG asset '%s' (%dx%d)", name, texture->width, texture->height);
    return texture;
}

int png_load(const char *name) {
    return load_png_texture(name) != NULL;
}

static void blend_rgba_pixel(uint32_t *destination, const stbi_uc *source) {
    stbi_uc *target = (stbi_uc *)destination;
    unsigned int alpha = source[3];
    unsigned int inverse;

    if (alpha == 0) return;
    if (alpha == 255) {
        target[0] = source[0];
        target[1] = source[1];
        target[2] = source[2];
        target[3] = 255;
        return;
    }

    inverse = 255 - alpha;
    target[0] = (stbi_uc)((source[0] * alpha + target[0] * inverse + 127) / 255);
    target[1] = (stbi_uc)((source[1] * alpha + target[1] * inverse + 127) / 255);
    target[2] = (stbi_uc)((source[2] * alpha + target[2] * inverse + 127) / 255);
    target[3] = (stbi_uc)(alpha + (target[3] * inverse + 127) / 255);
}

static int clipped_coordinate(float value, int limit) {
    if (value <= 0.0f) return 0;
    if (value >= (float)limit) return limit;
    return (int)value;
}

static void draw_texture_unrotated(
    const Texture *texture, float x, float y, float scale
) {
    float width = (float)texture->width * scale;
    float height = (float)texture->height * scale;
    int left;
    int top;
    int right;
    int bottom;
    int screen_y;

    if (!isfinite(width) || !isfinite(height) || width <= 0.0f || height <= 0.0f) return;
    if (x >= (float)current_buffer.width || y >= (float)current_buffer.height ||
        x + width <= 0.0f || y + height <= 0.0f) {
        return;
    }

    left = clipped_coordinate(floorf(x), current_buffer.width);
    top = clipped_coordinate(floorf(y), current_buffer.height);
    right = clipped_coordinate(ceilf(x + width), current_buffer.width);
    bottom = clipped_coordinate(ceilf(y + height), current_buffer.height);

    for (screen_y = top; screen_y < bottom; ++screen_y) {
        int source_y = (int)(((float)screen_y + 0.5f - y) / scale);
        int screen_x;
        if (source_y < 0) source_y = 0;
        if (source_y >= texture->height) source_y = texture->height - 1;

        for (screen_x = left; screen_x < right; ++screen_x) {
            int source_x = (int)(((float)screen_x + 0.5f - x) / scale);
            const stbi_uc *source;
            uint32_t *destination;
            if (source_x < 0) source_x = 0;
            if (source_x >= texture->width) source_x = texture->width - 1;

            source = texture->pixels +
                ((size_t)source_y * (size_t)texture->width + (size_t)source_x) * 4;
            destination = current_buffer.pixels +
                screen_y * current_buffer.stride + screen_x;
            blend_rgba_pixel(destination, source);
        }
    }
}

static void draw_texture_rotated(
    const Texture *texture, float x, float y, float angle, float scale
) {
    float cosine = cosf(angle);
    float sine = sinf(angle);
    float half_width = (float)texture->width * scale * 0.5f;
    float half_height = (float)texture->height * scale * 0.5f;
    float centre_x = x + half_width;
    float centre_y = y + half_height;
    float extent_x = fabsf(cosine) * half_width + fabsf(sine) * half_height;
    float extent_y = fabsf(sine) * half_width + fabsf(cosine) * half_height;
    int left;
    int top;
    int right;
    int bottom;
    int screen_y;

    if (!isfinite(extent_x) || !isfinite(extent_y) ||
        centre_x + extent_x <= 0.0f || centre_y + extent_y <= 0.0f ||
        centre_x - extent_x >= (float)current_buffer.width ||
        centre_y - extent_y >= (float)current_buffer.height) {
        return;
    }

    left = clipped_coordinate(floorf(centre_x - extent_x), current_buffer.width);
    top = clipped_coordinate(floorf(centre_y - extent_y), current_buffer.height);
    right = clipped_coordinate(ceilf(centre_x + extent_x), current_buffer.width);
    bottom = clipped_coordinate(ceilf(centre_y + extent_y), current_buffer.height);

    for (screen_y = top; screen_y < bottom; ++screen_y) {
        int screen_x;
        for (screen_x = left; screen_x < right; ++screen_x) {
            float dx = (float)screen_x + 0.5f - centre_x;
            float dy = (float)screen_y + 0.5f - centre_y;
            float local_x = cosine * dx + sine * dy;
            float local_y = -sine * dx + cosine * dy;
            int source_x = (int)floorf(local_x / scale + texture->width * 0.5f);
            int source_y = (int)floorf(local_y / scale + texture->height * 0.5f);
            const stbi_uc *source;
            uint32_t *destination;

            if (source_x < 0 || source_x >= texture->width ||
                source_y < 0 || source_y >= texture->height) {
                continue;
            }

            source = texture->pixels +
                ((size_t)source_y * (size_t)texture->width + (size_t)source_x) * 4;
            destination = current_buffer.pixels +
                screen_y * current_buffer.stride + screen_x;
            blend_rgba_pixel(destination, source);
        }
    }
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
    Texture *texture;

    if (!current_buffer.pixels || current_buffer.width <= 0 ||
        current_buffer.height <= 0 || current_buffer.stride < current_buffer.width ||
        !isfinite(x) || !isfinite(y) || !isfinite(angle) ||
        !isfinite(scale) || scale <= 0.0f) {
        return;
    }

    texture = load_png_texture(name);
    if (!texture) return;

    if (fabsf(angle) < 0.00001f) {
        draw_texture_unrotated(texture, x, y, scale);
    } else {
        draw_texture_rotated(texture, x, y, angle, scale);
    }
}

void text(const char *string, float x, float y, uint32_t color) {
    text_scaled(string, x, y, color, 1.0f);
}

void text_scaled(const char *string, float x, float y, uint32_t color, float scale) {
    if (!current_buffer.pixels || !string ||
        current_buffer.width <= 0 || current_buffer.height <= 0 ||
        current_buffer.stride < current_buffer.width) {
        return;
    }
    ds_render_text(
        current_buffer.pixels,
        current_buffer.width,
        current_buffer.height,
        current_buffer.stride,
        string,
        x,
        y,
        color,
        scale
    );
}

int text_width(const char *string) {
    return ds_measure_text(string);
}

int text_height(void) {
    return DS_FONT_HEIGHT;
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
                ds_release_assets();
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
