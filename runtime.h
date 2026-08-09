#ifndef RUNTIME_H
#define RUNTIME_H

#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The software renderer receives the locked Android frame here.  Scripts do
 * not touch this pointer; graphics.c owns clipping, batching and rasterisation
 * so generated code only submits high-level primitives. */
typedef struct {
    uint32_t *pixels;
    int width;
    int height;
    int stride;
} Buffer;

typedef struct { float x, y; } Vec2;
typedef struct { float x, y, z; } Vec3;

#define V2(x, y)       (Vec2){(x), (y)}
#define V2A(a, b)      (Vec2){(a).x + (b).x, (a).y + (b).y}
#define V2S(a, b)      (Vec2){(a).x - (b).x, (a).y - (b).y}
#define V2M(a, s)      (Vec2){(a).x * (s), (a).y * (s)}
#define V2D(a, b)      ((a).x * (b).x + (a).y * (b).y)
#define V2L(a)         sqrtf((a).x * (a).x + (a).y * (a).y)
#define V2N(a)         ({ float _l = V2L(a); _l > 0 ? V2((a).x / _l, (a).y / _l) : V2(0, 0); })
#define V2R(a, angle)  ({ float _c = cosf(angle), _s = sinf(angle); V2((a).x * _c - (a).y * _s, (a).x * _s + (a).y * _c); })

enum {
    DS_NIL = 0,
    DS_NUMBER = 1,
    DS_STRING = 2,
    DS_TABLE = 3,
    DS_FUNCTION = 4,
    DS_VEC2 = 5,
    DS_VEC3 = 6
};

typedef struct Val Val;
typedef struct Entry Entry;
typedef struct Table Table;

struct Val {
    int type;
    union {
        double num;
        char *str;
        Table *table;
        void *func;
        Vec2 v2;
        Vec3 v3;
    };
};

/* Tables remain available for dynamic data, but generated script variables do
 * not use them.  The compiler emits typed C locals/globals, so a hot variable
 * is a direct load/store rather than a ds_read/T_get hash lookup. */
struct Entry {
    Entry *next;
    uint32_t hash;
    char *key;
    Val *value;
};

struct Table {
    Entry **buckets;
    size_t capacity;
    size_t count;
    uint64_t version;
};

typedef struct {
    Table *table;
    const char *key;
    uint32_t hash;
    uint64_t version;
    Val *value;
} DSLookupCache;

extern Table *G;
extern Table *L;
extern int screen_w, screen_h;
extern double fps;

typedef struct { float x, y, dx, dy, ox, oy, r; } Joy;
extern Joy joy;

void ds_log(const char *format, ...);

Table *T_new(void);
void T_free(Table *table);
int T_set(Table *table, const char *key, const void *value, int type);
Val *T_get(Table *table, const char *key, int *type);
Val *T_get_cached(Table *table, const char *key, DSLookupCache *cache, int *type);

/* Recoverable runtime errors.  The Android loop wraps every script hook in
 * ds_call_protected(); ds_runtime_error records a diagnostic and transfers
 * control to that boundary instead of letting a bad script continue into an
 * invalid state. */
typedef void (*DSProtectedFunction)(void *userdata);
int ds_call_protected(DSProtectedFunction function, void *userdata, const char *label);
void ds_runtime_error(const char *format, ...);
const char *ds_runtime_error_message(void);
int ds_script_has_error(void);
void ds_clear_runtime_error(void);
void ds_request_script_restart(void);
void ds_restart_script(void); /* script-facing alias: request a safe restart */
int ds_script_restart_requested(void);
void ds_clear_script_restart(void);

/* String expressions in generated C use real concatenation.  The result of
 * ds_concat is heap-owned and remains valid until the caller releases it (or
 * until the script is restarted, when the runtime pool is reset). */
int ds_len(const char *string);
char *ds_concat(const char *left, const char *right);
int ds_find(const char *haystack, const char *needle, int from);
int ds_contains(const char *haystack, const char *needle);
int ds_starts_with(const char *string, const char *prefix);
int ds_ends_with(const char *string, const char *suffix);
char *ds_num_to_string(double number);
char *ds_bool_to_string(int value);
void ds_string_release(char *string);
void ds_string_pool_reset(void);

void cls(uint32_t color);
void rect(float x, float y, float w, float h, uint32_t color);
void circle(float x, float y, float r, uint32_t color);
void ring(float x, float y, float r, float t, uint32_t color);

void ds_set_asset_manager(AAssetManager *assets);
void ds_release_assets(void);
int png_load(const char *name);
void tex(float x, float y, const char *name, float angle, float scale);
void text(const char *string, float x, float y, uint32_t color);
void text_scaled(const char *string, float x, float y, uint32_t color, float scale);
int text_width(const char *string);
int text_height(void);

/* Software frame lifecycle.  Scripts only call the drawing primitives; the
 * host locks/posts ANativeWindow and the renderer flushes its command list into
 * the supplied frame. */
int ds_graphics_init(AAssetManager *assets);
int ds_graphics_begin_frame(Buffer *buffer);
void ds_graphics_end_frame(void);
void ds_graphics_cancel_frame(void);
void ds_graphics_shutdown(void);
void ds_graphics_error_screen(const char *message);

void init(AAssetManager *assets);
void update(void);
void draw(Buffer *buffer);
void touch(float x, float y, int action);
void reset(void);

#endif
