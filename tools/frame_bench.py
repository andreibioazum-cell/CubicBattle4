#!/usr/bin/env python3
"""Measures a battle frame on a host: commands, geometry and stage timings.

Why: a frame rate report gives no clue about where the time goes. The tool runs
the real battle scripts (game/game.c) on real geometry
(native/graphics/geometry.inc) and the real font, but instead of Vulkan it
counts what the GPU would receive: the commands a frame draws, how many of them
become vertices and triangles, and how long each stage takes (update, draw,
command to geometry, batching into draw calls).

Run from the repository root:

    python3 gen.py
    python3 tools/frame_bench.py [frames] [width] [height]

Defaults are 600 frames at 2400x1080, the player's phone in landscape. The
summary prints average and peak milliseconds per stage, average and peak counts
of commands, vertices, triangles and draw calls, and warns when a frame hits the
uint16 index limit of 65535 vertices, where part of the scene is silently
dropped.
"""
from __future__ import annotations

import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import ui_preview  # noqa: E402  (reuses its autostubs and STUBS)

CC = shlex.split(__import__("os").environ.get("CC", "cc"))

LAYER = r"""
/* ================= frame command recording ================= */
#include "native/graphics/types.inc"
#include "native/graphics/geometry.inc"
#include "ttf_font.c"

/* Assets are read from game/assets, the way AAssetManager does on a device. */
struct AAsset { FILE *fp; long len; };
static int dummy_amgr_storage;
AAsset *AAssetManager_open(AAssetManager *mgr, const char *name, int mode) {
    (void)mgr; (void)mode;
    static char path[512];
    snprintf(path, sizeof path, "game/assets/%s", name);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    AAsset *a = (AAsset *)calloc(1, sizeof *a);
    if (!a) { fclose(fp); return NULL; }
    a->fp = fp;
    fseek(fp, 0, SEEK_END); a->len = ftell(fp); fseek(fp, 0, SEEK_SET);
    return a;
}
off_t AAsset_getLength(AAsset *a) { return a ? (off_t)a->len : 0; }
int AAsset_read(AAsset *a, void *buf, size_t n) { return a ? (int)fread(buf, 1, n, a->fp) : -1; }
int AAsset_close(AAsset *a) { if (!a) return 0; fclose(a->fp); free(a); return 0; }

static Buffer g_buf;
void ds_set_asset_manager(AAssetManager *a) { amgr = a ? a : (AAssetManager *)&dummy_amgr_storage; }

/* ====== copied from native/graphics/lifecycle.inc ======
 * Only the asset source changes (files in the repository) and the deferred GPU
 * upload: a texture counts as uploaded right away, because the frame the player
 * sees is the steady state and not the first one. */
static void ds_vk_pending_texture(Texture *t) { (void)t; }

static Texture *find_tx(const char *n) {
    for (Texture *t = textures; t; t = t->next) if (strcmp(t->name, n) == 0) return t;
    return NULL;
}
static const char *norm_name(const char *n) {
    if (!n) return NULL;
    while (strncmp(n, "./", 2) == 0) n += 2;
    if (strncmp(n, "game/assets/", 12) == 0) n += 12;
    else if (strncmp(n, "assets/", 7) == 0) n += 7;
    if (!*n || *n == '/' || strchr(n, '\\')) return NULL;
    for (const char *c = n; *c;) {
        const char *s = c; while (*c && *c != '/') c++;
        if (c-s == 2 && s[0]=='.' && s[1]=='.') return NULL;
        if (*c == '/') c++;
    }
    return n;
}
static int open_asset(const char *n, uint8_t **out, size_t *sz) {
    if (!n || !out || !sz || !amgr) return 0;
    AAsset *a = AAssetManager_open(amgr, n, AASSET_MODE_BUFFER);
    if (!a) return 0;
    off_t len = AAsset_getLength(a);
    if (len <= 0 || (uint64_t)len > SIZE_MAX) { AAsset_close(a); return 0; }
    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (!buf) { AAsset_close(a); return 0; }
    size_t off = 0;
    while (off < (size_t)len) {
        int nr = AAsset_read(a, buf+off, (size_t)len-off);
        if (nr <= 0) break; off += nr;
    }
    AAsset_close(a);
    if (off != (size_t)len) { free(buf); return 0; }
    *out = buf; *sz = (size_t)len; return 1;
}
static Texture *load_png(const char *req) {
    const char *n = norm_name(req);
    if (!n) return NULL;
    Texture *t = find_tx(n);
    if (t) return t->pixels ? t : NULL;
    if (!amgr) return NULL;
    t = (Texture *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->name = strdup_safe(n);
    if (!t->name) { free(t); return NULL; }
    t->next = textures; textures = t;
    uint8_t *enc = NULL; size_t enc_sz = 0;
    if (!open_asset(n, &enc, &enc_sz) || enc_sz > (size_t)INT_MAX) { free(enc); return NULL; }
    int ch = 0;
    stbi_uc *dec = stbi_load_from_memory(enc, (int)enc_sz, &t->w, &t->h, &ch, STBI_rgb_alpha);
    free(enc);
    if (!dec || t->w <= 0 || t->h <= 0) { stbi_image_free(dec); return NULL; }
    size_t pc = (size_t)t->w * t->h;
    t->pixels = (uint32_t *)malloc(pc * sizeof(*t->pixels));
    if (!t->pixels) { stbi_image_free(dec); return NULL; }
    t->opaque = 1;
    for (size_t i = 0; i < pc; i++) {
        uint8_t a = dec[i*4+3];
        t->pixels[i] = dec[i*4] | ((uint32_t)dec[i*4+1] << 8) | ((uint32_t)dec[i*4+2] << 16) | ((uint32_t)a << 24);
        if (a != 255) t->opaque = 0;
    }
    stbi_image_free(dec);
    /* On a device a texture is on the GPU after the first frame, and geometry is
     * built only for uploaded textures (t->gpu.uploaded && t->gpu.desc). */
    t->gpu.uploaded = 1;
    t->gpu.desc = 1;
    return t;
}
static int ensure_font(void) {
    if (font) return 1;
    if (font_tried) return 0;
    font_tried = 1;
    uint8_t *data = NULL; size_t sz = 0;
    if (!open_asset(DS_FONT_ASSET, &data, &sz)) return 0;
    font = ds_font_create(data, sz, DS_FONT_PIXEL_HEIGHT);
    free(data);
    return font != NULL;
}

static DSCmd *push(DSCommand t) {
    if (!frame_open) return NULL;
    if (cmd_n == cmd_cap) {
        size_t cap = cmd_cap ? cmd_cap*2 : 256;
        if (cap < cmd_n || cap > SIZE_MAX/sizeof(*cmds)) return NULL;
        DSCmd *nc = (DSCmd *)realloc(cmds, cap*sizeof(*cmds));
        if (!nc) return NULL;
        cmds = nc; cmd_cap = cap;
    }
    DSCmd *c = &cmds[cmd_n++];
    memset(c, 0, sizeof(*c)); c->t = t; return c;
}
void rect(float x, float y, float w, float h, uint32_t c) {
    DSCmd *p = push(DS_CMD_RECT); if (!p) return;
    p->v.rc.x=x; p->v.rc.y=y; p->v.rc.w=w; p->v.rc.h=h; p->v.rc.c=pack_c(c);
}
void clear_screen(uint32_t c) { rect(0.0f, 0.0f, (float)screen_w, (float)screen_h, c); }
void roundrect(float x, float y, float w, float h, float r, uint32_t c) {
    DSCmd *p = push(DS_CMD_ROUND); if (!p) return;
    p->v.rr.x=x; p->v.rr.y=y; p->v.rr.w=w; p->v.rr.h=h; p->v.rr.r=r; p->v.rr.c=pack_c(c);
}
void rect_rot(float x, float y, float w, float h, float ang, uint32_t c) {
    DSCmd *p = push(DS_CMD_RECT_ROT); if (!p) return;
    p->v.rot.x=x; p->v.rot.y=y; p->v.rot.w=w; p->v.rot.h=h; p->v.rot.ang=ang; p->v.rot.c=pack_c(c);
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
static uint32_t text_force_white(uint32_t c) {
    if (c == 0xFFFF4444u || c == 0xFF4FC3F7u || c == 0xFFFF3333u || c == 0xFF33A8FFu) return c;
    if ((c & 0x00ffffffu) == 0x00202020u) return c;
    return (c & 0xff000000u) | 0x00ffffffu;
}
void text_scaled(const char *s, float x, float y, uint32_t c, float sc) {
    if (!frame_open || !s || !ensure_font()) return;
    DSCmd *p = push(DS_CMD_TEXT); if (!p) return;
    p->v.tt.s = strdup_safe(s);
    if (!p->v.tt.s) return;
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
        if (db > base) db = base;
        if (first || dt < minT) minT = dt;
        if (first || db > maxB) maxB = db;
        first = 0;
    }
    return first ? 0 : (int)(maxB - minT + 0.5f);
}
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
int text_width(const char *s) { return text_ink_width(s); }
int text_height(const char *s) { return text_ink_height(s); }

/* ====== command to geometry, a copy of ds_vk_build_geometry ======
 * (native/graphics/vulkan_record.inc) plus the batching of ds_vk_record_draws,
 * without Vulkan and with counters only. */
typedef struct { int pipeline; const void *desc; size_t first; size_t count; } BenchBatch;
static BenchBatch *b_batches;
static size_t b_n, b_cap;
static size_t b_draw_calls, b_merged_verts, b_fail_verts;
static size_t b_cmds_by_type[16];

static int bench_batch_push(int pipeline, const void *desc, size_t first, size_t count) {
    if (count == 0) return 1;
    if (b_n == b_cap) {
        size_t cap = b_cap ? b_cap * 2 : 256;
        BenchBatch *nb = (BenchBatch *)realloc(b_batches, cap * sizeof(*nb));
        if (!nb) return 0;
        b_batches = nb; b_cap = cap;
    }
    BenchBatch *b = &b_batches[b_n++];
    b->pipeline = pipeline; b->desc = desc;
    b->first = first; b->count = count;
    return 1;
}

static int bench_build_geometry(void) {
    geo_reset();
    b_n = 0;
    memset(b_cmds_by_type, 0, sizeof(b_cmds_by_type));
    const void *font_desc = font ? (const void *)1 : NULL;
    for (size_t i = 0; i < cmd_n; i++) {
        DSCmd *c = &cmds[i];
        b_cmds_by_type[c->t < 16 ? c->t : 0]++;
        int pipeline = 0;
        const void *desc = NULL;
        size_t is = geo_in;
        size_t v_before = geo_vn;
        switch (c->t) {
            case DS_CMD_RECT:
                geo_rect(c->v.rc.x, c->v.rc.y, c->v.rc.w, c->v.rc.h, c->v.rc.c);
                break;
            case DS_CMD_ROUND:
                pipeline = 0; desc = NULL;
                geo_roundrect(c->v.rr.x, c->v.rr.y, c->v.rr.w, c->v.rr.h, c->v.rr.r, c->v.rr.c);
                break;
            case DS_CMD_RECT_ROT:
                geo_rect_rot(c->v.rot.x, c->v.rot.y, c->v.rot.w, c->v.rot.h,
                             c->v.rot.ang, c->v.rot.c);
                break;
            case DS_CMD_CIRCLE:
                geo_circle(c->v.ci.x, c->v.ci.y, c->v.ci.r, c->v.ci.c);
                break;
            case DS_CMD_RING:
                geo_ring(c->v.rg.x, c->v.rg.y, c->v.rg.r, c->v.rg.th, c->v.rg.c);
                break;
            case DS_CMD_LINE:
                geo_line(c->v.ln.x1, c->v.ln.y1, c->v.ln.x2, c->v.ln.y2, c->v.ln.th, c->v.ln.c);
                break;
            case DS_CMD_TEX: {
                Texture *t = c->v.tx.tx;
                if (t && t->gpu.uploaded && t->gpu.desc) {
                    pipeline = 1;
                    desc = (const void *)(uintptr_t)t->gpu.desc;
                    geo_tex(c->v.tx.x, c->v.tx.y, c->v.tx.a, c->v.tx.sc,
                            (float)t->w, (float)t->h, 0xffffffffu);
                }
                break;
            }
            case DS_CMD_TEX_TINT: {
                Texture *t = c->v.tx2.tx;
                if (t && t->gpu.uploaded && t->gpu.desc) {
                    pipeline = 2;
                    desc = (const void *)(uintptr_t)t->gpu.desc;
                    geo_tex(c->v.tx2.x, c->v.tx2.y, c->v.tx2.a, c->v.tx2.sc,
                            (float)t->w, (float)t->h, c->v.tx2.c);
                }
                break;
            }
            case DS_CMD_TEXT:
                if (font && font_desc) {
                    pipeline = 1;
                    desc = font_desc;
                    geo_text(c->v.tt.s, c->v.tt.x, c->v.tt.y, c->v.tt.c, c->v.tt.sc, font);
                }
                break;
        }
        /* The uint16 index limit: extra vertices are dropped silently by
         * geo_push_vert, which shows up here as incomplete geometry. */
        if (geo_vn == 65535 && geo_in == is) b_fail_verts++;
        (void)v_before;
        if (!bench_batch_push(pipeline, desc, is, geo_in - is)) return 0;
    }
    /* Merges runs of batches sharing a pipeline and a texture. */
    b_draw_calls = 0; b_merged_verts = 0;
    size_t i = 0;
    while (i < b_n) {
        int pipe = b_batches[i].pipeline;
        const void *desc = (pipe == 1 || pipe == 2) ? b_batches[i].desc : NULL;
        size_t count = 0;
        size_t j = i;
        while (j < b_n && b_batches[j].pipeline == pipe &&
               (desc == NULL || b_batches[j].desc == desc)) {
            count += b_batches[j].count;
            j++;
        }
        b_draw_calls++;
        b_merged_verts += count;
        i = j;
    }
    return 1;
}
"""

MAIN = r"""
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

typedef struct { double min, max, sum; } Stat;
static void stat_add(Stat *s, double v) {
    if (s->sum == 0 && s->min == 0 && s->max == 0) { s->min = v; s->max = v; }
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
    s->sum += v;
}

static size_t stat_max(size_t *a, int n) { size_t m = 0; for (int i = 0; i < n; i++) if (a[i] > m) m = a[i]; return m; }

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 600;
    int W = argc > 2 ? atoi(argv[2]) : 2400;
    int H = argc > 3 ? atoi(argv[3]) : 1080;
    if (frames < 1) frames = 600;
    screen_w = W; screen_h = H;
    amgr = (AAssetManager *)&dummy_amgr_storage;
    ds_main();
    ds_fn_init();
    language = 1; show_hitboxes = 1; music_volume = 70;
    winter_theme = 1; show_fps = 1;
    ds_fn_apply_winter_theme();
    ds_fn_set_class_owned(CLASS_AZUM, 1);
    ds_fn_set_class_owned(CLASS_SANTA, 1);
    ds_fn_set_class_owned(CLASS_EBUC, 1);
    candies = 320; cups = 1200;
    player_class = CLASS_AZUM;
    ds_fn_sync_selected_class();

    game_state = ST_SOLO;
    azum_skin = SKIN_NORMAL;
    ds_fn_init_game();
    dt = 1.0 / 60.0;

    /* Warm up: textures, font, the first battle frames. */
    for (int i = 0; i < 60; i++) {
        ds_fn_update();
        frame_open = 1; cmd_n = 0;
        ds_fn_draw();
        bench_build_geometry();
        for (size_t k = 0; k < cmd_n; k++) if (cmds[k].t == DS_CMD_TEXT) free(cmds[k].v.tt.s);
        frame_open = 0;
    }

    Stat s_upd = {0}, s_draw = {0}, s_geo = {0}, s_total = {0};
    Stat s_cmds = {0}, s_verts = {0}, s_tris = {0}, s_calls = {0}, s_batches = {0};
    size_t peak_verts = 0, peak_cmds = 0, peak_calls = 0;
    size_t cap_hits = 0;
    size_t joy_id = 0;
    (void)joy_id;

    for (int f = 0; f < frames; f++) {
        dt = 1.0 / 60.0;
        /* Input: the joystick walks in a circle and a punch lands every 40
         * frames, so the scene stays alive like in a real match. */
        double ang = (double)f * 0.07;
        ds_fn_touch((float)(joy.x + cos(ang) * 70.0), (float)(joy.y + sin(ang) * 70.0), 0, 1);
        if (f % 40 == 0) ds_fn_touch((float)atk_x, (float)atk_y, 0, 2);
        if (f % 40 == 2) ds_fn_touch((float)atk_x, (float)atk_y, 1, 2);
        if (f % 90 == 0) ds_fn_touch((float)joy.x, (float)joy.y, 0, 3);

        double t0 = now_ms();
        ds_fn_update();
        double t1 = now_ms();

        frame_open = 1; cmd_n = 0;
        ds_fn_draw();
        double t2 = now_ms();

        bench_build_geometry();
        double t3 = now_ms();

        if (geo_vn >= 65535) cap_hits++;

        stat_add(&s_upd, t1 - t0);
        stat_add(&s_draw, t2 - t1);
        stat_add(&s_geo, t3 - t2);
        stat_add(&s_total, t3 - t0);
        stat_add(&s_cmds, (double)cmd_n);
        stat_add(&s_verts, (double)geo_vn);
        stat_add(&s_tris, (double)geo_in);
        stat_add(&s_calls, (double)b_draw_calls);
        stat_add(&s_batches, (double)b_n);
        if (geo_vn > peak_verts) peak_verts = geo_vn;
        if (cmd_n > peak_cmds) peak_cmds = cmd_n;
        if (b_draw_calls > peak_calls) peak_calls = b_draw_calls;

        for (size_t k = 0; k < cmd_n; k++) if (cmds[k].t == DS_CMD_TEXT) free(cmds[k].v.tt.s);
        frame_open = 0;
    }

    printf("Бой на хосте: %d кадров, окно %dx%d, хитбоксы вкл.\n", frames, W, H);
    printf("Время этапов, мс на кадр (среднее / максимум):\n");
    printf("  update (скрипты):   %.3f / %.3f\n", s_upd.sum / frames, s_upd.max);
    printf("  draw   (команды):   %.3f / %.3f\n", s_draw.sum / frames, s_draw.max);
    printf("  геометрия + пачки:  %.3f / %.3f\n", s_geo.sum / frames, s_geo.max);
    printf("  итого CPU:          %.3f / %.3f\n", s_total.sum / frames, s_total.max);
    printf("Наполнение кадра (среднее, пик):\n");
    printf("  команд:             %.0f, пик %zu\n", s_cmds.sum / frames, peak_cmds);
    printf("  вершин:             %.0f, пик %zu\n", s_verts.sum / frames, peak_verts);
    printf("  треугольников:      %.0f\n", s_tris.sum / frames);
    printf("  draw call'ов:       %.1f (пик %zu), пачек до слияния: %.1f\n",
           s_calls.sum / frames, peak_calls, s_batches.sum / frames);
    printf("  команд по типам (последний кадр): rect %zu, round %zu, circle %zu, ring %zu, "
           "line %zu, tex %zu, text %zu, tint %zu, rect_rot %zu\n",
           b_cmds_by_type[DS_CMD_RECT], b_cmds_by_type[DS_CMD_ROUND], b_cmds_by_type[DS_CMD_CIRCLE],
           b_cmds_by_type[DS_CMD_RING], b_cmds_by_type[DS_CMD_LINE], b_cmds_by_type[DS_CMD_TEX],
           b_cmds_by_type[DS_CMD_TEXT], b_cmds_by_type[DS_CMD_TEX_TINT], b_cmds_by_type[DS_CMD_RECT_ROT]);
    if (peak_verts >= 65535)
        printf("ВНИМАНИЕ: кадр упирается в предел индексов uint16 (65535 вершин) - "
               "часть геометрии молча теряется (%zu кадров).\n", cap_hits);
    return 0;
}
"""


def compile_bench(temp: Path) -> Path:
    src = temp / "bench.c"
    stubs = temp / "stubs.c"
    binary = temp / "frame_bench"
    src.write_text(ui_preview.STUBS + LAYER + MAIN, encoding="utf-8")
    stubs.write_text("#include <stdarg.h>\n#include <stdio.h>\n#include <string.h>\n", encoding="utf-8")
    protos = ui_preview.prototypes()
    cmd = [*CC, "-std=gnu99", "-O2", "-I", str(ROOT), "-I", str(ROOT / "game"),
           "-I", str(ROOT / "tools" / "host_test" / "stub"),
           str(src), str(stubs), "-lm", "-o", str(binary)]
    done: set[str] = set()
    for _ in range(10):
        run = subprocess.run(cmd, capture_output=True, text=True)
        missing = sorted(set(__import__("re").findall(r"undefined reference to `(\w+)'", run.stderr)))
        if not missing:
            if run.returncode == 0:
                return binary
            sys.exit("benchmark build failed:\n" + run.stderr)
        lines = ["/* Autostubs: signatures from runtime.h and net.h, neutral bodies. */",
                 "#include <stdarg.h>", "#include <stdio.h>"]
        unknown = []
        for name in missing:
            if name in done:
                continue
            if name in ui_preview.VAR_DECLS:
                lines.append(ui_preview.VAR_DECLS[name])
            elif name in protos:
                lines.append(ui_preview.stub_source(name, *protos[name]))
            else:
                unknown.append(name)
                continue
            done.add(name)
        if unknown:
            sys.exit("no prototypes for these stubs: " + ", ".join(unknown))
        stubs.write_text("\n".join(lines) + "\n", encoding="utf-8")
    sys.exit("could not link the benchmark in 10 passes")


def main(argv: list[str]) -> int:
    args = argv[1:]
    game_c = ROOT / "game" / "game.c"
    if not game_c.exists():
        sys.exit("game/game.c is missing, run python3 gen.py first")
    with tempfile.TemporaryDirectory(prefix="frame-bench-") as td:
        binary = compile_bench(Path(td))
        run = subprocess.run([str(binary), *args], capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
