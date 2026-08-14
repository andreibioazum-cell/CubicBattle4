#include "runtime.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"
#ifdef STB_IMAGE_STATIC
#undef STB_IMAGE_STATIC
#endif

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Texture Texture;
typedef struct DSFont DSFont;
typedef struct { uint32_t codepoint; float advance, bearing_x, bearing_top; int width, height; float u0, v0, u1, v1; } DSFontGlyph;
struct Texture { Texture *next; char *name; int w, h, opaque; uint32_t *pixels; };

typedef enum { DS_CMD_RECT, DS_CMD_ROUND, DS_CMD_CIRCLE, DS_CMD_RING, DS_CMD_LINE, DS_CMD_TEX, DS_CMD_TEXT } DCCmd;
typedef struct {
    DCCmd t;
    union {
        struct { float x, y, w, h; uint32_t c; } rc;
        struct { float x, y, w, h, r; uint32_t c; } rr;
        struct { float x, y, r; uint32_t c; } ci;
        struct { float x, y, r, th; uint32_t c; } rg;
        struct { float x1, y1, x2, y2, th; uint32_t c; } ln;
        struct { float x, y, a, sc; Texture *tx; } tx;
        struct { const char *s; float x, y, sc; uint32_t c; } tt;
    } v;
} DC;

static uint32_t pack_c(uint32_t c) {
    uint32_t a = (c >> 24) & 0xff, r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    return r | (g << 8) | (b << 16) | ((a ? a : 255) << 24);
}
static uint32_t blend(uint32_t d, uint32_t s) {
    uint32_t a = (s >> 24) & 0xff;
    if (!a) return d; if (a == 255) return s;
    uint32_t inv = 255 - a;
    return (((s & 0xff)*a + (d & 0xff)*inv + 127) / 255) |
           (((((s >> 8) & 0xff)*a + ((d >> 8) & 0xff)*inv + 127) / 255) << 8) |
           (((((s >> 16) & 0xff)*a + ((d >> 16) & 0xff)*inv + 127) / 255) << 16) |
           ((a + ((((d >> 24) & 0xff)*inv + 127) / 255)) << 24);
}
static int cl_floor(float v, int lim) { int r = (int)floorf(v); return r < 0 ? 0 : (r > lim ? lim : r); }
static int cl_ceil(float v, int lim) { int r = (int)ceilf(v); return r < 0 ? 0 : (r > lim ? lim : r); }

static void fill_span(uint32_t *d, int n, uint32_t c) {
    while (n >= 4) { d[0] = d[1] = d[2] = d[3] = c; d += 4; n -= 4; }
    while (n-- > 0) *d++ = c;
}
static void paint_span(uint32_t *d, int n, uint32_t c) {
    if ((c >> 24) >= 255) { fill_span(d, n, c); return; }
    while (n-- > 0) { *d = blend(*d, c); d++; }
}

static void render_rect(Buffer *b, float x, float y, float w, float h, uint32_t c) {
    if (!b || w <= 0 || h <= 0) return;
    int l = cl_floor(x, b->width), t = cl_floor(y, b->height), r = cl_ceil(x + w, b->width), bo = cl_ceil(y + h, b->height);
    for (int row = t; row < bo; row++) {
        if ((c >> 24) >= 255) fill_span(b->pixels + row * b->stride + l, r - l, c);
        else { uint32_t *d = b->pixels + row * b->stride + l; for (int col = 0; col < r - l; col++) d[col] = blend(d[col], c); }
    }
}
static void render_roundrect(Buffer *b, float x, float y, float w, float h, float rad, uint32_t c) {
    if (!b || w <= 0 || h <= 0) return;
    if (rad > w * 0.5f) rad = w * 0.5f; if (rad > h * 0.5f) rad = h * 0.5f;
    int t0 = cl_floor(y, b->height), t1 = cl_ceil(y + h, b->height);
    for (int row = t0; row < t1; row++) {
        float yi = (float)row + 0.5f - y, ins = 0;
        if (rad > 0) {
            if (yi < rad) { float d = rad - yi; ins = rad - sqrtf(rad * rad - d * d); }
            else if (yi > h - rad) { float d = yi - (h - rad); ins = rad - sqrtf(rad * rad - d * d); }
        }
        int s = (int)ceilf(x + ins), e = (int)ceilf(x + w - ins);
        if (s < 0) s = 0; if (e > b->width) e = b->width;
        if (e > s) fill_span(b->pixels + row * b->stride + s, e - s, c);
    }
}
static void render_circle(Buffer *b, float x, float y, float rad, uint32_t c) {
    if (!b || rad <= 0) return;
    int r = (int)ceilf(rad), cx = (int)floorf(x + 0.5f), cy = (int)floorf(y + 0.5f);
    long long r2 = (long long)r * r;
    for (int dy = -r; dy <= r; dy++) {
        int sy = cy + dy; if (sy < 0 || sy >= b->height) continue;
        int hw = (int)sqrt((double)(r2 - (long long)dy * dy)), l = cx - hw, rr = cx + hw + 1;
        if (l < 0) l = 0; if (rr > b->width) rr = b->width;
        if (l < rr) paint_span(b->pixels + sy * b->stride + l, rr - l, c);
    }
}
static void render_ring(Buffer *b, float x, float y, float rad, float th, uint32_t c) {
    if (!b || rad <= 0 || th <= 0) return;
    int out = (int)ceilf(rad), in = (int)floorf(rad - th);
    if (in <= 0) { render_circle(b, x, y, rad, c); return; }
    int cx = (int)floorf(x + 0.5f), cy = (int)floorf(y + 0.5f);
    long long o2 = (long long)out * out, i2 = (long long)in * in;
    for (int dy = -out; dy <= out; dy++) {
        int sy = cy + dy; if (sy < 0 || sy >= b->height) continue;
        int oh = (int)sqrt((double)(o2 - (long long)dy * dy)), ih = (abs(dy) <= in) ? (int)sqrt((double)(i2 - (long long)dy * dy)) : -1;
        int l = cx - oh, rr = cx + oh + 1; if (l < 0) l = 0; if (rr > b->width) rr = b->width;
        if (ih < 0) { if (l < rr) paint_span(b->pixels + sy * b->stride + l, rr - l, c); }
        else {
            int il = cx - ih, ir = cx + ih + 1, lr = il < rr ? il : rr, rl = ir > l ? ir : l;
            if (l < lr) paint_span(b->pixels + sy * b->stride + l, lr - l, c);
            if (rl < rr) paint_span(b->pixels + sy * b->stride + rl, rr - rl, c);
        }
    }
}
static void render_line(Buffer *b, float x1, float y1, float x2, float y2, float th, uint32_t c) {
    if (!b || th <= 0) return;
    float dx = x2 - x1, dy = y2 - y1, len2 = dx * dx + dy * dy, rad = th * 0.5f, rad2 = rad * rad;
    if (len2 <= 0.0001f) { render_circle(b, x1, y1, rad, c); return; }
    int left = cl_floor(fminf(x1, x2) - rad, b->width), right = cl_ceil(fmaxf(x1, x2) + rad, b->width);
    int top = cl_floor(fminf(y1, y2) - rad, b->height), bottom = cl_ceil(fmaxf(y1, y2) + rad, b->height);
    for (int py = top; py < bottom; py++) {
        float fy = (float)py + 0.5f;
        for (int px = left; px < right; px++) {
            float fx = (float)px + 0.5f, u = ((fx - x1)*dx + (fy - y1)*dy) / len2;
            if (u < 0 || u > 1) continue;
            float ox = x1 + u*dx - fx, oy = y1 + u*dy - fy;
            if (ox*ox + oy*oy <= rad2) b->pixels[py * b->stride + px] = blend(b->pixels[py * b->stride + px], c);
        }
    }
}

static Buffer *cur_buf;
static DC *cmds;
static size_t cmd_n, cmd_cap;
static int frame_open;
static AAssetManager *amgr;
static Texture *textures;
static DSFont *font;
static int font_tried;

DSFont *ds_font_create(const uint8_t *data, size_t size, int ph);
void ds_font_destroy(DSFont *font);
const DSFontGlyph *ds_font_glyph(const DSFont *font, uint32_t cp);
int ds_font_aw(const DSFont *font);
int ds_font_ah(const DSFont *font);
const uint8_t *ds_font_alpha(const DSFont *font);
float ds_font_lineh(const DSFont *font);
float ds_font_ascent(const DSFont *font);

static int open_asset(const char *n, uint8_t **out, size_t *sz) {
#ifdef __ANDROID__
    if (!amgr || !out || !sz) return 0;
    AAsset *a = AAssetManager_open(amgr, n, AASSET_MODE_BUFFER); if (!a) return 0;
    off_t len = AAsset_getLength(a); if (len <= 0) { AAsset_close(a); return 0; }
    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (!buf) { AAsset_close(a); return 0; }
    AAsset_read(a, buf, (size_t)len); AAsset_close(a);
    *out = buf; *sz = (size_t)len; return 1;
#else
    if (!n || !out || !sz) return 0;
    FILE *f = fopen(n, "rb");
    if (!f) { char b[512]; snprintf(b, sizeof(b), "game/assets/%s", n); f = fopen(b, "rb"); if (!f) { snprintf(b, sizeof(b), "assets/%s", n); f = fopen(b, "rb"); } }
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return 0; }
    uint8_t *buf = (uint8_t *)malloc((size_t)len); if (!buf) { fclose(f); return 0; }
    fread(buf, 1, (size_t)len, f); fclose(f);
    *out = buf; *sz = (size_t)len; return 1;
#endif
}

static Texture *load_png(const char *req) {
    if (!req) return NULL;
    const char *n = req;
    if (strncmp(n, "game/assets/", 12) == 0) n += 12; else if (strncmp(n, "assets/", 7) == 0) n += 7;
    for (Texture *t = textures; t; t = t->next) if (strcmp(t->name, n) == 0) return t;
    uint8_t *enc = NULL; size_t enc_sz = 0;
    if (!open_asset(n, &enc, &enc_sz)) return NULL;
    int w, h, ch;
    stbi_uc *dec = stbi_load_from_memory(enc, (int)enc_sz, &w, &h, &ch, STBI_rgb_alpha); free(enc);
    if (!dec) return NULL;
    Texture *t = (Texture *)calloc(1, sizeof(*t));
    t->name = strdup(n); t->w = w; t->h = h; t->pixels = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t a = dec[i*4+3]; t->pixels[i] = dec[i*4] | (dec[i*4+1] << 8) | (dec[i*4+2] << 16) | (a << 24);
        if (a != 255) t->opaque = 0;
    }
    stbi_image_free(dec); t->next = textures; textures = t;
    return t;
}

static int ensure_font(void) {
    if (font) return 1; if (font_tried) return 0; font_tried = 1;
    uint8_t *data = NULL; size_t sz = 0;
    if (!open_asset("fonts/ChillRoundGothic_Heavy.ttf", &data, &sz)) return 0;
    font = ds_font_create(data, sz, 32); free(data);
    return font != NULL;
}

static int utf8_dec(const char **c) {
    const uint8_t *p = (const uint8_t *)*c; int r;
    if (!p || !*p) return -1;
    if (*p < 0x80) r = *p++;
    else if ((*p&0xe0)==0xc0 && (p[1]&0xc0)==0x80) { r = ((*p&0x1f)<<6)|(p[1]&0x3f); p += 2; }
    else if ((*p&0xf0)==0xe0) { r = ((*p&0x0f)<<12)|((p[1]&0x3f)<<6)|(p[2]&0x3f); p += 3; }
    else { r = ((*p&7)<<18)|((p[1]&0x3f)<<12)|((p[2]&0x3f)<<6)|(p[3]&0x3f); p += 4; }
    *c = (const char *)p; return r;
}

static void render_text_now(Buffer *b, const char *s, float x, float y, uint32_t c, float sc) {
    if (!b || !font || !s || sc <= 0) return;
    int aw = ds_font_aw(font), ah = ds_font_ah(font); const uint8_t *al = ds_font_alpha(font);
    const DSFontGlyph *ref = ds_font_glyph(font, 'S');
    float pen = x - (ref ? ref->bearing_x : 0)*sc, base = y + (ref ? ref->bearing_top : 0)*sc;
    for (const char *cur = s; *cur;) {
        int cp = utf8_dec(&cur); if (cp == '\n') { pen = x; base += ds_font_lineh(font)*sc; continue; }
        const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp); if (!g) continue;
        int sx = (int)floorf(g->u0*aw + 0.5f), sy = (int)floorf(g->v0*ah + 0.5f);
        int dw = (int)ceilf(g->width*sc), dh = (int)ceilf(g->height*sc);
        int dx = (int)floorf(pen + g->bearing_x*sc), dy = (int)floorf(base - g->bearing_top*sc);
        for (int yy = 0; yy < dh; yy++) {
            int scr_y = dy + yy; if (scr_y < 0 || scr_y >= b->height) continue;
            int srow = sy + (int)(yy / sc); if (srow >= ah) continue;
            for (int xx = 0; xx < dw; xx++) {
                int scr_x = dx + xx; if (scr_x < 0 || scr_x >= b->width) continue;
                int scol = sx + (int)(xx / sc); if (scol >= aw) continue;
                uint8_t cov = al[srow*aw + scol];
                if (cov) b->pixels[scr_y*b->stride + scr_x] = blend(b->pixels[scr_y*b->stride + scr_x], (c & 0xffffff) | (((uint32_t)cov * (c>>24) / 255) << 24));
            }
        }
        pen += g->advance * sc;
    }
}

static void draw_tx(Buffer *b, const Texture *t, float x, float y, float a, float sc) {
    if (!b || !t || !t->pixels || sc <= 0) return;
    if (fabsf(a) < 0.0005f) {
        int l = cl_floor(x, b->width), t0 = cl_floor(y, b->height), r = cl_ceil(x + t->w*sc, b->width), bo = cl_ceil(y + t->h*sc, b->height);
        for (int sy = t0; sy < bo; sy++) {
            int src_y = (int)(((float)sy + 0.5f - y) / sc); if (src_y >= t->h) src_y = t->h - 1;
            for (int sx = l; sx < r; sx++) {
                int src_x = (int)(((float)sx + 0.5f - x) / sc); if (src_x >= t->w) src_x = t->w - 1;
                b->pixels[sy*b->stride + sx] = blend(b->pixels[sy*b->stride + sx], t->pixels[src_y*t->w + src_x]);
            }
        }
    } else {
        float hw = t->w*0.5f*sc, hh = t->h*0.5f*sc, cx = x+hw, cy = y+hh, ca = cosf(a), sa = sinf(a);
        float dx = fabsf(hw*ca) + fabsf(hh*sa), dy = fabsf(hw*sa) + fabsf(hh*ca);
        int l = cl_floor(cx-dx, b->width), t0 = cl_floor(cy-dy, b->height), r = cl_ceil(cx+dx, b->width), bo = cl_ceil(cy+dy, b->height);
        for (int sy = t0; sy < bo; sy++) {
            float py = (float)sy + 0.5f - cy;
            for (int sx = l; sx < r; sx++) {
                float px = (float)sx + 0.5f - cx;
                int tx = (int)floorf((px*ca + py*sa)/sc + t->w*0.5f), ty = (int)floorf((-px*sa + py*ca)/sc + t->h*0.5f);
                if (tx >= 0 && tx < t->w && ty >= 0 && ty < t->h) b->pixels[sy*b->stride + sx] = blend(b->pixels[sy*b->stride + sx], t->pixels[ty*t->w + tx]);
            }
        }
    }
}

static DC *push(DCCmd t) {
    if (!frame_open) return NULL;
    if (cmd_n == cmd_cap) {
        cmd_cap = cmd_cap ? cmd_cap * 2 : 256;
        cmds = (DC *)realloc(cmds, cmd_cap * sizeof(*cmds));
    }
    DC *c = &cmds[cmd_n++]; memset(c, 0, sizeof(*c)); c->t = t; return c;
}

static void flush(void) {
    if (!cur_buf) return;
    for (size_t i = 0; i < cmd_n; i++) {
        DC *c = &cmds[i];
        switch (c->t) {
            case DS_CMD_RECT:   render_rect(cur_buf, c->v.rc.x, c->v.rc.y, c->v.rc.w, c->v.rc.h, c->v.rc.c); break;
            case DS_CMD_ROUND:  render_roundrect(cur_buf, c->v.rr.x, c->v.rr.y, c->v.rr.w, c->v.rr.h, c->v.rr.r, c->v.rr.c); break;
            case DS_CMD_CIRCLE: render_circle(cur_buf, c->v.ci.x, c->v.ci.y, c->v.ci.r, c->v.ci.c); break;
            case DS_CMD_RING:   render_ring(cur_buf, c->v.rg.x, c->v.rg.y, c->v.rg.r, c->v.rg.th, c->v.rg.c); break;
            case DS_CMD_LINE:   render_line(cur_buf, c->v.ln.x1, c->v.ln.y1, c->v.ln.x2, c->v.ln.y2, c->v.ln.th, c->v.ln.c); break;
            case DS_CMD_TEX:    draw_tx(cur_buf, c->v.tx.tx, c->v.tx.x, c->v.tx.y, c->v.tx.a, c->v.tx.sc); break;
            case DS_CMD_TEXT:   render_text_now(cur_buf, c->v.tt.s, c->v.tt.x, c->v.tt.y, c->v.tt.c, c->v.tt.sc); break;
        }
    }
}

void ds_release_assets(void) {
    for (Texture *t = textures; t;) { Texture *n = t->next; free(t->pixels); free(t->name); free(t); t = n; }
    textures = NULL; ds_font_destroy(font); font = NULL; font_tried = 0; amgr = NULL;
}
void ds_set_asset_manager(AAssetManager *a) { if (amgr != a) { ds_release_assets(); amgr = a; } }
int png_load(const char *n) { return load_png(n) != NULL; }

void rect(float x, float y, float w, float h, uint32_t c) { DC *p = push(DS_CMD_RECT); if (p) { p->v.rc.x=x; p->v.rc.y=y; p->v.rc.w=w; p->v.rc.h=h; p->v.rc.c=pack_c(c); } }
void roundrect(float x, float y, float w, float h, float r, uint32_t c) { DC *p = push(DS_CMD_ROUND); if (p) { p->v.rr.x=x; p->v.rr.y=y; p->v.rr.w=w; p->v.rr.h=h; p->v.rr.r=r; p->v.rr.c=pack_c(c); } }
void circle(float x, float y, float r, uint32_t c) { DC *p = push(DS_CMD_CIRCLE); if (p) { p->v.ci.x=x; p->v.ci.y=y; p->v.ci.r=r; p->v.ci.c=pack_c(c); } }
void ring(float x, float y, float r, float t, uint32_t c) { DC *p = push(DS_CMD_RING); if (p) { p->v.rg.x=x; p->v.rg.y=y; p->v.rg.r=r; p->v.rg.th=t; p->v.rg.c=pack_c(c); } }
void line(float x1, float y1, float x2, float y2, float thickness, uint32_t c) { DC *p = push(DS_CMD_LINE); if (p) { p->v.ln.x1=x1; p->v.ln.y1=y1; p->v.ln.x2=x2; p->v.ln.y2=y2; p->v.ln.th=thickness; p->v.ln.c=pack_c(c); } }
void tex(float x, float y, const char *name, float a, float s) { if (frame_open) { Texture *t = load_png(name); if (t) { DC *p = push(DS_CMD_TEX); if (p) { p->v.tx.x=x; p->v.tx.y=y; p->v.tx.a=a; p->v.tx.sc=s; p->v.tx.tx=t; } } } }
void text_scaled(const char *s, float x, float y, uint32_t c, float sc) { if (frame_open && s && ensure_font()) { DC *p = push(DS_CMD_TEXT); if (p) { p->v.tt.s=s; p->v.tt.x=x; p->v.tt.y=y; p->v.tt.sc=sc; p->v.tt.c=pack_c(c); } } }
void text(const char *s, float x, float y, uint32_t c) { text_scaled(s, x, y, c, 1.0f); }

int text_ink_width(const char *s) {
    if (!s || !ensure_font()) return 0;
    const DSFontGlyph *ref = ds_font_glyph(font, 'S'); float pen = -(ref ? ref->bearing_x : 0), minL = 0, maxR = 0; int first = 1;
    for (const char *c = s; *c;) {
        int cp = utf8_dec(&c); const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp); if (!g) continue;
        float dl = pen + g->bearing_x, dr = dl + g->width;
        if (first || dl < minL) minL = dl; if (first || dr > maxR) maxR = dr;
        first = 0; pen += g->advance;
    }
    return first ? 0 : (int)(maxR - minL + 0.5f);
}
int text_ink_height(const char *s) {
    if (!s || !ensure_font()) return 0;
    const DSFontGlyph *ref = ds_font_glyph(font, 'S'); float base = ref ? ref->bearing_top : 0, minT = 0, maxB = 0; int first = 1;
    for (const char *c = s; *c;) {
        int cp = utf8_dec(&c); const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp); if (!g) continue;
        float dt = base - g->bearing_top, db = dt + g->height;
        if (first || dt < minT) minT = dt; if (first || db > maxB) maxB = db;
        first = 0;
    }
    return first ? 0 : (int)(maxB - minT + 0.5f);
}
int text_ink_top(const char *s) {
    if (!s || !ensure_font()) return 0;
    const DSFontGlyph *ref = ds_font_glyph(font, 'S'); float base = ref ? ref->bearing_top : 0, minT = 0; int first = 1;
    for (const char *c = s; *c;) {
        int cp = utf8_dec(&c); const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp); if (!g) continue;
        float dt = base - g->bearing_top; if (first || dt < minT) minT = dt; first = 0;
    }
    return first ? 0 : (int)floorf(minT);
}

int ds_graphics_init(AAssetManager *a) { if (amgr != a) { ds_release_assets(); amgr = a; } return 1; }
int ds_graphics_begin_frame(Buffer *b) {
    if (!b || !b->pixels || b->width <= 0) return 0;
    cur_buf = b; frame_open = 1; cmd_n = 0;
    memset(b->pixels, 0, (size_t)b->height * b->stride * sizeof(uint32_t));
    return 1;
}
void ds_graphics_end_frame(void) { if (frame_open) { flush(); cmd_n = frame_open = 0; cur_buf = NULL; } }
void ds_graphics_cancel_frame(void) { cmd_n = frame_open = 0; cur_buf = NULL; }
void ds_graphics_error_screen(const char *m) {
    if (!cur_buf || !font) return;
    memset(cur_buf->pixels, 0x1c, (size_t)cur_buf->height * cur_buf->stride * sizeof(uint32_t));
    render_text_now(cur_buf, "=== ERROR ===", 16, 12, 0xffffffff, 0.7f);
    if (m) render_text_now(cur_buf, m, 16, 42, 0xffffb0b0, 0.6f);
}
void ds_graphics_shutdown(void) { ds_graphics_cancel_frame(); ds_release_assets(); free(cmds); cmds = NULL; cmd_cap = cmd_n = 0; }

#include "ttf_font.c"
