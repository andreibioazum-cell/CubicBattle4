#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdint.h>
#include <android/asset_manager.h>
#include <android/native_window.h>
#include <android/log.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "text.h"

/* The runtime deliberately keeps the value representation small and boring.
 * A table owns the Val stored in each entry.  This is important because the
 * compiler emits compound literals for constants; pointers to those literals
 * must not outlive the function in which they were created. */

typedef struct { uint32_t *pixels; int width, height, stride; } Buffer;

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

#define DS_TABLE_SIZE 128

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

struct Entry {
    Entry *next;
    uint32_t hash;
    char *key;
    Val *value;
};

struct Table {
    Entry *buckets[DS_TABLE_SIZE];
    int count;
};

extern Table *G;
extern Table *L;
extern int screen_w, screen_h;
extern double fps;

typedef struct { float x, y, dx, dy, ox, oy, r; } Joy;
extern Joy joy;

Table *T_new(void);
void T_free(Table *table);
int T_set(Table *table, const char *key, const void *value, int type);
Val *T_get(Table *table, const char *key, int *type);

/* Runtime failures go to logcat with the original message.  There is no
 * protected-call wrapper here: a script/runtime error must remain visible. */
void ds_runtime_error(const char *format, ...);

void print(Val value);
void printn(double number);
void prints(const char *string);
double tonumber(Val value);
const char *tostring(Val value);

void cls(uint32_t color);
void rect(float x, float y, float w, float h, uint32_t color);
void circle(float x, float y, float r, uint32_t color);
void ring(float x, float y, float r, float t, uint32_t color);

/* PNG files live under game/assets at build time and are addressed relative
 * to the APK asset root (for example, "player.png" or "sprites/enemy.png").
 * png_load is optional because tex also loads and caches on first use. */
void ds_set_asset_manager(AAssetManager *assets);
void ds_release_assets(void);
int png_load(const char *name);
void tex(float x, float y, const char *name, float angle, float scale);
void text(const char *string, float x, float y, uint32_t color);

void init(AAssetManager *assets);
void update(void);
void draw(Buffer *buffer);
void touch(float x, float y, int action);
const char *ds_str_cat(const char *prefix, double val);

#endif
