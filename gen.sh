#!/bin/bash

cat > game.c << 'EOF'
#include "runtime.h"
#include <math.h>

void init(AAssetManager* assets) {
    G = T_new();
    T_set(G, "COLOR_BG", &(Val){.type=1,.num=0xFFCCCCCC}, 1);
    T_set(G, "COLOR_PLAYER", &(Val){.type=1,.num=0xFFEE7722}, 1);
    T_set(G, "COLOR_JOY", &(Val){.type=1,.num=0xFF000000}, 1);
    T_set(G, "COLOR_TEXT", &(Val){.type=1,.num=0xFFFFFFFF}, 1);
    
    Table* p = T_new();
    T_set(p, "x", &(Val){.type=1,.num=screen_w/2.0}, 1);
    T_set(p, "y", &(Val){.type=1,.num=screen_h/2.0}, 1);
    T_set(p, "angle", &(Val){.type=1,.num=0}, 1);
    T_set(p, "speed", &(Val){.type=1,.num=5}, 1);
    T_set(p, "size", &(Val){.type=1,.num=40}, 1);
    T_set(G, "player", p, 3);
    
    joy.x = 150;
    joy.y = screen_h - 150;
    joy.r = 80;
}

void update() {
    Val* pv = T_get(G, "player", NULL);
    Table* p = pv ? (Table*)pv->table : NULL;
    if (!p) return;
    
    Val* x = T_get(p, "x", NULL);
    Val* y = T_get(p, "y", NULL);
    Val* a = T_get(p, "angle", NULL);
    Val* s = T_get(p, "speed", NULL);
    Val* sz = T_get(p, "size", NULL);
    if (!x || !y || !a || !s || !sz) return;
    
    x->num += joy.dx * s->num;
    y->num += joy.dy * s->num;
    if (joy.dx != 0 || joy.dy != 0) a->num = atan2(joy.dx, -joy.dy);
    
    double mx = screen_w - sz->num;
    double my = screen_h - sz->num;
    if (x->num < sz->num) x->num = sz->num;
    if (x->num > mx) x->num = mx;
    if (y->num < sz->num) y->num = sz->num;
    if (y->num > my) y->num = my;
}

void draw(Buffer* rb) {
    cls(0xFFCCCCCC);
    
    Val* pv = T_get(G, "player", NULL);
    Table* p = pv ? (Table*)pv->table : NULL;
    if (p) {
        Val* x = T_get(p, "x", NULL);
        Val* y = T_get(p, "y", NULL);
        Val* sz = T_get(p, "size", NULL);
        if (x && y && sz) {
            float h = sz->num / 2;
            rect(x->num - h, y->num - h, sz->num, sz->num, 0xFFEE7722);
        }
    }
    
    ring(joy.x, joy.y, joy.r, 4, 0xFF000000);
    circle(joy.x + joy.ox, joy.y + joy.oy, 35, 0xFF000000);
    text("FPS: 60", 10, 10, 0xFFFFFFFF);
}

void touch(float x, float y, int action) {
    float dx = x - joy.x;
    float dy = y - joy.y;
    float dist = sqrt(dx*dx + dy*dy);
    
    if (action == 1) {
        joy.dx = 0; joy.dy = 0;
        joy.ox = 0; joy.oy = 0;
        return;
    }
    
    if (dist > joy.r + 30) return;
    if (dist < 15) {
        joy.dx = 0; joy.dy = 0;
        joy.ox = 0; joy.oy = 0;
        return;
    }
    
    float c = dist > joy.r ? joy.r : dist;
    joy.dx = dx / dist;
    joy.dy = dy / dist;
    joy.ox = joy.dx * c;
    joy.oy = joy.dy * c;
}
EOF

echo "game.c generated!"
