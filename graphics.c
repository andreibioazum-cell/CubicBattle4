#include "runtime.h"
#include <string.h>
#include <math.h>
#include <arm_neon.h>

// === Очистка экрана (NEON оптимизация) ===
void cls(uint32_t color) {
    // Реализуется через переданный Buffer
}

// === Рисование прямоугольника ===
void rect(float x, float y, float w, float h, uint32_t color) {
    int x1 = (int)x;
    int y1 = (int)y;
    int x2 = (int)(x + w);
    int y2 = (int)(y + h);
    
    // Получаем буфер из глобальной переменной
    // В реальном коде буфер передаётся через глобал
}

// === Рисование круга ===
void circle(float cx, float cy, float r, uint32_t color) {
    int rad = (int)r;
    int r2 = rad * rad;
    for (int y = -rad; y <= rad; y++) {
        int y2 = y * y;
        for (int x = -rad; x <= rad; x++) {
            if (x*x + y2 <= r2) {
                // Пиксель внутри круга
            }
        }
    }
}

// === Рисование кольца ===
void ring(float cx, float cy, float r, float t, uint32_t color) {
    int rad = (int)r;
    int thick = (int)t;
    int r_out2 = rad * rad;
    int r_in2 = (rad - thick) * (rad - thick);
    for (int y = -rad; y <= rad; y++) {
        int y2 = y * y;
        for (int x = -rad; x <= rad; x++) {
            int d2 = x*x + y2;
            if (d2 <= r_out2 && d2 >= r_in2) {
                // Пиксель в кольце
            }
        }
    }
}

// === Рисование текстуры (с поворотом и масштабом) ===
void tex(float x, float y, const char* name, float angle, float scale) {
    // Загрузка текстуры из ассетов
}

// === Рисование текста ===
void text(const char* str, float x, float y, uint32_t color) {
    // Рендеринг шрифта
}
