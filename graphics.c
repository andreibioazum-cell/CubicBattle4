#include "runtime.h"
#include "ttf_font.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#ifdef DIMSCRIPT_GRAPHICS_EMBEDDED
#define STB_IMAGE_STATIC
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"
#ifdef STB_IMAGE_STATIC
#undef STB_IMAGE_STATIC
#endif

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>

/* See the note at the end of main.c: the legacy NDK command embeds the
 * renderer in main.c.  A standalone ttf_font.c remains supported as a weak
 * compatibility translation unit. */
#ifdef DIMSCRIPT_GRAPHICS_EMBEDDED
#define DIMSCRIPT_TTF_EMBEDDED
#endif
#include "ttf_font.c"
#ifdef DIMSCRIPT_TTF_EMBEDDED
#undef DIMSCRIPT_TTF_EMBEDDED
#endif

/*
 * DimScript software renderer
 * ---------------------------
 *
 * The Android window is locked only once per frame by main.c.  Script calls
 * append high-level commands to a small frame list; the list is then rasterised
 * into the window in draw order.  This keeps the script API independent from
 * the window stride and lets the software path optimise spans, clears and
 * opaque texture rows instead of running a generic per-pixel operation for
 * every primitive.
 *
 * The TTF atlas is generated once at load time.  Text rendering still blends
 * glyph coverage into the software frame, but no font outline is parsed and no
 * glyph bitmap is rebuilt during a frame.
 */

typedef struct Texture Texture;
struct Texture {
    Texture *next;
    char *name;
    int width;
    int height;
    int opaque;
    uint32_t *pixels; /* packed as RGBA bytes in little-endian memory */
};

typedef enum {
    DS_CMD_CLEAR,
    DS_CMD_RECT,
    DS_CMD_CIRCLE,
    DS_CMD_RING,
    DS_CMD_TEXTURE,
    DS_CMD_TEXT
} DSCommandType;

typedef struct {
    DSCommandType type;
    union {
        struct { uint32_t colour; } clear;
        struct { float x, y, width, height; uint32_t colour; } rect;
        struct { float x, y, radius; uint32_t colour; } circle;
        struct { float x, y, radius, thickness; uint32_t colour; } ring;
        struct { float x, y, angle, scale; Texture *texture; } texture;
        struct { const char *string; float x, y, scale; uint32_t colour; } text;
    } value;
} DSCommand;

static Buffer *current_buffer = NULL;
static DSCommand *commands = NULL;
static size_t command_count = 0;
static size_t command_capacity = 0;
static int frame_open = 0;

static AAssetManager *asset_manager = NULL;
static Texture *textures = NULL;
static DSFont *font = NULL;
static int font_attempted = 0;

static void graphics_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_ERROR, "DimScript.Graphics", format, args);
    va_end(args);
}

static char *graphics_strdup(const char *source) {
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
    if (strncmp(name, "game/assets/", 12) == 0) name += 12;
    else if (strncmp(name, "assets/", 7) == 0) name += 7;
    if (!*name || *name == '/' || strchr(name, '\\')) return NULL;
    cursor = name;
    while (*cursor) {
        const char *segment = cursor;
        while (*cursor && *cursor != '/') ++cursor;
        if (cursor - segment == 2 && segment[0] == '.' && segment[1] == '.') return NULL;
        if (*cursor == '/') ++cursor;
    }
    return name;
}

static uint32_t pack_colour(uint32_t colour) {
    unsigned int alpha = (colour >> 24) & 0xffu;
    unsigned int red = (colour >> 16) & 0xffu;
    unsigned int green = (colour >> 8) & 0xffu;
    unsigned int blue = colour & 0xffu;
    if (alpha == 0) alpha = 255;
    /* Android RGBA_8888 is R,G,B,A in memory on the ARM little-endian
     * targets used by the project. */
    return red | (green << 8) | (blue << 16) | (alpha << 24);
}

static unsigned int channel_red(uint32_t colour) { return colour & 0xffu; }
static unsigned int channel_green(uint32_t colour) { return (colour >> 8) & 0xffu; }
static unsigned int channel_blue(uint32_t colour) { return (colour >> 16) & 0xffu; }
static unsigned int channel_alpha(uint32_t colour) { return (colour >> 24) & 0xffu; }

static uint32_t blend_pixel(uint32_t destination, uint32_t source) {
    unsigned int alpha = channel_alpha(source);
    unsigned int inverse;
    unsigned int red;
    unsigned int green;
    unsigned int blue;
    unsigned int out_alpha;
    if (alpha == 0) return destination;
    if (alpha == 255) return source;
    inverse = 255 - alpha;
    red = (channel_red(source) * alpha + channel_red(destination) * inverse + 127) / 255;
    green = (channel_green(source) * alpha + channel_green(destination) * inverse + 127) / 255;
    blue = (channel_blue(source) * alpha + channel_blue(destination) * inverse + 127) / 255;
    out_alpha = alpha + (channel_alpha(destination) * inverse + 127) / 255;
    return red | (green << 8) | (blue << 16) | (out_alpha << 24);
}

static uint32_t blend_coverage(uint32_t destination, uint32_t colour, unsigned int coverage) {
    unsigned int alpha;
    if (coverage == 0) return destination;
    alpha = channel_alpha(colour) * coverage / 255;
    return blend_pixel(destination, (colour & 0x00ffffffu) | (alpha << 24));
}

static void fill_span(uint32_t *destination, int count, uint32_t colour) {
    /* A tight unrolled fill is substantially faster than a branch-heavy
     * pixel helper while remaining valid for any 4-byte alignment. */
    while (count >= 8) {
        destination[0] = colour;
        destination[1] = colour;
        destination[2] = colour;
        destination[3] = colour;
        destination[4] = colour;
        destination[5] = colour;
        destination[6] = colour;
        destination[7] = colour;
        destination += 8;
        count -= 8;
    }
    while (count-- > 0) *destination++ = colour;
}

static void clear_buffer(Buffer *buffer, uint32_t colour) {
    int y;
    if (!buffer || !buffer->pixels || buffer->width <= 0 || buffer->height <= 0 ||
        buffer->stride < buffer->width) return;
    for (y = 0; y < buffer->height; ++y) {
        fill_span(buffer->pixels + y * buffer->stride, buffer->width, colour);
    }
}

static int clamp_floor(float value, int limit) {
    int result;
    if (value <= 0.0f) return 0;
    if (value >= (float)limit) return limit;
    result = (int)floorf(value);
    if (result < 0) return 0;
    if (result > limit) return limit;
    return result;
}

static int clamp_ceil(float value, int limit) {
    int result;
    if (value <= 0.0f) return 0;
    if (value >= (float)limit) return limit;
    result = (int)ceilf(value);
    if (result < 0) return 0;
    if (result > limit) return limit;
    return result;
}

static void render_rect(Buffer *buffer, float x, float y, float width, float height, uint32_t colour) {
    int left;
    int top;
    int right;
    int bottom;
    int row;
    if (!buffer || !isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) ||
        width <= 0.0f || height <= 0.0f) return;
    if (x >= buffer->width || y >= buffer->height || x + width <= 0.0f || y + height <= 0.0f) return;
    left = clamp_floor(floorf(x), buffer->width);
    top = clamp_floor(floorf(y), buffer->height);
    right = clamp_ceil(ceilf(x + width), buffer->width);
    bottom = clamp_ceil(ceilf(y + height), buffer->height);
    for (row = top; row < bottom; ++row) {
        fill_span(buffer->pixels + row * buffer->stride + left, right - left, colour);
    }
}

static void render_circle(Buffer *buffer, float x, float y, float radius, uint32_t colour) {
    int r;
    int centre_x;
    int centre_y;
    int dy;
    long long radius_squared;
    if (!buffer || !isfinite(x) || !isfinite(y) || !isfinite(radius) || radius <= 0.0f) return;
    r = (int)ceilf(radius);
    if (r <= 0) return;
    centre_x = (int)floorf(x + 0.5f);
    centre_y = (int)floorf(y + 0.5f);
    radius_squared = (long long)r * r;
    for (dy = -r; dy <= r; ++dy) {
        int screen_y = centre_y + dy;
        int half_width;
        int left;
        int right;
        if (screen_y < 0 || screen_y >= buffer->height) continue;
        half_width = (int)sqrt((double)(radius_squared - (long long)dy * dy));
        left = centre_x - half_width;
        right = centre_x + half_width + 1;
        if (left < 0) left = 0;
        if (right > buffer->width) right = buffer->width;
        if (left < right) fill_span(buffer->pixels + screen_y * buffer->stride + left,
                                     right - left, colour);
    }
}

static void render_ring(Buffer *buffer, float x, float y, float radius, float thickness,
                        uint32_t colour) {
    int outer;
    int inner;
    int centre_x;
    int centre_y;
    int dy;
    long long outer_squared;
    long long inner_squared;
    if (!buffer || !isfinite(x) || !isfinite(y) || !isfinite(radius) ||
        !isfinite(thickness) || radius <= 0.0f || thickness <= 0.0f) return;
    outer = (int)ceilf(radius);
    inner = (int)floorf(radius - thickness);
    if (inner <= 0) {
        render_circle(buffer, x, y, radius, colour);
        return;
    }
    centre_x = (int)floorf(x + 0.5f);
    centre_y = (int)floorf(y + 0.5f);
    outer_squared = (long long)outer * outer;
    inner_squared = (long long)inner * inner;
    for (dy = -outer; dy <= outer; ++dy) {
        int screen_y = centre_y + dy;
        int outer_half;
        int inner_half = -1;
        int left;
        int right;
        if (screen_y < 0 || screen_y >= buffer->height) continue;
        outer_half = (int)sqrt((double)(outer_squared - (long long)dy * dy));
        if (abs(dy) <= inner) {
            inner_half = (int)sqrt((double)(inner_squared - (long long)dy * dy));
        }
        left = centre_x - outer_half;
        right = centre_x + outer_half + 1;
        if (left < 0) left = 0;
        if (right > buffer->width) right = buffer->width;
        if (inner_half < 0) {
            if (left < right) fill_span(buffer->pixels + screen_y * buffer->stride + left,
                                         right - left, colour);
        } else {
            int inner_left = centre_x - inner_half;
            int inner_right = centre_x + inner_half + 1;
            int left_right = inner_left < right ? inner_left : right;
            int right_left = inner_right > left ? inner_right : left;
            if (left < left_right) fill_span(buffer->pixels + screen_y * buffer->stride + left,
                                             left_right - left, colour);
            if (right_left < right) fill_span(buffer->pixels + screen_y * buffer->stride + right_left,
                                               right - right_left, colour);
        }
    }
}

static Texture *find_texture(const char *name) {
    Texture *texture = textures;
    while (texture) {
        if (strcmp(texture->name, name) == 0) return texture;
        texture = texture->next;
    }
    return NULL;
}

static int open_asset(const char *name, unsigned char **data, size_t *size) {
    AAsset *asset;
    off_t length;
    size_t offset = 0;
    unsigned char *copy;
    if (!asset_manager || !data || !size) return 0;
    asset = AAssetManager_open(asset_manager, name, AASSET_MODE_BUFFER);
    if (!asset) return 0;
    length = AAsset_getLength(asset);
    if (length <= 0 || (uint64_t)length > (uint64_t)SIZE_MAX) {
        AAsset_close(asset);
        return 0;
    }
    copy = (unsigned char *)malloc((size_t)length);
    if (!copy) {
        AAsset_close(asset);
        return 0;
    }
    while (offset < (size_t)length) {
        int read_count = AAsset_read(asset, copy + offset, (size_t)length - offset);
        if (read_count <= 0) break;
        offset += (size_t)read_count;
    }
    AAsset_close(asset);
    if (offset != (size_t)length) {
        free(copy);
        return 0;
    }
    *data = copy;
    *size = (size_t)length;
    return 1;
}

static Texture *load_png_texture(const char *requested_name) {
    const char *name = normalise_asset_name(requested_name);
    Texture *texture;
    unsigned char *encoded = NULL;
    size_t encoded_size = 0;
    stbi_uc *decoded;
    int channels = 0;
    size_t pixel_count;
    size_t i;

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
    texture = (Texture *)calloc(1, sizeof(*texture));
    if (!texture) {
        ds_runtime_error("out of memory while caching PNG '%s'", name);
        return NULL;
    }
    texture->name = graphics_strdup(name);
    if (!texture->name) {
        free(texture);
        ds_runtime_error("out of memory while caching PNG name '%s'", name);
        return NULL;
    }
    texture->next = textures;
    textures = texture;
    if (!open_asset(name, &encoded, &encoded_size) || encoded_size > (size_t)INT_MAX) {
        free(encoded);
        ds_runtime_error("PNG asset not found or unreadable: '%s'", name);
        return NULL;
    }
    decoded = stbi_load_from_memory(encoded, (int)encoded_size, &texture->width, &texture->height,
                                    &channels, STBI_rgb_alpha);
    free(encoded);
    if (!decoded || texture->width <= 0 || texture->height <= 0) {
        ds_runtime_error("could not decode PNG '%s': %s", name,
                         stbi_failure_reason() ? stbi_failure_reason() : "unknown PNG error");
        stbi_image_free(decoded);
        return NULL;
    }
    pixel_count = (size_t)texture->width * (size_t)texture->height;
    texture->pixels = (uint32_t *)malloc(pixel_count * sizeof(*texture->pixels));
    if (!texture->pixels) {
        stbi_image_free(decoded);
        ds_runtime_error("out of memory while uploading PNG '%s'", name);
        return NULL;
    }
    texture->opaque = 1;
    for (i = 0; i < pixel_count; ++i) {
        const stbi_uc *source = decoded + i * 4;
        unsigned int alpha = source[3];
        texture->pixels[i] = source[0] | ((unsigned int)source[1] << 8) |
                             ((unsigned int)source[2] << 16) | (alpha << 24);
        if (alpha != 255) texture->opaque = 0;
    }
    stbi_image_free(decoded);
    ds_log("loaded PNG asset '%s' (%dx%d) for software renderer",
           name, texture->width, texture->height);
    return texture;
}

static int ensure_font(void) {
    unsigned char *data = NULL;
    size_t size = 0;
    if (font) return 1;
    if (font_attempted) return 0;
    font_attempted = 1;
    if (!open_asset("fonts/DejaVuSans.ttf", &data, &size)) {
        ds_runtime_error("TTF asset not found: fonts/DejaVuSans.ttf (put a TTF in game/assets/fonts)");
        return 0;
    }
    font = ds_font_create(data, size, 32);
    free(data);
    if (!font) {
        ds_runtime_error("could not parse TrueType font fonts/DejaVuSans.ttf");
        return 0;
    }
    return 1;
}

static DSCommand *command_push(DSCommandType type) {
    DSCommand *command;
    if (!frame_open) return NULL;
    if (command_count == command_capacity) {
        size_t capacity = command_capacity ? command_capacity * 2 : 256;
        DSCommand *new_commands;
        if (capacity < command_count || capacity > SIZE_MAX / sizeof(*commands)) {
            ds_runtime_error("too many software-renderer commands in one frame");
            return NULL;
        }
        new_commands = (DSCommand *)realloc(commands, capacity * sizeof(*commands));
        if (!new_commands) {
            ds_runtime_error("out of memory while building the software frame");
            return NULL;
        }
        commands = new_commands;
        command_capacity = capacity;
    }
    command = &commands[command_count++];
    memset(command, 0, sizeof(*command));
    command->type = type;
    return command;
}

void ds_release_assets(void) {
    Texture *texture = textures;
    while (texture) {
        Texture *next = texture->next;
        free(texture->pixels);
        free(texture->name);
        free(texture);
        texture = next;
    }
    textures = NULL;
    ds_font_destroy(font);
    font = NULL;
    font_attempted = 0;
    asset_manager = NULL;
}

void ds_set_asset_manager(AAssetManager *assets) {
    if (asset_manager != assets) {
        ds_release_assets();
        asset_manager = assets;
    }
    if (!asset_manager) ds_runtime_error("Android asset manager is unavailable");
}

int png_load(const char *name) {
    return load_png_texture(name) != NULL;
}

void cls(uint32_t colour) {
    DSCommand *command = command_push(DS_CMD_CLEAR);
    if (command) command->value.clear.colour = pack_colour(colour);
}

void rect(float x, float y, float width, float height, uint32_t colour) {
    DSCommand *command = command_push(DS_CMD_RECT);
    if (!command) return;
    command->value.rect.x = x;
    command->value.rect.y = y;
    command->value.rect.width = width;
    command->value.rect.height = height;
    command->value.rect.colour = pack_colour(colour);
}

void circle(float x, float y, float radius, uint32_t colour) {
    DSCommand *command = command_push(DS_CMD_CIRCLE);
    if (!command) return;
    command->value.circle.x = x;
    command->value.circle.y = y;
    command->value.circle.radius = radius;
    command->value.circle.colour = pack_colour(colour);
}

void ring(float x, float y, float radius, float thickness, uint32_t colour) {
    DSCommand *command = command_push(DS_CMD_RING);
    if (!command) return;
    command->value.ring.x = x;
    command->value.ring.y = y;
    command->value.ring.radius = radius;
    command->value.ring.thickness = thickness;
    command->value.ring.colour = pack_colour(colour);
}

void tex(float x, float y, const char *name, float angle, float scale) {
    DSCommand *command;
    Texture *texture;
    if (!frame_open) return;
    texture = load_png_texture(name);
    if (!texture) return;
    command = command_push(DS_CMD_TEXTURE);
    if (!command) return;
    command->value.texture.x = x;
    command->value.texture.y = y;
    command->value.texture.angle = angle;
    command->value.texture.scale = scale;
    command->value.texture.texture = texture;
}

static int utf8_next_graphics(const char **cursor) {
    const unsigned char *p = (const unsigned char *)*cursor;
    int result;
    if (!p || !*p) return -1;
    if (*p < 0x80) result = *p++;
    else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
        result = ((*p & 0x1f) << 6) | (p[1] & 0x3f); p += 2;
    } else if ((*p & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        result = ((*p & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); p += 3;
    } else if ((*p & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 &&
               (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        result = ((*p & 7) << 18) | ((p[1] & 0x3f) << 12) |
                 ((p[2] & 0x3f) << 6) | (p[3] & 0x3f); p += 4;
    } else result = *p++;
    *cursor = (const char *)p;
    return result;
}

static void blend_text_pixel(uint32_t *destination, uint32_t colour, unsigned int coverage) {
    *destination = blend_coverage(*destination, colour, coverage);
}

static void render_text_now(Buffer *buffer, const char *string, float x, float y,
                            uint32_t colour, float scale) {
    const char *cursor;
    float pen_x;
    float baseline;
    int atlas_width;
    int atlas_height;
    const unsigned char *atlas;
    if (!buffer || !font || !string || !isfinite(x) || !isfinite(y) ||
        !isfinite(scale) || scale <= 0.0f) return;
    atlas_width = ds_font_atlas_width(font);
    atlas_height = ds_font_atlas_height(font);
    atlas = ds_font_atlas_alpha(font);
    pen_x = x;
    baseline = y + ds_font_ascent(font) * scale;
    cursor = string;
    while (*cursor) {
        int codepoint = utf8_next_graphics(&cursor);
        const DSFontGlyph *glyph;
        int source_x;
        int source_y;
        int dest_width;
        int dest_height;
        int dest_x;
        int dest_y;
        int sy;
        if (codepoint == '\n') {
            pen_x = x;
            baseline += ds_font_line_height(font) * scale;
            continue;
        }
        glyph = ds_font_glyph(font, (uint32_t)codepoint);
        if (!glyph) continue;
        source_x = (int)floorf(glyph->u0 * atlas_width + 0.5f);
        source_y = (int)floorf(glyph->v0 * atlas_height + 0.5f);
        dest_width = (int)ceilf(glyph->width * scale);
        dest_height = (int)ceilf(glyph->height * scale);
        dest_x = (int)floorf(pen_x + glyph->bearing_x * scale);
        dest_y = (int)floorf(baseline - glyph->bearing_top * scale);
        for (sy = 0; sy < dest_height; ++sy) {
            int screen_y = dest_y + sy;
            int source_row;
            int sx;
            if (screen_y < 0 || screen_y >= buffer->height) continue;
            source_row = source_y + (int)(sy / scale);
            if (source_row < 0 || source_row >= atlas_height) continue;
            for (sx = 0; sx < dest_width; ++sx) {
                int screen_x = dest_x + sx;
                int source_column;
                unsigned int coverage;
                if (screen_x < 0 || screen_x >= buffer->width) continue;
                source_column = source_x + (int)(sx / scale);
                if (source_column < 0 || source_column >= atlas_width) continue;
                coverage = atlas[source_row * atlas_width + source_column];
                if (coverage) {
                    blend_text_pixel(buffer->pixels + screen_y * buffer->stride + screen_x,
                                     colour, coverage);
                }
            }
        }
        pen_x += glyph->advance * scale;
    }
}

void text(const char *string, float x, float y, uint32_t colour) {
    text_scaled(string, x, y, colour, 1.0f);
}

void text_scaled(const char *string, float x, float y, uint32_t colour, float scale) {
    DSCommand *command;
    if (!frame_open || !string || !ensure_font()) return;
    command = command_push(DS_CMD_TEXT);
    if (!command) return;
    command->value.text.string = string;
    command->value.text.x = x;
    command->value.text.y = y;
    command->value.text.scale = scale;
    command->value.text.colour = pack_colour(colour);
}

int text_width(const char *string) {
    if (!string || !ensure_font()) return 0;
    return (int)(ds_font_measure(font, string) + 0.5f);
}

int text_height(void) {
    if (!ensure_font()) return 32;
    return (int)(ds_font_line_height(font) + 0.5f);
}

static void render_texture_unrotated(Buffer *buffer, const Texture *texture,
                                     float x, float y, float scale) {
    float width;
    float height;
    int left;
    int top;
    int right;
    int bottom;
    int screen_y;
    if (!buffer || !texture || !texture->pixels || !isfinite(x) || !isfinite(y) ||
        !isfinite(scale) || scale <= 0.0f) return;
    width = texture->width * scale;
    height = texture->height * scale;
    if (x >= buffer->width || y >= buffer->height || x + width <= 0.0f || y + height <= 0.0f) return;
    left = clamp_floor(floorf(x), buffer->width);
    top = clamp_floor(floorf(y), buffer->height);
    right = clamp_ceil(ceilf(x + width), buffer->width);
    bottom = clamp_ceil(ceilf(y + height), buffer->height);

    if (scale == 1.0f && x == floorf(x) && y == floorf(y) && texture->opaque &&
        left == (int)x && top == (int)y) {
        int source_left = left - (int)x;
        int source_top = top - (int)y;
        for (screen_y = top; screen_y < bottom; ++screen_y) {
            int source_y = source_top + screen_y - top;
            int copy_width = right - left;
            if (source_y < 0 || source_y >= texture->height) continue;
            if (source_left < 0) {
                copy_width += source_left;
                source_left = 0;
            }
            if (source_left + copy_width > texture->width) copy_width = texture->width - source_left;
            if (copy_width > 0) {
                memcpy(buffer->pixels + screen_y * buffer->stride + left,
                       texture->pixels + source_y * texture->width + source_left,
                       (size_t)copy_width * sizeof(uint32_t));
            }
        }
        return;
    }

    for (screen_y = top; screen_y < bottom; ++screen_y) {
        int source_y = (int)(((float)screen_y + 0.5f - y) / scale);
        int screen_x;
        if (source_y < 0) source_y = 0;
        if (source_y >= texture->height) source_y = texture->height - 1;
        for (screen_x = left; screen_x < right; ++screen_x) {
            int source_x = (int)(((float)screen_x + 0.5f - x) / scale);
            uint32_t source;
            if (source_x < 0) source_x = 0;
            if (source_x >= texture->width) source_x = texture->width - 1;
            source = texture->pixels[source_y * texture->width + source_x];
            buffer->pixels[screen_y * buffer->stride + screen_x] =
                blend_pixel(buffer->pixels[screen_y * buffer->stride + screen_x], source);
        }
    }
}

static void render_texture_rotated(Buffer *buffer, const Texture *texture,
                                   float x, float y, float angle, float scale) {
    float cosine;
    float sine;
    float half_width;
    float half_height;
    float centre_x;
    float centre_y;
    float extent_x;
    float extent_y;
    int left;
    int top;
    int right;
    int bottom;
    int screen_y;
    if (!buffer || !texture || !texture->pixels || !isfinite(angle) || !isfinite(scale) || scale <= 0.0f) return;
    cosine = cosf(angle);
    sine = sinf(angle);
    half_width = texture->width * scale * 0.5f;
    half_height = texture->height * scale * 0.5f;
    centre_x = x + half_width;
    centre_y = y + half_height;
    extent_x = fabsf(cosine) * half_width + fabsf(sine) * half_height;
    extent_y = fabsf(sine) * half_width + fabsf(cosine) * half_height;
    if (!isfinite(extent_x) || !isfinite(extent_y) ||
        centre_x + extent_x <= 0.0f || centre_y + extent_y <= 0.0f ||
        centre_x - extent_x >= buffer->width || centre_y - extent_y >= buffer->height) return;
    left = clamp_floor(floorf(centre_x - extent_x), buffer->width);
    top = clamp_floor(floorf(centre_y - extent_y), buffer->height);
    right = clamp_ceil(ceilf(centre_x + extent_x), buffer->width);
    bottom = clamp_ceil(ceilf(centre_y + extent_y), buffer->height);
    for (screen_y = top; screen_y < bottom; ++screen_y) {
        int screen_x;
        for (screen_x = left; screen_x < right; ++screen_x) {
            float dx = screen_x + 0.5f - centre_x;
            float dy = screen_y + 0.5f - centre_y;
            float local_x = cosine * dx + sine * dy;
            float local_y = -sine * dx + cosine * dy;
            int source_x = (int)floorf(local_x / scale + texture->width * 0.5f);
            int source_y = (int)floorf(local_y / scale + texture->height * 0.5f);
            uint32_t source;
            if (source_x < 0 || source_x >= texture->width || source_y < 0 || source_y >= texture->height) continue;
            source = texture->pixels[source_y * texture->width + source_x];
            buffer->pixels[screen_y * buffer->stride + screen_x] =
                blend_pixel(buffer->pixels[screen_y * buffer->stride + screen_x], source);
        }
    }
}

static void render_texture(Buffer *buffer, const Texture *texture,
                           float x, float y, float angle, float scale) {
    if (!isfinite(angle) || fabsf(angle) < 0.00001f) {
        render_texture_unrotated(buffer, texture, x, y, scale);
    } else {
        render_texture_rotated(buffer, texture, x, y, angle, scale);
    }
}

static void flush_commands(void) {
    size_t i;
    if (!current_buffer) return;
    for (i = 0; i < command_count; ++i) {
        DSCommand *command = &commands[i];
        switch (command->type) {
            case DS_CMD_CLEAR:
                clear_buffer(current_buffer, command->value.clear.colour);
                break;
            case DS_CMD_RECT:
                render_rect(current_buffer, command->value.rect.x, command->value.rect.y,
                            command->value.rect.width, command->value.rect.height,
                            command->value.rect.colour);
                break;
            case DS_CMD_CIRCLE:
                render_circle(current_buffer, command->value.circle.x, command->value.circle.y,
                              command->value.circle.radius, command->value.circle.colour);
                break;
            case DS_CMD_RING:
                render_ring(current_buffer, command->value.ring.x, command->value.ring.y,
                            command->value.ring.radius, command->value.ring.thickness,
                            command->value.ring.colour);
                break;
            case DS_CMD_TEXTURE:
                render_texture(current_buffer, command->value.texture.texture,
                               command->value.texture.x, command->value.texture.y,
                               command->value.texture.angle, command->value.texture.scale);
                break;
            case DS_CMD_TEXT:
                render_text_now(current_buffer, command->value.text.string,
                                command->value.text.x, command->value.text.y,
                                command->value.text.colour, command->value.text.scale);
                break;
        }
    }
}

int ds_graphics_init(AAssetManager *assets) {
    if (asset_manager != assets) {
        ds_release_assets();
        asset_manager = assets;
    }
    return 1;
}

int ds_graphics_begin_frame(Buffer *buffer) {
    if (!buffer || !buffer->pixels || buffer->width <= 0 || buffer->height <= 0 ||
        buffer->stride < buffer->width) return 0;
    current_buffer = buffer;
    frame_open = 1;
    command_count = 0;
    /* A script may omit cls().  Clear once per frame so stale Android buffer
     * contents never leak through; a later cls command still preserves draw
     * order exactly. */
    clear_buffer(buffer, pack_colour(0x00000000u));
    return 1;
}

void ds_graphics_end_frame(void) {
    if (!frame_open) return;
    flush_commands();
    command_count = 0;
    frame_open = 0;
    current_buffer = NULL;
}

void ds_graphics_cancel_frame(void) {
    command_count = 0;
    frame_open = 0;
    current_buffer = NULL;
}

void ds_graphics_error_screen(const char *message) {
    if (!current_buffer) return;
    clear_buffer(current_buffer, pack_colour(0xff2e070b));
    if (font && message) render_text_now(current_buffer, message, 16.0f, 16.0f,
                                         pack_colour(0xffffffffu), 0.65f);
}

void ds_graphics_shutdown(void) {
    ds_graphics_cancel_frame();
    ds_release_assets();
    free(commands);
    commands = NULL;
    command_capacity = 0;
    command_count = 0;
}

/* When graphics.c is also listed by a modern build, main.c supplies the
 * strong embedded definitions and these standalone definitions are harmless. */
#ifndef DIMSCRIPT_GRAPHICS_EMBEDDED
#if defined(__GNUC__) || defined(__clang__)
#pragma weak ds_release_assets
#pragma weak ds_set_asset_manager
#pragma weak png_load
#pragma weak cls
#pragma weak rect
#pragma weak circle
#pragma weak ring
#pragma weak tex
#pragma weak text
#pragma weak text_scaled
#pragma weak text_width
#pragma weak text_height
#pragma weak ds_graphics_init
#pragma weak ds_graphics_begin_frame
#pragma weak ds_graphics_end_frame
#pragma weak ds_graphics_cancel_frame
#pragma weak ds_graphics_shutdown
#pragma weak ds_graphics_error_screen
#endif
#endif
