/* Turns DrawScript primitives into triangles for the GPU.
 *
 * It matches the software renderer that used to sit here: a rectangle is an
 * aligned quad, a circle a fan with an adaptive segment count, a ring a band
 * between two circles, a line a quad with round caps on its ends, where coverage
 * is "distance to the segment <= thickness / 2" as before, a rounded rectangle a
 * centre plus side bands plus quarter fans, a rotated rectangle (rect_rot) a quad
 * around the centre of the shape like a texture but without UV, used by the
 * hitboxes that need sharp corners and real alpha blending, a texture a rotated
 * quad with UV, with the rotation of the old per-pixel loop, and text one quad
 * per glyph from the font atlas.
 *
 * Nothing here is Vulkan specific: a host unit test (tools/host_test) compares the
 * triangle raster with the old software rasteriser. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { float x, y, u, v; uint32_t c; } GeoVert;

static GeoVert *geo_verts;
static size_t geo_vn, geo_vcap;
static uint16_t *geo_idx;
static size_t geo_in, geo_icap;

static int geo_push_vert(float x, float y, float u, float v, uint32_t c) {
    /* uint16 indices address 65536 vertices at most, and an overflow would make
     * the GPU read garbage vertices, up to a lost device, so extra geometry is
     * dropped silently. */
    if (geo_vn >= 65535) return 0;
    if (geo_vn == geo_vcap) {
        size_t cap = geo_vcap ? geo_vcap * 2 : 1024;
        if (cap > SIZE_MAX / sizeof(*geo_verts)) return 0;
        GeoVert *nv = (GeoVert *)realloc(geo_verts, cap * sizeof(*nv));
        if (!nv) return 0;
        geo_verts = nv; geo_vcap = cap;
    }
    GeoVert *g = &geo_verts[geo_vn++];
    g->x = x; g->y = y; g->u = u; g->v = v; g->c = c;
    return 1;
}
static int geo_push_index(uint16_t i) {
    if (geo_in == geo_icap) {
        size_t cap = geo_icap ? geo_icap * 2 : 2048;
        if (cap > SIZE_MAX / sizeof(*geo_idx)) return 0;
        uint16_t *ni = (uint16_t *)realloc(geo_idx, cap * sizeof(*ni));
        if (!ni) return 0;
        geo_idx = ni; geo_icap = cap;
    }
    geo_idx[geo_in++] = i;
    return 1;
}
/* A ready quad of four vertices in the order (0, 1, 2, 3). */
static int geo_quad(const GeoVert q[4]) {
    size_t base = geo_vn;
    for (int i = 0; i < 4; i++)
        if (!geo_push_vert(q[i].x, q[i].y, q[i].u, q[i].v, q[i].c)) return 0;
    static const uint16_t order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++)
        if (!geo_push_index((uint16_t)(base + order[i]))) return 0;
    return 1;
}
static void geo_reset(void) { geo_vn = 0; geo_in = 0; }

/* --- trigonometry cache for the fans ---
 *
 * Circles and rings appear in the hundreds per frame (dust, controls, hitboxes,
 * universe particles, explosions) and almost all of them tessellate with the same
 * segment count. cosf and sinf are the most expensive part of building geometry,
 * so the values are computed once per (segment count, formula) pair and reused.
 * The angle formulas are exactly the ones used when computing directly, so the
 * vertices come out identical to before. */
#define GEO_TRIG_SLOTS 12
typedef struct { int n, mode; float *cosv, *sinv; } GeoTrig;
static GeoTrig geo_trig[GEO_TRIG_SLOTS];
static int geo_trig_next;

/* mode 0 is a circle fan with a = i * 2pi / n in float, as before, and mode 1 is
 * a ring band with a = (i / n) * 2pi in double, as before. */
static const GeoTrig *geo_trig_table(int n, int mode) {
    if (n < 3 || n > 4096) return NULL;
    for (int i = 0; i < GEO_TRIG_SLOTS; i++) {
        GeoTrig *t = &geo_trig[i];
        if (t->cosv && t->n == n && t->mode == mode) return t;
    }
    GeoTrig *t = &geo_trig[geo_trig_next];
    geo_trig_next = (geo_trig_next + 1) % GEO_TRIG_SLOTS;
    free(t->cosv); free(t->sinv);
    t->cosv = NULL; t->sinv = NULL; t->n = 0;
    float *c = (float *)malloc((size_t)(n + 1) * sizeof(float));
    float *v = (float *)malloc((size_t)(n + 1) * sizeof(float));
    if (!c || !v) { free(c); free(v); return NULL; }
    for (int i = 0; i <= n; i++) {
        float a = mode == 0 ? (float)i * (float)(2.0 * M_PI) / (float)n
                            : (float)((double)i / n * 2.0 * M_PI);
        c[i] = cosf(a);
        v[i] = sinf(a);
    }
    t->cosv = c; t->sinv = v; t->n = n; t->mode = mode;
    return t;
}
static void geo_trig_free(void) {
    for (int i = 0; i < GEO_TRIG_SLOTS; i++) {
        free(geo_trig[i].cosv); free(geo_trig[i].sinv);
        geo_trig[i].cosv = geo_trig[i].sinv = NULL;
        geo_trig[i].n = geo_trig[i].mode = 0;
    }
}

/* --- primitives --- */

static int geo_rect(float x, float y, float w, float h, uint32_t c) {
    if (!isfinite(x + y + w + h) || w <= 0 || h <= 0) return 1;
    GeoVert q[4] = {
        { x,     y,     0, 0, c }, { x + w, y,     1, 0, c },
        { x + w, y + h, 1, 1, c }, { x,     y + h, 0, 1, c },
    };
    return geo_quad(q);
}

/* Adaptive segment count of a circle. The old 1.5 segments per pixel gave 105
 * triangles even for an ordinary button of radius 70, and a few control rings per
 * frame cost weak CPUs and GPUs more than the whole 2D scene. At 0.6 with a cap of
 * 64 the result still looks smooth on a phone screen while the geometry of large
 * circles more than halves. */
static int geo_circle_segs(float r) {
    if (r < 0) r = -r;
    int n = (int)(r * 0.6f);
    if (n < 12) n = 12;
    if (n > 64) n = 64;
    return n;
}

static int geo_circle(float x, float y, float r, uint32_t c) {
    if (!isfinite(x + y + r) || r <= 0) return 1;
    int n = geo_circle_segs(r);
    size_t base = geo_vn;
    if (!geo_push_vert(x, y, 0.5f, 0.5f, c)) return 0;
    const GeoTrig *tt = geo_trig_table(n, 0);
    for (int i = 0; i < n; i++) {
        float ca, sa;
        if (tt) { ca = tt->cosv[i]; sa = tt->sinv[i]; }
        else { float a = (float)i * (float)(2.0 * M_PI) / (float)n; ca = cosf(a); sa = sinf(a); }
        if (!geo_push_vert(x + ca * r, y + sa * r, 0, 0, c)) return 0;
    }
    for (int i = 1; i <= n; i++)
        if (!geo_push_index((uint16_t)base) ||
            !geo_push_index((uint16_t)(base + i)) ||
            !geo_push_index((uint16_t)(base + (i % n) + 1))) return 0;
    return 1;
}

static int geo_ring(float x, float y, float r, float th, uint32_t c) {
    if (!isfinite(x + y + r + th) || r <= 0 || th <= 0) return 1;
    if (th >= r) return geo_circle(x, y, r, c);
    int n = geo_circle_segs(r);
    size_t base = geo_vn;
    const GeoTrig *tt = geo_trig_table(n, 1);
    for (int i = 0; i <= n; i++) {
        float ca, sa;
        if (tt) { ca = tt->cosv[i]; sa = tt->sinv[i]; }
        else { float a = (float)((double)i / n * 2.0 * M_PI); ca = cosf(a); sa = sinf(a); }
        if (!geo_push_vert(x + ca * (r - th), y + sa * (r - th), 0, 0, c)) return 0;
        if (!geo_push_vert(x + ca * r, y + sa * r, 0, 0, c)) return 0;
    }
    for (int i = 0; i < n; i++) {
        size_t b = base + (size_t)i * 2;
        if (!geo_push_index((uint16_t)b) || !geo_push_index((uint16_t)(b + 1)) ||
            !geo_push_index((uint16_t)(b + 2))) return 0;
        if (!geo_push_index((uint16_t)(b + 1)) || !geo_push_index((uint16_t)(b + 3)) ||
            !geo_push_index((uint16_t)(b + 2))) return 0;
    }
    return 1;
}

/* A line is a rectangle along the segment plus two round caps, since the
 * per-pixel version used disc coverage of radius thickness / 2. */
static int geo_line(float x1, float y1, float x2, float y2, float th, uint32_t c) {
    if (!isfinite(x1 + y1 + x2 + y2 + th) || th <= 0) return 1;
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    float rad = th * 0.5f;
    if (len <= 0.0001f) return geo_circle(x1, y1, rad, c);
    float nx = -dy / len * rad, ny = dx / len * rad;
    GeoVert q[4] = {
        { x1 + nx, y1 + ny, 0, 0, c }, { x2 + nx, y2 + ny, 1, 0, c },
        { x2 - nx, y2 - ny, 1, 1, c }, { x1 - nx, y1 - ny, 0, 1, c },
    };
    if (!geo_quad(q)) return 0;
    int n = geo_circle_segs(rad) / 2;
    if (n < 6) n = 6;
    for (int cap = 0; cap < 2; cap++) {
        float cx = cap ? x2 : x1, cy = cap ? y2 : y1;
        float dir = cap ? -1.0f : 1.0f; /* the cap points away from the segment */
        float base_ang = atan2f(dy, dx) + (float)M_PI * 0.5f;
        size_t base = geo_vn;
        if (!geo_push_vert(cx, cy, 0, 0, c)) return 0;
        for (int i = 0; i <= n; i++) {
            float a = base_ang + dir * (float)((double)i / n * M_PI);
            if (!geo_push_vert(cx + cosf(a) * rad, cy + sinf(a) * rad, 0, 0, c)) return 0;
        }
        for (int i = 1; i <= n; i++)
            if (!geo_push_index((uint16_t)base) ||
                !geo_push_index((uint16_t)(base + i)) ||
                !geo_push_index((uint16_t)(base + i + 1))) return 0;
    }
    return 1;
}

static int geo_roundrect(float x, float y, float w, float h, float rad, uint32_t c) {
    if (!isfinite(x + y + w + h + rad) || w <= 0 || h <= 0) return 1;
    if (rad < 0) rad = 0;
    if (rad > w * 0.5f) rad = w * 0.5f;
    if (rad > h * 0.5f) rad = h * 0.5f;
    if (rad < 0.5f) return geo_rect(x, y, w, h, c);
    if (!geo_rect(x + rad, y, w - rad * 2, h, c)) return 0;               /* centre */
    if (!geo_rect(x, y + rad, rad, h - rad * 2, c)) return 0;             /* left */
    if (!geo_rect(x + w - rad, y + rad, rad, h - rad * 2, c)) return 0;   /* right */
    /* Six segments per quarter are enough for buttons of phone size. A single
     * roundrect used to cost up to 256 triangles, which hurt most in the frame
     * counter that is rebuilt every frame. */
    int n = (int)ceilf(rad * 0.35f);
    if (n < 6) n = 6;
    if (n > 20) n = 20;
    static const float cxs[4] = { 1, 0, 0, 1 }, cys[4] = { 1, 1, 0, 0 };
    static const float a0s[4] = { 0, (float)M_PI * 0.5f, (float)M_PI, (float)M_PI * 1.5f };
    for (int k = 0; k < 4; k++) {
        float cx = x + cxs[k] * (w - rad) + (cxs[k] ? 0 : rad);
        float cy = y + cys[k] * (h - rad) + (cys[k] ? 0 : rad);
        size_t base = geo_vn;
        if (!geo_push_vert(cx, cy, 0, 0, c)) return 0;
        for (int i = 0; i <= n; i++) {
            float a = a0s[k] + (float)((double)i / n * M_PI * 0.5);
            if (!geo_push_vert(cx + cosf(a) * rad, cy + sinf(a) * rad, 0, 0, c)) return 0;
        }
        for (int i = 1; i <= n; i++)
            if (!geo_push_index((uint16_t)base) ||
                !geo_push_index((uint16_t)(base + i)) ||
                !geo_push_index((uint16_t)(base + i + 1))) return 0;
    }
    return 1;
}

/* Rotated rectangle: the same rotation as a texture, corners around the centre of
 * the shape, but without UV or sampling, so it is a solid fill of the vertex
 * colour. The hitboxes need it, since the punch zone and the dash strip are
 * straight rectangles with sharp corners, while line() has round caps and
 * roundrect goes through a non-blending pipeline and stays opaque. */
static int geo_rect_rot(float x, float y, float w, float h, float ang, uint32_t c) {
    if (!isfinite(x + y + w + h + ang) || w <= 0 || h <= 0) return 1;
    /* Without a rotation the result must match geo_rect bit for bit. */
    if (ang == 0.0f) return geo_rect(x, y, w, h, c);
    float hw = w * 0.5f, hh = h * 0.5f;
    float ca = cosf(ang), sa = sinf(ang);
    float cx = x + hw, cy = y + hh;
    static const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
    GeoVert q[4];
    for (int i = 0; i < 4; i++) {
        float lx = sx[i] * hw, ly = sy[i] * hh;
        q[i].x = cx + ca * lx - sa * ly;
        q[i].y = cy + sa * lx + ca * ly;
        q[i].u = 0; q[i].v = 0; q[i].c = c;
    }
    return geo_quad(q);
}

/* Texture: the rotation of the old per-pixel loop, which mapped p backwards
 * through R(a) * p + centre, so the vertices use R(-a) applied to the corners.
 * x and y are the top left corner of the unaligned texture, sc the scale and a the
 * angle. */
static int geo_tex(float x, float y, float ang, float sc, float tw, float th_, uint32_t c) {
    if (!isfinite(x + y + ang + sc) || sc <= 0 || tw <= 0 || th_ <= 0) return 1;
    float hw = tw * 0.5f * sc, hh = th_ * 0.5f * sc;
    /* The arena background tile is dozens of unrotated textures per frame, and at
     * zero the cosine and sine are known, so no trigonometry call is needed. */
    float ca = ang == 0.0f ? 1.0f : cosf(ang), sa = ang == 0.0f ? 0.0f : sinf(ang);
    float cx = x + hw, cy = y + hh;
    static const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
    static const float uu[4] = { 0, 1, 1, 0 },  vv[4] = { 0, 0, 1, 1 };
    GeoVert q[4];
    for (int i = 0; i < 4; i++) {
        float lx = sx[i] * hw, ly = sy[i] * hh;
        q[i].x = cx + ca * lx - sa * ly;
        q[i].y = cy + sa * lx + ca * ly;
        q[i].u = uu[i]; q[i].v = vv[i]; q[i].c = c;
    }
    return geo_quad(q);
}

/* Text: one quad per glyph, with UV as the glyph window in the font atlas. The
 * metrics and the pen layout match the old render_text_now exactly. */
static int geo_text(const char *s, float x, float y, uint32_t c, float sc, const DSFont *fnt) {
    if (!s || !fnt || !isfinite(x + y + sc) || sc <= 0) return 1;
    float asc = ds_font_ascent(fnt), lb = 0;
    const DSFontGlyph *ref = ds_font_glyph(fnt, 'S');
    if (ref) { asc = ref->bearing_top; lb = ref->bearing_x; }
    float pen = x - lb * sc, base = y + asc * sc;
    for (const char *cur = s; *cur;) {
        int cp = utf8_dec(&cur);
        if (cp == '\n') { pen = x - lb * sc; base += ds_font_lineh(fnt) * sc; continue; }
        const DSFontGlyph *g = ds_font_glyph(fnt, (uint32_t)cp);
        if (!g) continue;
        if (g->width > 0 && g->height > 0) {
            GeoVert q[4] = {
                { pen + g->bearing_x * sc,                base - g->bearing_top * sc,                 g->u0, g->v0, c },
                { pen + (g->bearing_x + g->width) * sc,   base - g->bearing_top * sc,                 g->u1, g->v0, c },
                { pen + (g->bearing_x + g->width) * sc,   base - (g->bearing_top - g->height) * sc,   g->u1, g->v1, c },
                { pen + g->bearing_x * sc,                base - (g->bearing_top - g->height) * sc,   g->u0, g->v1, c },
            };
            if (!geo_quad(q)) return 0;
        }
        pen += g->advance * sc;
    }
    return 1;
}
