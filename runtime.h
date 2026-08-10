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

typedef struct {
    uint32_t *pixels;
    int width;
    int height;
    int stride;
} Buffer;

extern int screen_w, screen_h;

/* Секунды с прошлого кадра (обновляет хост, ограничено 0.1 с).
 * Плавные анимации считайте через dt, а не через счётчики кадров:
 * скорость игрового цикла не фиксирована. */
extern double dt;

/* Джойстик: позиция, направление, смещение ручки и радиус. */
typedef struct { float x, y, dx, dy, ox, oy, r; } Joy;
extern Joy joy;

void ds_log(const char *format, ...);

/* Защищённый вызов хуков скрипта. При ошибке управление возвращается сюда,
 * а не в сломанный код. */
typedef void (*DSProtectedFunction)(void *userdata);
int ds_call_protected(DSProtectedFunction function, void *userdata, const char *label);
void ds_runtime_error(const char *format, ...);
const char *ds_runtime_error_message(void);
int ds_script_has_error(void);
void ds_clear_runtime_error(void);
void ds_request_script_restart(void);
int ds_script_restart_requested(void);
void ds_clear_script_restart(void);

int ds_len(const char *string);
char *ds_concat(const char *left, const char *right);
char *ds_num_to_string(double number);
char *ds_bool_to_string(int value);
void ds_string_pool_reset(void);

void rect(float x, float y, float w, float h, uint32_t color);
void roundrect(float x, float y, float w, float h, float r, uint32_t color);
void circle(float x, float y, float r, uint32_t color);
void ring(float x, float y, float r, float t, uint32_t color);
/* Толстая линия с альфа-смешиванием — нужна для траектории прицела. */
void line(float x1, float y1, float x2, float y2, float thickness, uint32_t color);

void ds_set_asset_manager(AAssetManager *assets);
void ds_release_assets(void);
int png_load(const char *name);
void tex(float x, float y, const char *name, float angle, float scale);
void text(const char *string, float x, float y, uint32_t color);
void text_scaled(const char *string, float x, float y, uint32_t color, float scale);
int text_ink_width(const char *string);
int text_ink_height(const char *string);

/* Звёзды фона лобби: полёт из верхнего-левого в правый-нижний угол. */
void ds_init_stars(int count, uint32_t color);
void ds_update_stars(void);
void ds_draw_stars(void);

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
