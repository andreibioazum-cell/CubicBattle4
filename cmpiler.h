#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdint.h>
#include <android/asset_manager.h>
#include <android/native_window.h>
#include <android/log.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// === Буфер ===
typedef struct { uint32_t* pixels; int width, height, stride; } Buffer;

// === Векторы (SIMD-оптимизированные) ===
typedef struct { float x, y; } Vec2;
typedef struct { float x, y, z; } Vec3;

#define V2(x,y) (Vec2){x,y}
#define V2A(a,b) (Vec2){a.x+b.x, a.y+b.y}
#define V2S(a,b) (Vec2){a.x-b.x, a.y-b.y}
#define V2M(a,s) (Vec2){a.x*s, a.y*s}
#define V2D(a,b) (a.x*b.x + a.y*b.y)
#define V2L(a) sqrtf(a.x*a.x + a.y*a.y)
#define V2N(a) ({float l=sqrtf(a.x*a.x+a.y*a.y); l>0?V2(a.x/l,a.y/l):V2(0,0);})
#define V2R(a,ang) ({float c=cosf(ang), s=sinf(ang); V2(a.x*c-a.y*s, a.x*s+a.y*c);})

// === Таблица (оптимизированная хеш-таблица) ===
#define TBL_SIZE 128
typedef struct Entry { struct Entry* next; uint32_t hash; char* key; void* val; int type; } Entry;
typedef struct { Entry* buckets[TBL_SIZE]; int count; } Table;

// === Значения ===
typedef struct { int type; union { double num; char* str; Table* table; void* func; Vec2 v2; Vec3 v3; }; } Val;

// === Глобальные переменные ===
extern Table* G;
extern Table* L;  // локальные

// === API таблиц ===
Table* T_new(void);
void T_set(Table* t, const char* key, void* val, int type);
void* T_get(Table* t, const char* key, int* type);
void T_del(Table* t, const char* key);

// === Встроенные функции ===
void print(Val v);
void printn(double n);
void prints(const char* s);
double tonumber(Val v);
const char* tostring(Val v);

// === Графика ===
void cls(uint32_t color);
void rect(float x, float y, float w, float h, uint32_t color);
void circle(float x, float y, float r, uint32_t color);
void ring(float x, float y, float r, float t, uint32_t color);
void tex(float x, float y, const char* name, float angle, float scale);
void text(const char* str, float x, float y, uint32_t color);

// === Джойстик ===
typedef struct { float x, y, dx, dy, ox, oy, r; } Joy;
extern Joy joy;

// === Система ===
extern int screen_w, screen_h;
extern double fps;

// === Хуки (переопределяются скомпилированным кодом) ===
void init(AAssetManager* assets);
void update(void);
void draw(Buffer* rb);
void touch(float x, float y, int action);

#endif
