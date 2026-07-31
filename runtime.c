#include "runtime.h"

Table* G = NULL;
Table* L = NULL;
Joy joy = {0};
int screen_w = 0, screen_h = 0;
double fps = 0;

// === Хеш ===
static uint32_t hash(const char* s) {
    uint32_t h = 0;
    while (*s) h = h * 31 + *s++;
    return h;
}

// === Таблицы ===
Table* T_new() { return calloc(1, sizeof(Table)); }

void T_set(Table* t, const char* key, void* val, int type) {
    uint32_t h = hash(key) % TBL_SIZE;
    Entry* e = t->buckets[h];
    while (e) { if (strcmp(e->key, key)==0) { e->val=val; e->type=type; return; } e=e->next; }
    e = malloc(sizeof(Entry));
    e->key = strdup(key);
    e->hash = hash(key);
    e->val = val;
    e->type = type;
    e->next = t->buckets[h];
    t->buckets[h] = e;
    t->count++;
}

void* T_get(Table* t, const char* key, int* type) {
    uint32_t h = hash(key) % TBL_SIZE;
    Entry* e = t->buckets[h];
    while (e) { if (strcmp(e->key, key)==0) { if(type)*type=e->type; return e->val; } e=e->next; }
    if(type) *type = 0;
    return NULL;
}

void T_del(Table* t, const char* key) {
    uint32_t h = hash(key) % TBL_SIZE;
    Entry* e = t->buckets[h];
    Entry* prev = NULL;
    while (e) {
        if (strcmp(e->key, key)==0) {
            if (prev) prev->next = e->next;
            else t->buckets[h] = e->next;
            free(e->key); free(e);
            t->count--;
            return;
        }
        prev = e;
        e = e->next;
    }
}

// === Встроенные функции ===
void print(Val v) {
    switch(v.type) {
        case 1: __android_log_print(ANDROID_LOG_INFO, "DS", "%f", v.num); break;
        case 2: __android_log_print(ANDROID_LOG_INFO, "DS", "%s", v.str); break;
        case 5: __android_log_print(ANDROID_LOG_INFO, "DS", "v2(%.2f,%.2f)", v.v2.x, v.v2.y); break;
        case 6: __android_log_print(ANDROID_LOG_INFO, "DS", "v3(%.2f,%.2f,%.2f)", v.v3.x, v.v3.y, v.v3.z); break;
        default: __android_log_print(ANDROID_LOG_INFO, "DS", "nil"); break;
    }
}

void printn(double n) { __android_log_print(ANDROID_LOG_INFO, "DS", "%f", n); }
void prints(const char* s) { __android_log_print(ANDROID_LOG_INFO, "DS", "%s", s); }
double tonumber(Val v) { return v.type==1 ? v.num : 0; }
const char* tostring(Val v) { return v.type==2 ? v.str : ""; }

// === Графика (заглушки) ===
void cls(uint32_t color) {}
void rect(float x, float y, float w, float h, uint32_t color) {}
void circle(float x, float y, float r, uint32_t color) {}
void ring(float x, float y, float r, float t, uint32_t color) {}
void tex(float x, float y, const char* name, float angle, float scale) {}
void text(const char* str, float x, float y, uint32_t color) {}

// === Хуки ===
void init(AAssetManager* assets) { G = T_new(); }
void update() {}
void draw(Buffer* rb) {}
void touch(float x, float y, int action) {}
