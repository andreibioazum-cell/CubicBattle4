/* Host equivalence test: the new tessellator (geometry.inc, with GPU triangle
 * rasterisation emulated) against the old per-pixel renderer
 * (legacy_raster.inc, a verbatim copy of the old native/graphics/raster.inc).
 *
 * Build and run it this way, from the repository root:
 *   gcc -std=gnu99 -O1 -o /tmp/test_geometry \
 *       tools/host_test/test_geometry.c tools/host_test/test_geo_side.c \
 *       -I tools/host_test/stub -I /tmp/vktools/Vulkan-Headers/include -I . -lm
 *   /tmp/test_geometry [dir_for_bmp]
 *
 * Checks: aligned rectangles must match pixel for pixel, while circles, rings,
 * lines, rounded rectangles and textures must match up to the edge pixels, since
 * the legacy renderer grew its edges outward to whole pixels and the GPU paints by
 * pixel centres. The text layout is checked numerically against the formula of the
 * old render_text_now. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* --- environment stubs of the legacy rasteriser --- */
#include "runtime.h"
void ds_log(const char *format, ...) { (void)format; }
void ds_log_err(const char *format, ...) { (void)format; }
void ds_console_log(int is_error, const char *format, ...) { (void)is_error; (void)format; }
void ds_runtime_error(const char *format, ...) { (void)format; }
const char *ds_runtime_error_message(void) { return ""; }
int console_count(void) { return 0; }
const char *console_line(int i) { (void)i; return ""; }
int console_type(int i) { (void)i; return 0; }

/* --- the old software rasteriser, verbatim --- */
#include "legacy_raster.inc"

/* The font API stubs live in test_geo_side.c; this unit never calls them. */

/* --- wrappers of the new geometry (another translation unit) --- */
typedef struct { float x, y, u, v; uint32_t c; } GeoVert;
void geo_side_reset(void);
size_t geo_side_vert_count(void);
size_t geo_side_index_count(void);
const GeoVert *geo_side_verts(void);
const uint16_t *geo_side_indices(void);
int geo_side_rect(float, float, float, float, uint32_t);
int geo_side_circle(float, float, float, uint32_t);
int geo_side_ring(float, float, float, float, uint32_t);
int geo_side_line(float, float, float, float, float, uint32_t);
int geo_side_roundrect(float, float, float, float, float, uint32_t);
int geo_side_rect_rot(float, float, float, float, float, uint32_t);
int geo_side_tex(float, float, float, float, float, float, uint32_t);
int geo_side_text(const char *, float, float, uint32_t, float);


/* --- emulation of the GPU rasteriser: triangles, pixel centres --- */

static int edge_fn(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

/* A pixel on the shared edge of two triangles may belong to exactly one of them,
 * otherwise the alpha blending accumulates twice, which cannot happen on real
 * hardware because it applies the top-left rule. The edge a->b owns its boundary
 * when it is a left edge (b.y > a.y) or a top edge (horizontal, b.x < a.x); for
 * triangles with the opposite winding the rule is mirrored. */
static int edge_owns(int e0, float ax, float ay, float bx, float by, int ccw) {
    if (e0 != 0) return 0;
    int forward = (by > ay) || (by == ay && bx < ax);
    return ccw ? forward : !forward;
}

static void raster_tri(Buffer *b, const GeoVert *v) {
    float minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < 3; i++) {
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > b->width) x1 = b->width;
    if (y1 > b->height) y1 = b->height;
    double area = edge_fn(v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y);
    if (fabs(area) < 1e-9) return;
    int ccw = area > 0;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
            double w0 = edge_fn(v[1].x, v[1].y, v[2].x, v[2].y, fx, fy);
            double w1 = edge_fn(v[2].x, v[2].y, v[0].x, v[0].y, fx, fy);
            double w2 = edge_fn(v[0].x, v[0].y, v[1].x, v[1].y, fx, fy);
            int inside;
            if (ccw) inside = (w0 > 0 || edge_owns((int)w0, v[1].x, v[1].y, v[2].x, v[2].y, ccw)) &&
                              (w1 > 0 || edge_owns((int)w1, v[2].x, v[2].y, v[0].x, v[0].y, ccw)) &&
                              (w2 > 0 || edge_owns((int)w2, v[0].x, v[0].y, v[1].x, v[1].y, ccw));
            else inside = (w0 < 0 || edge_owns((int)w0, v[1].x, v[1].y, v[2].x, v[2].y, ccw)) &&
                          (w1 < 0 || edge_owns((int)w1, v[2].x, v[2].y, v[0].x, v[0].y, ccw)) &&
                          (w2 < 0 || edge_owns((int)w2, v[0].x, v[0].y, v[1].x, v[1].y, ccw));
            if (!inside) continue;
            /* A solid primitive has one colour on all its vertices and no uv, so
             * the vertex colour is used. */
            uint32_t col = v[0].c;
            b->pixels[py * b->stride + px] = blend(b->pixels[py * b->stride + px], col);
        }
    }
}

/* Textured variant: a nearest sampler with the blending rule of the GPU
 * (image.frag: color * texel; tint.frag: vertex rgb, alpha tex.a * col.a). */
static void raster_tri_tex(Buffer *b, const GeoVert *v, const Texture *t, int tint) {
    float minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < 3; i++) {
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > b->width) x1 = b->width;
    if (y1 > b->height) y1 = b->height;
    double area = edge_fn(v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y);
    if (fabs(area) < 1e-9) return;
    int ccw = area > 0;
    uint32_t col = v[0].c;
    uint32_t cr = col & 0xff, cg = (col >> 8) & 0xff, cb = (col >> 16) & 0xff, ca = (col >> 24) & 0xff;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
            double w0 = edge_fn(v[1].x, v[1].y, v[2].x, v[2].y, fx, fy);
            double w1 = edge_fn(v[2].x, v[2].y, v[0].x, v[0].y, fx, fy);
            double w2 = edge_fn(v[0].x, v[0].y, v[1].x, v[1].y, fx, fy);
            int inside;
            if (ccw) inside = (w0 > 0 || edge_owns((int)w0, v[1].x, v[1].y, v[2].x, v[2].y, ccw)) &&
                              (w1 > 0 || edge_owns((int)w1, v[2].x, v[2].y, v[0].x, v[0].y, ccw)) &&
                              (w2 > 0 || edge_owns((int)w2, v[0].x, v[0].y, v[1].x, v[1].y, ccw));
            else inside = (w0 < 0 || edge_owns((int)w0, v[1].x, v[1].y, v[2].x, v[2].y, ccw)) &&
                          (w1 < 0 || edge_owns((int)w1, v[2].x, v[2].y, v[0].x, v[0].y, ccw)) &&
                          (w2 < 0 || edge_owns((int)w2, v[0].x, v[0].y, v[1].x, v[1].y, ccw));
            if (!inside) continue;
            w0 /= area; w1 /= area; w2 /= area;
            float u = (float)(w0 * v[0].u + w1 * v[1].u + w2 * v[2].u);
            float vv = (float)(w0 * v[0].v + w1 * v[1].v + w2 * v[2].v);
            /* Nearest sampler: the texel centre is floor(uv * size). */
            int tx = (int)floorf(u * t->w);
            int ty = (int)floorf(vv * t->h);
            if (tx < 0) tx = 0; if (tx >= t->w) tx = t->w - 1;
            if (ty < 0) ty = 0; if (ty >= t->h) ty = t->h - 1;
            uint32_t p = t->pixels[ty * t->w + tx];
            uint32_t src;
            if (tint) {
                uint32_t a = ((p >> 24) & 0xff) * ca / 255;
                src = cr | (cg << 8) | (cb << 16) | (a << 24);
            } else {
                src = p; /* image.frag: col(white) * texel = texel */
            }
            if ((src >> 24) == 0) continue;
            b->pixels[py * b->stride + px] = blend(b->pixels[py * b->stride + px], src);
        }
    }
}

/* Running geometry through the "GPU": mode 0 blends, mode 1 draws a textured
 * quad (a sample of the test texture, with tint taken from the vertex colour:
 * white means an ordinary texture, anything else a tint) and mode 2 replaces
 * without blending, as roundrect does through fill_span in the software
 * renderer. */
static void raster_tri_repl(Buffer *b, const GeoVert *v) {
    float minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < 3; i++) {
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > b->width) x1 = b->width;
    if (y1 > b->height) y1 = b->height;
    double area = edge_fn(v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y);
    if (fabs(area) < 1e-9) return;
    int ccw = area > 0;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
            double w0 = edge_fn(v[1].x, v[1].y, v[2].x, v[2].y, fx, fy);
            double w1 = edge_fn(v[2].x, v[2].y, v[0].x, v[0].y, fx, fy);
            double w2 = edge_fn(v[0].x, v[0].y, v[1].x, v[1].y, fx, fy);
            int inside;
            if (ccw) inside = (w0 > 0 || edge_owns((int)w0, v[1].x, v[1].y, v[2].x, v[2].y, ccw)) &&
                              (w1 > 0 || edge_owns((int)w1, v[2].x, v[2].y, v[0].x, v[0].y, ccw)) &&
                              (w2 > 0 || edge_owns((int)w2, v[0].x, v[0].y, v[1].x, v[1].y, ccw));
            else inside = (w0 < 0 || edge_owns((int)w0, v[1].x, v[1].y, v[2].x, v[2].y, ccw)) &&
                          (w1 < 0 || edge_owns((int)w1, v[2].x, v[2].y, v[0].x, v[0].y, ccw)) &&
                          (w2 < 0 || edge_owns((int)w2, v[0].x, v[0].y, v[1].x, v[1].y, ccw));
            if (!inside) continue;
            b->pixels[py * b->stride + px] = v[0].c; /* replace, as fill_span does */
        }
    }
}

static void rasterize_geo(Buffer *b, const Texture *t, int mode) {
    const GeoVert *verts = geo_side_verts();
    const uint16_t *idx = geo_side_indices();
    size_t n = geo_side_index_count();
    for (size_t i = 0; i + 3 <= n; i += 3) {
        GeoVert v[3] = { verts[idx[i]], verts[idx[i + 1]], verts[idx[i + 2]] };
        if (mode == 1 && t) raster_tri_tex(b, v, t, v[0].c != 0xffffffffu);
        else if (mode == 2) raster_tri_repl(b, v);
        else raster_tri(b, v);
    }
}

/* --- comparison and output --- */

static void buf_alloc(Buffer *b, int w, int h) {
    b->width = w; b->height = h; b->stride = w;
    b->pixels = (uint32_t *)calloc((size_t)w * h, 4);
}
static void buf_clear(Buffer *b) { memset(b->pixels, 0, (size_t)b->width * b->height * 4); }

static void write_bmp(const char *path, const Buffer *b) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int w = b->width, h = b->height;
    int row = (w * 3 + 3) & ~3;
    int data = row * h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(int32_t *)(hdr + 2) = 54 + data;
    *(int32_t *)(hdr + 10) = 54;
    *(int32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = w;
    *(int32_t *)(hdr + 22) = h;
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    *(int32_t *)(hdr + 34) = data;
    fwrite(hdr, 1, 54, f);
    uint8_t *rowb = (uint8_t *)calloc(1, (size_t)row);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t p = b->pixels[y * b->stride + x];
            rowb[x*3+0] = (uint8_t)((p >> 16) & 0xff);
            rowb[x*3+1] = (uint8_t)((p >> 8) & 0xff);
            rowb[x*3+2] = (uint8_t)(p & 0xff);
        }
        fwrite(rowb, 1, (size_t)row, f);
    }
    free(rowb);
    fclose(f);
}

typedef struct { long diff_big, diff_small, changed_union; int max_delta; } CmpStat;

static CmpStat compare(const Buffer *a, const Buffer *g) {
    CmpStat st = {0, 0, 0, 0};
    for (int y = 0; y < a->height; y++) {
        for (int x = 0; x < a->width; x++) {
            uint32_t pa = a->pixels[y * a->stride + x];
            uint32_t pg = g->pixels[y * g->stride + x];
            if (pa == pg) continue;
            int d = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                int da = abs((int)((pa >> sh) & 0xff) - (int)((pg >> sh) & 0xff));
                if (da > d) d = da;
            }
            if (d > st.max_delta) st.max_delta = d;
            st.changed_union++;
            if (d > 40) st.diff_big++;
            else st.diff_small++;
        }
    }
    return st;
}

/* --- scenes --- */

static Texture test_texture;
static Buffer *gpu_buf_sink;

/* Every primitive is a batch of its own: the geometry is built and rasterised
 * into the buffer with the blending mode that fits it. */
static Buffer *gpu_target;
static void gpu_rect(float x, float y, float w, float h, uint32_t c) {
    geo_side_reset(); geo_side_rect(x, y, w, h, c);
    rasterize_geo(gpu_target, NULL, 0);
}
static void gpu_circle(float x, float y, float r, uint32_t c) {
    geo_side_reset(); geo_side_circle(x, y, r, c);
    rasterize_geo(gpu_target, NULL, 0);
}
static void gpu_ring(float x, float y, float r, float th, uint32_t c) {
    geo_side_reset(); geo_side_ring(x, y, r, th, c);
    rasterize_geo(gpu_target, NULL, 0);
}
static void gpu_line(float x1, float y1, float x2, float y2, float th, uint32_t c) {
    geo_side_reset(); geo_side_line(x1, y1, x2, y2, th, c);
    rasterize_geo(gpu_target, NULL, 0);
}
static void gpu_roundrect(float x, float y, float w, float h, float r, uint32_t c) {
    geo_side_reset(); geo_side_roundrect(x, y, w, h, r, c);
    rasterize_geo(gpu_target, NULL, 2); /* fill_span: replace */
}
static void gpu_tex(float x, float y, float a, float sc, float w, float h, uint32_t c) {
    geo_side_reset(); geo_side_tex(x, y, a, sc, w, h, c);
    rasterize_geo(gpu_target, &test_texture, 1);
}
static void make_texture(void) {
    static uint32_t pixels[24 * 16];
    test_texture.w = 24; test_texture.h = 16; test_texture.opaque = 0;
    test_texture.pixels = pixels;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 24; x++) {
            int on = ((x / 4) + (y / 4)) % 2;
            uint8_t a = (uint8_t)(40 + ((x * 7 + y * 11) % 200));
            pixels[y * 24 + x] = (uint32_t)(on ? 0x30c060 : 0xe04020) | ((uint32_t)a << 24);
        }
}

static void draw_scene_1(Buffer *b, int legacy) {
    /* Aligned opaque rectangles must match pixel for pixel. */
    uint32_t red = pack_c(0xFFE23B3B), green = pack_c(0xFF30C060), blue = pack_c(0xFF3080E0);
    if (legacy) {
        render_rect(b, 10, 8, 40, 26, red);
        render_rect(b, 70, 20, 50, 40, green);
        render_rect(b, 30, 60, 90, 30, blue);
    } else {
        gpu_target = gpu_buf_sink;
        gpu_rect(10, 8, 40, 26, red);
        gpu_rect(70, 20, 50, 40, green);
        gpu_rect(30, 60, 90, 30, blue);
    }
}

static void draw_scene_2(Buffer *b, int legacy) {
    /* Translucent circles, rings, lines and rounded corners over base plates. */
    uint32_t base = pack_c(0xFF202830), alpha_w = pack_c(0x90FFFFFF), alpha_r = pack_c(0x60E04040);
    if (legacy) {
        render_rect(b, 0, 0, 120, 100, base);
        render_circle(b, 30, 30, 22, alpha_w);
        render_circle(b, 80, 35, 15, alpha_r);
        render_ring(b, 60, 70, 24, 6, alpha_w);
        render_ring(b, 95, 25, 8, 14, alpha_w);   /* th >= r makes a disc */
        render_line(b, 8, 90, 90, 15, 5, alpha_r);
        render_line(b, 40, 40, 40.0001f, 40, 7, alpha_w); /* degenerate: a circle */
        render_roundrect(b, 15, 78, 60, 18, 9, alpha_w);
        render_roundrect(b, 60, 55, 50, 30, 0, alpha_r);  /* r=0 gives a rectangle */
    } else {
        gpu_target = gpu_buf_sink;
        gpu_rect(0, 0, 120, 100, base);
        gpu_circle(30, 30, 22, alpha_w);
        gpu_circle(80, 35, 15, alpha_r);
        gpu_ring(60, 70, 24, 6, alpha_w);
        gpu_ring(95, 25, 8, 14, alpha_w);
        gpu_line(8, 90, 90, 15, 5, alpha_r);
        gpu_line(40, 40, 40.0001f, 40, 7, alpha_w);
        gpu_roundrect(15, 78, 60, 18, 9, alpha_w);
        gpu_roundrect(60, 55, 50, 30, 0, alpha_r);
    }
}

static void draw_scene_3(Buffer *b, int legacy) {
    if (legacy) {
        draw_tx(b, &test_texture, 4, 4, 0.0f, 1.0f);
        draw_tx(b, &test_texture, 40, 10, 0.0f, 2.5f);
        draw_tx(b, &test_texture, 10, 60, 0.7f, 2.0f);
        draw_tx_tint(b, &test_texture, 70, 55, 1.3f, 1.5f, pack_c(0x80102030));
        draw_tx_tint(b, &test_texture, 90, 80, 0.0f, 1.0f, pack_c(0xFF000000));
    } else {
        gpu_target = gpu_buf_sink;
        gpu_tex(4, 4, 0.0f, 1.0f, 24, 16, pack_c(0xFFFFFFFF));
        gpu_tex(40, 10, 0.0f, 2.5f, 24, 16, pack_c(0xFFFFFFFF));
        gpu_tex(10, 60, 0.7f, 2.0f, 24, 16, pack_c(0xFFFFFFFF));
        gpu_tex(70, 55, 1.3f, 1.5f, 24, 16, pack_c(0x80102030));
        gpu_tex(90, 80, 0.0f, 1.0f, 24, 16, pack_c(0xFF000000));
    }
}

static void draw_scene_4(Buffer *b, int legacy) {
    /* Clipping: shapes crossing the screen edges. */
    uint32_t alpha_g = pack_c(0xA030C060);
    if (legacy) {
        render_circle(b, -10, 10, 30, alpha_g);
        render_circle(b, 118, 95, 40, alpha_g);
        render_rect(b, -20, 40, 50, 20, alpha_g);
        render_line(b, -5, 60, 130, 10, 8, alpha_g);
        render_ring(b, 60, -8, 30, 9, alpha_g);
    } else {
        gpu_target = gpu_buf_sink;
        gpu_circle(-10, 10, 30, alpha_g);
        gpu_circle(118, 95, 40, alpha_g);
        gpu_rect(-20, 40, 50, 20, alpha_g);
        gpu_line(-5, 60, 130, 10, 8, alpha_g);
        gpu_ring(60, -8, 30, 9, alpha_g);
    }
}

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : NULL;
    make_texture();
    Buffer legacy_buf, gpu_buf;
    buf_alloc(&legacy_buf, 120, 100);
    buf_alloc(&gpu_buf, 120, 100);
    gpu_buf_sink = &gpu_buf;

    int failures = 0;

    struct { const char *name; void (*fn)(Buffer *, int); int exact; } scenes[] = {
        { "rects_aligned", draw_scene_1, 1 },
        { "shapes_alpha", draw_scene_2, 0 },
        { "textures", draw_scene_3, 0 },
        { "clipping", draw_scene_4, 0 },
    };

    for (size_t s = 0; s < sizeof(scenes) / sizeof(scenes[0]); s++) {
        buf_clear(&legacy_buf);
        buf_clear(&gpu_buf);
        scenes[s].fn(&legacy_buf, 1);
        geo_side_reset();
        scenes[s].fn(&gpu_buf, 0);
        CmpStat st = compare(&legacy_buf, &gpu_buf);
        double ratio = (double)st.diff_big / (double)(legacy_buf.width * legacy_buf.height);
        int ok;
        if (scenes[s].exact) ok = st.diff_big == 0 && st.diff_small == 0 && st.changed_union == 0;
        else ok = ratio <= 0.05 && st.diff_big <= 450;
        /* Curved scenes are compared with a tolerance: the software renderer
         * computed coverage from the analytic circle, while the GPU paints by
         * pixel centres inside polygons, so a thin band of about a pixel differs
         * along the border, and on rotated textures the bytes differ on some
         * boundary texels, a float coin flip on an exact edge. */
        printf("%-14s: diff>40: %ld, diff<=40: %ld, changed: %ld, max_delta: %d -> %s\n",
               scenes[s].name, st.diff_big, st.diff_small, st.changed_union, st.max_delta,
               ok ? "OK" : "FAIL");
        if (!ok) failures++;
        if (outdir) {
            char path[512];
            snprintf(path, sizeof(path), "%s/legacy_%s.bmp", outdir, scenes[s].name);
            write_bmp(path, &legacy_buf);
            snprintf(path, sizeof(path), "%s/gpu_%s.bmp", outdir, scenes[s].name);
            write_bmp(path, &gpu_buf);
        }
    }

    /* Rotated rectangle (rect_rot, the hitboxes): its four corners must be the
     * corners of the unrotated rectangle turned by ang around its centre, and at
     * ang == 0 the geometry must match geo_rect bit for bit. */
    {
        int ok = 1;
        float x = 100.0f, y = 50.0f, w = 40.0f, h = 80.0f, ang = 0.6f;
        geo_side_reset();
        geo_side_rect_rot(x, y, w, h, ang, pack_c(0xFFFFFFFF));
        const GeoVert *v = geo_side_verts();
        if (geo_side_vert_count() != 4 || geo_side_index_count() != 6) ok = 0;
        if (ok) {
            float hw = w * 0.5f, hh = h * 0.5f, cx = x + hw, cy = y + hh;
            const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
            for (int i = 0; i < 4 && ok; i++) {
                float lx = sx[i] * hw, ly = sy[i] * hh;
                float ex = cx + cosf(ang) * lx - sinf(ang) * ly;
                float ey = cy + sinf(ang) * lx + cosf(ang) * ly;
                if (fabsf(v[i].x - ex) > 1e-3f || fabsf(v[i].y - ey) > 1e-3f) ok = 0;
            }
        }
        /* Without a rotation it is exactly geo_rect. */
        geo_side_reset();
        geo_side_rect_rot(x, y, w, h, 0.0f, pack_c(0xFFFFFFFF));
        const GeoVert *q = geo_side_verts();
        if (geo_side_vert_count() != 4) ok = 0;
        else if (q[0].x != x || q[0].y != y || q[2].x != x + w || q[2].y != y + h) ok = 0;
        /* The area of the quad does not change under rotation (shoelace). */
        geo_side_reset();
        geo_side_rect_rot(x, y, w, h, 1.1f, pack_c(0xFFFFFFFF));
        const GeoVert *r = geo_side_verts();
        double area = 0;
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;
            area += (double)r[i].x * r[j].y - (double)r[j].x * r[i].y;
        }
        area = fabs(area) * 0.5;
        if (fabs(area - (double)w * h) > 0.5) ok = 0;
        printf("%-14s: %s\n", "rect_rot", ok ? "OK" : "FAIL");
        if (!ok) failures++;
    }

    /* Numerical check of the text layout, from the old render_text_now formula.
     * pen = x - lb*sc; base = y + asc*sc, where asc is bearing_top of 'S'; a glyph
     * quad sits at dx = pen + bearing_x*sc and top = base - bearing_top*sc with a
     * size of width*sc by height*sc, and missing characters become '?'. */
    geo_side_reset();
    if (!geo_side_text("SAЯ\nA!", 10, 5, pack_c(0xFFFFFFFF), 2.0f)) {
        printf("text_layout: could not build the geometry -> FAIL\n");
        failures++;
    } else {
        const GeoVert *verts = geo_side_verts();
        size_t vn = geo_side_vert_count();
        /* reference metrics: S adv20 bx2 bt30 w14 h30; A adv18 bx1 bt28 w16 h28; ? adv16 bx1 bt28 w14 h28 */
        float pen = 10.0f - 2.0f * 2.0f, base = 5.0f + 30.0f * 2.0f;
        float expect[7][4]; /* x, y, w, h for 'S','A','?','\n','A','?' */
        int gi = 0;
        float p = pen, bse = base;
        /* 'S' */ { expect[gi][0]=p+2*2; expect[gi][1]=bse-30*2; expect[gi][2]=14*2; expect[gi][3]=30*2; p+=20*2; gi++; }
        /* 'A' */ { expect[gi][0]=p+1*2; expect[gi][1]=bse-28*2; expect[gi][2]=16*2; expect[gi][3]=28*2; p+=18*2; gi++; }
        /* 'Я' -> '?' */ { expect[gi][0]=p+1*2; expect[gi][1]=bse-28*2; expect[gi][2]=14*2; expect[gi][3]=28*2; p+=16*2; gi++; }
        /* '\n' */ { p = pen; bse += 40.0f * 2.0f; }
        /* 'A' */ { expect[gi][0]=p+1*2; expect[gi][1]=bse-28*2; expect[gi][2]=16*2; expect[gi][3]=28*2; p+=18*2; gi++; }
        /* '!' -> '?' */ { expect[gi][0]=p+1*2; expect[gi][1]=bse-28*2; expect[gi][2]=14*2; expect[gi][3]=28*2; gi++; }
        int text_ok = vn == (size_t)gi * 4;
        if (text_ok) {
            for (int q = 0; q < gi && text_ok; q++) {
                const GeoVert *v = verts + q * 4;
                float qx = v[0].x, qy = v[0].y;
                float qw = v[1].x - v[0].x, qh = v[3].y - v[0].y;
                if (fabs(qx - expect[q][0]) > 0.01f || fabs(qy - expect[q][1]) > 0.01f ||
                    fabs(qw - expect[q][2]) > 0.01f || fabs(qh - expect[q][3]) > 0.01f) {
                    printf("  glyph %d: quad (%.2f, %.2f, %.2f, %.2f), expected (%.2f, %.2f, %.2f, %.2f)\n",
                           q, qx, qy, qw, qh, expect[q][0], expect[q][1], expect[q][2], expect[q][3]);
                    text_ok = 0;
                }
            }
        } else {
            printf("  %zu glyphs, expected %d\n", vn / 4, gi);
        }
        printf("%-14s: %s\n", "text_layout", text_ok ? "OK" : "FAIL");
        if (!text_ok) failures++;
    }

    /* Index sanity on a large scene. */
    geo_side_reset();
    for (int i = 0; i < 300; i++)
        geo_side_circle((float)(i % 60) * 2, (float)(i % 40) * 2, 30.0f + (float)i, pack_c(0x80FFFFFF));
    size_t vn = geo_side_vert_count(), in = geo_side_index_count();
    const uint16_t *idx = geo_side_indices();
    int idx_ok = in % 3 == 0;
    for (size_t i = 0; idx_ok && i < in; i++) if (idx[i] >= vn) idx_ok = 0;
    printf("%-14s: verts=%zu idx=%zu -> %s\n", "index_sanity", vn, in, idx_ok ? "OK" : "FAIL");
    if (!idx_ok) failures++;

    free(legacy_buf.pixels);
    free(gpu_buf.pixels);
    printf(failures ? "RESULT: %d scene(s) failed\n" : "RESULT: every check passed\n", failures);
    return failures ? 1 : 0;
}
