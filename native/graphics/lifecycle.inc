    float hw = t->w*0.5f*sc, hh = t->h*0.5f*sc;
    float cx = x+hw, cy = y+hh, ca = cosf(ang), sa = sinf(ang);
    float dx = fabsf(hw*ca) + fabsf(hh*sa), dy = fabsf(hw*sa) + fabsf(hh*ca);
    int l = cl_floor(floorf(cx-dx), b->width), t0 = cl_floor(floorf(cy-dy), b->height);
    int r = cl_ceil(ceilf(cx+dx), b->width),   bo = cl_ceil(ceilf(cy+dy), b->height);
    uint32_t tr = tint & 0xff, tg = (tint >> 8) & 0xff, tb = (tint >> 16) & 0xff, ta = (tint >> 24) & 0xff;
    for (int sy = t0; sy < bo; sy++) {
        float py = (float)sy + 0.5f - cy;
        for (int sx = l; sx < r; sx++) {
            float px = (float)sx + 0.5f - cx;
            int tx = (int)floorf((px*ca + py*sa)/sc + t->w*0.5f);
            int ty = (int)floorf((-px*sa + py*ca)/sc + t->h*0.5f);
            if (tx < 0 || tx >= t->w || ty < 0 || ty >= t->h) continue;
            uint32_t p = t->pixels[ty*t->w + tx];
            uint32_t sa = (p >> 24) & 0xff;
            if (!sa) continue;
            uint32_t a = (sa * ta) / 255;
            if (!a) continue;
            uint32_t s = tr | (tg << 8) | (tb << 16) | (a << 24);
            b->pixels[sy*b->stride + sx] = blend(b->pixels[sy*b->stride + sx], s);
        }
    }
}
static void draw_tx_tint(Buffer *b, const Texture *t, float x, float y, float a, float sc, uint32_t tint) {
    if (!b || !t || !t->pixels || !isfinite(x+y+a+sc) || sc <= 0) return;
    if (fabsf(a) < 0.0005f) { draw_tx_unrot_tint(b, t, x, y, sc, tint); return; }
    draw_tx_rot_tint(b, t, x, y, a, sc, tint);
}
static DSCmd *push(DSCommand t) {
    if (!frame_open) return NULL;
    if (cmd_n == cmd_cap) {
        size_t cap = cmd_cap ? cmd_cap*2 : 256;
        if (cap < cmd_n || cap > SIZE_MAX/sizeof(*cmds)) { ds_runtime_error("too many renderer commands"); return NULL; }
        DSCmd *nc = (DSCmd *)realloc(cmds, cap*sizeof(*cmds));
        if (!nc) { ds_runtime_error("out of memory in command buffer"); return NULL; }
        cmds = nc; cmd_cap = cap;
    }
    DSCmd *c = &cmds[cmd_n++];
    memset(c, 0, sizeof(*c)); c->t = t; return c;
}
static void flush(void) {
    if (!cur_buf) return;
    for (size_t i = 0; i < cmd_n; i++) {
        DSCmd *c = &cmds[i];
        switch (c->t) {
            case DS_CMD_RECT:  render_rect(cur_buf, c->v.rc.x, c->v.rc.y, c->v.rc.w, c->v.rc.h, c->v.rc.c); break;
            case DS_CMD_ROUND: render_roundrect(cur_buf, c->v.rr.x, c->v.rr.y, c->v.rr.w, c->v.rr.h, c->v.rr.r, c->v.rr.c); break;
            case DS_CMD_CIRCLE: render_circle(cur_buf, c->v.ci.x, c->v.ci.y, c->v.ci.r, c->v.ci.c); break;
            case DS_CMD_RING:   render_ring(cur_buf, c->v.rg.x, c->v.rg.y, c->v.rg.r, c->v.rg.th, c->v.rg.c); break;
            case DS_CMD_LINE:   render_line(cur_buf, c->v.ln.x1, c->v.ln.y1, c->v.ln.x2, c->v.ln.y2, c->v.ln.th, c->v.ln.c); break;
            case DS_CMD_TEX:    draw_tx(cur_buf, c->v.tx.tx, c->v.tx.x, c->v.tx.y, c->v.tx.a, c->v.tx.sc); break;
            case DS_CMD_TEX_TINT: draw_tx_tint(cur_buf, c->v.tx2.tx, c->v.tx2.x, c->v.tx2.y, c->v.tx2.a, c->v.tx2.sc, c->v.tx2.c); break;
            case DS_CMD_TEXT:
                render_text_now(cur_buf, c->v.tt.s, c->v.tt.x, c->v.tt.y, c->v.tt.c, c->v.tt.sc);
                free(c->v.tt.s); c->v.tt.s = NULL;
                break;
        }
    }
}
static void discard_commands(void) {
    for (size_t i = 0; i < cmd_n; i++) {
        if (cmds[i].t == DS_CMD_TEXT) {
            free(cmds[i].v.tt.s);
            cmds[i].v.tt.s = NULL;
        }
    }
    cmd_n = 0;
}
void ds_release_assets(void) {
    Texture *t = textures;
    while (t) { Texture *n = t->next; free(t->pixels); free(t->name); free(t); t = n; }
    textures = NULL;
    ds_font_destroy(font); font = NULL; font_tried = 0;
    amgr = NULL;
}
void ds_set_asset_manager(AAssetManager *a) {
    if (amgr != a) { ds_release_assets(); amgr = a; }
    if (!amgr) ds_runtime_error("Android asset manager is unavailable");
}
int png_load(const char *n) { return load_png(n) != NULL; }
void rect(float x, float y, float w, float h, uint32_t c) {
    DSCmd *p = push(DS_CMD_RECT); if (!p) return;
    p->v.rc.x=x; p->v.rc.y=y; p->v.rc.w=w; p->v.rc.h=h; p->v.rc.c=pack_c(c);
}
void clear_screen(uint32_t c) {
    rect(0.0f, 0.0f, (float)screen_w, (float)screen_h, c);
}
void roundrect(float x, float y, float w, float h, float r, uint32_t c) {
    DSCmd *p = push(DS_CMD_ROUND); if (!p) return;
    p->v.rr.x=x; p->v.rr.y=y; p->v.rr.w=w; p->v.rr.h=h; p->v.rr.r=r; p->v.rr.c=pack_c(c);
}
void circle(float x, float y, float r, uint32_t c) {
    DSCmd *p = push(DS_CMD_CIRCLE); if (!p) return;
    p->v.ci.x=x; p->v.ci.y=y; p->v.ci.r=r; p->v.ci.c=pack_c(c);
}
void ring(float x, float y, float r, float t, uint32_t c) {
    DSCmd *p = push(DS_CMD_RING); if (!p) return;
    p->v.rg.x=x; p->v.rg.y=y; p->v.rg.r=r; p->v.rg.th=t; p->v.rg.c=pack_c(c);
}
void line(float x1, float y1, float x2, float y2, float thickness, uint32_t c) {
    DSCmd *p = push(DS_CMD_LINE); if (!p) return;
    p->v.ln.x1=x1; p->v.ln.y1=y1; p->v.ln.x2=x2; p->v.ln.y2=y2;
    p->v.ln.th=thickness; p->v.ln.c=pack_c(c);
}
void tex(float x, float y, const char *name, float a, float s) {
    if (!frame_open) return;
    Texture *t = load_png(name); if (!t) return;
    DSCmd *p = push(DS_CMD_TEX); if (!p) return;
    p->v.tx.x=x; p->v.tx.y=y; p->v.tx.a=a; p->v.tx.sc=s; p->v.tx.tx=t;
}
void tex_tint(float x, float y, const char *name, float a, float s, uint32_t c) {
    if (!frame_open) return;
    Texture *t = load_png(name); if (!t) return;
    DSCmd *p = push(DS_CMD_TEX_TINT); if (!p) return;
    p->v.tx2.x=x; p->v.tx2.y=y; p->v.tx2.a=a; p->v.tx2.sc=s; p->v.tx2.tx=t; p->v.tx2.c=pack_c(c);
}
/* Весь текст в игре рисуется чисто белым (255,255,255). Скрипты по-прежнему
 * передают цвет, но из него остаётся только альфа — она нужна плавным
 * затуханиям подписей (fade_color в core/ui.ds, админский баннер, экран
 * предупреждения). RGB подменяется на белый, поэтому ни золотые/зелёные
 * надписи достижений, ни красные/голубые ники админов на экране не
 * появляются: одно правило на всю игру вместо правки каждого вызова. */
static uint32_t text_force_white(uint32_t c) {
    // Preserve admin nickname colors (red for Dimasi4ek229, blue for qwetyuiopaj1234)
    if (c == 0xFFFF4444u || c == 0xFF4FC3F7u || c == 0xFFFF3333u || c == 0xFF33A8FFu)
        return c;
    return (c & 0xff000000u) | 0x00ffffffu;
}
void text_scaled(const char *s, float x, float y, uint32_t c, float sc) {
    if (!frame_open || !s || !ensure_font()) return;
    DSCmd *p = push(DS_CMD_TEXT); if (!p) return;
    /* Network and script strings can come from reused buffers; renderer commands
     * must own text until the frame is flushed or cancelled. */
    p->v.tt.s = strdup_safe(s);
    if (!p->v.tt.s) { ds_runtime_error("out of memory copying renderer text"); return; }
    p->v.tt.x=x; p->v.tt.y=y; p->v.tt.sc=sc; p->v.tt.c=pack_c(text_force_white(c));
}
void text(const char *s, float x, float y, uint32_t c) { text_scaled(s, x, y, c, 1.0f); }
int text_ink_width(const char *s) {
    if (!s || !ensure_font()) return 0;
    const DSFontGlyph *ref = ds_font_glyph(font, 'S');
    float pen = -(ref ? ref->bearing_x : 0);
    int first = 1; float minL = 0, maxR = 0;
    for (const char *c = s; *c;) {
        int cp = utf8_dec(&c);
        const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp);
        if (!g) continue;
        float dl = pen + g->bearing_x, dr = dl + g->width;
        if (first || dl < minL) minL = dl;
        if (first || dr > maxR) maxR = dr;
        first = 0; pen += g->advance;
    }
    return first ? 0 : (int)(maxR - minL + 0.5f);
}
int text_ink_height(const char *s) {
    if (!s || !ensure_font()) return 0;
    const DSFontGlyph *ref = ds_font_glyph(font, 'S');
    float base = ref ? ref->bearing_top : 0;
    int first = 1; float minT = 0, maxB = 0;
    for (const char *c = s; *c;) {
        int cp = utf8_dec(&c);
        const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp);
        if (!g) continue;
        float dt = base - g->bearing_top, db = dt + g->height;
        /* Нижние выносные элементы ('р','у','д','g','y' и т.п.) не должны
         * раздувать высоту ink-бокса: иначе строка с ними центрируется выше
         * строк без них, и текст «уезжает вверх» на часть выносного элемента.
         * Обрезаем низ по базовой линии — она ровно в dt == base. */
        if (db > base) db = base;
        if (first || dt < minT) minT = dt;
        if (first || db > maxB) maxB = db;
        first = 0;
    }
    return first ? 0 : (int)(maxB - minT + 0.5f);
}
int text_width(const char *s) { return text_ink_width(s); }
int text_height(const char *s) { return text_ink_height(s); }
int text_ink_top(const char *s) {
    if (!s || !ensure_font()) return 0;
    const DSFontGlyph *ref = ds_font_glyph(font, 'S');
    float base = ref ? ref->bearing_top : 0;
    int first = 1; float minT = 0;
    for (const char *c = s; *c;) {
        int cp = utf8_dec(&c);
        const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp);
        if (!g) continue;
        float dt = base - g->bearing_top;
        if (first || dt < minT) minT = dt;
        first = 0;
    }
    return first ? 0 : (int)floorf(minT);
}
int ds_graphics_init(AAssetManager *a) { if (amgr != a) { ds_release_assets(); amgr = a; } return 1; }
int ds_graphics_begin_frame(Buffer *b) {
    if (!b || !b->pixels || b->width <= 0 || b->height <= 0 || b->stride < b->width) return 0;
    discard_commands(); cur_buf = b; frame_open = 1;
    clear_buf(b, pack_c(0x00000000));
    return 1;
}
void ds_graphics_end_frame(void) {
    if (!frame_open) return;
    flush(); cmd_n = 0; frame_open = 0; cur_buf = NULL;
}
void ds_graphics_cancel_frame(void) { discard_commands(); frame_open = 0; cur_buf = NULL; }
void ds_graphics_error_screen(const char *m) {
    if (!cur_buf) return;
    clear_buf(cur_buf, pack_c(0xff1c0b10));
    if (!font) return;
    int y = 12;
    render_text_now(cur_buf, "=== DIMSCRIPT ERROR ===", 16, y, pack_c(0xffffffff), 0.7f);
    y += 30;
    if (m && *m) {
        render_text_now(cur_buf, m, 16, y, pack_c(0xffffb0b0), 0.6f);
        y += 28;
    }
    y += 4;
    render_text_now(cur_buf, "--- console (last lines) ---", 16, y, pack_c(0xff9aa0b0), 0.55f);
    y += 24;
    int n = console_count();
    int start = n > 16 ? n - 16 : 0;
    for (int i = start; i < n; i++) {
        const char *line = console_line(i);
        if (!line || !*line) { y += 19; continue; }
        uint32_t col = console_type(i) ? pack_c(0xffff8a80) : pack_c(0xffc8d0dc);
        render_text_now(cur_buf, line, 16, y, col, 0.55f);
        y += 19;
        if (y > cur_buf->height - 8) break;
    }
}
void ds_graphics_shutdown(void) {
    ds_graphics_cancel_frame();
    ds_release_assets();
    free(cmds); cmds = NULL; cmd_cap = 0; cmd_n = 0;
}
#include "ttf_font.c"
