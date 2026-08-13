#ifndef RUNTIME_H
#define RUNTIME_H

// Только Android 10, arm64/arm32 — без кроссплатформы
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t *pixels;
    int width;
    int height;
    int stride;
} Buffer;

extern int screen_w, screen_h;
extern double dt;

typedef struct { float x, y, dx, dy, ox, oy, r; } Joy;
extern Joy joy;

void ds_log(const char *format, ...);
/* ds_log_err — некритичная ошибка (например «текстура не загрузилась»):
 * пишется в консоль красным и в logcat как ERROR, но НЕ останавливает скрипт. */
void ds_log_err(const char *format, ...);
/* ds_console_log — запись в консоль из любых потоков (сетевых и т.д.) */
void ds_console_log(int is_error, const char *format, ...);
typedef void (*DSProtectedFunction)(void *userdata);
int ds_call_protected(DSProtectedFunction function, void *userdata, const char *label);
void ds_runtime_error(const char *format, ...);
const char *ds_runtime_error_message(void);
int ds_script_has_error(void);
void ds_clear_runtime_error(void);
void ds_request_script_restart(void);
int ds_script_restart_requested(void);
void ds_clear_script_restart(void);

char *ds_concat(const char *left, const char *right);
char *ds_num_to_string(double number);
void ds_string_pool_reset(void);

/* --- Консоль: кольцевой буфер лога/ошибок, виден в настройках --- */
int console_count(void);
const char *console_line(int index);
int console_type(int index);   /* 1 = ошибка, 0 = обычный лог */
void console_clear(void);

/* --- Типы для расширенной стандартной библиотеки --- */
typedef struct DSArray DSArray;
typedef struct DSDict DSDict;
typedef struct DSTimer DSTimer;

DSArray* arr_new(void);
void arr_push(DSArray* a, double v);
double arr_pop(DSArray* a);
double arr_get(DSArray* a, double idx);
void arr_set(DSArray* a, double idx, double v);
double arr_len(DSArray* a);
void arr_clear(DSArray* a);
void arr_free(DSArray* a);

DSDict* dict_new(void);
void dict_set(DSDict* d, const char* key, double val);
double dict_get(DSDict* d, const char* key);
int dict_has(DSDict* d, const char* key);
void dict_del(DSDict* d, const char* key);
void dict_free(DSDict* d);

DSTimer* timer_new(void);
void timer_start(DSTimer* t);
double timer_elapsed(DSTimer* t);
void timer_reset(DSTimer* t);
void timer_free(DSTimer* t);

const char* file_read(const char* path);
int file_write(const char* path, const char* content);
int file_exists(const char* path);
int file_del(const char* path);

const char* json_get_str(const char* json, const char* path);
double json_get_num(const char* json, const char* path);
int json_get_bool(const char* json, const char* path);

const char* http_get(const char* url);
const char* http_post(const char* url, const char* body);

double clamp(double v, double lo, double hi);
double lerp(double a, double b, double t);
double dist(double x1, double y1, double x2, double y2);
double now(void);
double str_len(const char *s);
int str_eq(const char *a, const char *b);

/* клавиатура через JNI */
void ds_set_activity(void *activity);
void keyboard_show(void);
void keyboard_hide(void);
const char* keyboard_get_text(void);
const char* keyboard_get_raw(void);
void keyboard_clear(void);
int keyboard_visible(void);
int keyboard_enter_pressed(void);
int keyboard_handle_key(int keycode, int action);
void keyboard_type(const char *text);   /* дописать строку в буфер клавиатуры */
void keyboard_backspace(void);          /* стереть последний символ */

/* графика */
void rect(float x, float y, float w, float h, uint32_t color);
void roundrect(float x, float y, float w, float h, float r, uint32_t color);
void circle(float x, float y, float r, uint32_t color);
void ring(float x, float y, float r, float t, uint32_t color);
void line(float x1, float y1, float x2, float y2, float thickness, uint32_t color);

void ds_set_asset_manager(AAssetManager *assets);
void ds_release_assets(void);
int png_load(const char *name);
void tex(float x, float y, const char *name, float angle, float scale);
void text(const char *string, float x, float y, uint32_t color);
void text_scaled(const char *string, float x, float y, uint32_t color, float scale);
int text_ink_width(const char *string);
int text_ink_height(const char *string);
int text_ink_top(const char *string);

int ds_graphics_init(AAssetManager *assets);
int ds_graphics_begin_frame(Buffer *buffer);
void ds_graphics_end_frame(void);
void ds_graphics_cancel_frame(void);
void ds_graphics_shutdown(void);
void ds_graphics_error_screen(const char *message);

void init(AAssetManager *assets);
void update(void);
void draw(Buffer *buffer);
void touch(float x, float y, int action, int pointer_id);
void reset(void);

#endif
