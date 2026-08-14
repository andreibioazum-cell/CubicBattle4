typedef struct DSFont DSFont;
void ds_font_destroy(DSFont *font);

#define DS_FONT_ATLAS_W 1024
#define DS_FONT_ATLAS_H 1024
#define DS_FONT_SS 4
#define DS_FONT_MAX 256
#define TAG(a,b,c,d) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d))

typedef struct { float x, y; int on_curve; } DSPoint;
typedef struct { DSPoint *points; int pc, pp; int *ends; int cc, cp; } DSOutline;
typedef struct { float x, y; } DSFPoint;
typedef struct { DSFPoint *points; int count, cap; } DSFC;

struct DSFont {
    uint8_t *data; size_t size;
    int upem, loc_format, ng, nhm, asc_u, desc_u, lg_u;
    uint32_t head, hhea, hmtx, maxp, loca, glyf, cmap, cmap4, cmap12;
    float scale, ascent, line_h;
    int aw, ah;
    uint8_t *alpha;
    DSFontGlyph *glyphs; int gcount;
};

static int in_r(const DSFont *f, size_t o, size_t l) { return f && o <= f->size && l <= f->size - o; }
static uint16_t bu16(const DSFont *f, size_t o) { return in_r(f, o, 2) ? (uint16_t)((f->data[o]<<8)|f->data[o+1]) : 0; }
static int16_t bs16(const DSFont *f, size_t o) { return (int16_t)bu16(f, o); }
static uint32_t bu32(const DSFont *f, size_t o) {
    return in_r(f, o, 4) ? ((uint32_t)f->data[o]<<24|(uint32_t)f->data[o+1]<<16|(uint32_t)f->data[o+2]<<8|(uint32_t)f->data[o+3]) : 0;
}
static int tbound(const DSFont *f, uint32_t tag, uint32_t *off, uint32_t *len) {
    if (!f || !in_r(f, 0, 12)) return 0;
    uint16_t n = bu16(f, 4);
    for (size_t i = 0; i < n; i++) {
        size_t r = 12 + i*16;
        if (in_r(f, r, 16) && bu32(f, r) == tag) {
            uint32_t a = bu32(f, r+8), l = bu32(f, r+12);
            if (in_r(f, a, l)) { if (off) *off = a; if (len) *len = l; return 1; }
        }
    }
    return 0;
}

static void ol_init(DSOutline *o) { memset(o, 0, sizeof(*o)); }
static void ol_free(DSOutline *o) { if (o) { free(o->points); free(o->ends); memset(o, 0, sizeof(*o)); } }
static int ol_add(DSOutline *o, const DSPoint *p, int n) {
    if (n <= 0) return 0;
    int np = o->pc + n, cp = o->pp ? o->pp : 32; while (cp < np) cp *= 2;
    DSPoint *pts = (DSPoint *)realloc(o->points, (size_t)cp * sizeof(*pts)); if (!pts) return 0;
    o->points = pts; o->pp = cp;
    int nc = o->cc + 1, cpc = o->cp ? o->cp : 8; while (cpc < nc) cpc *= 2;
    int *ends = (int *)realloc(o->ends, (size_t)cpc * sizeof(*ends)); if (!ends) return 0;
    o->ends = ends; o->cp = cpc;
    memcpy(o->points + o->pc, p, (size_t)n * sizeof(*p));
    o->pc += n; o->ends[o->cc++] = o->pc - 1;
    return 1;
}

static int g_offs(const DSFont *f, int g, uint32_t *s, uint32_t *e) {
    if (!f || g < 0 || g >= f->ng) return 0;
    uint32_t a = f->loc_format == 0 ? (uint32_t)bu16(f, f->loca + (size_t)g*2)*2 : bu32(f, f->loca + (size_t)g*4);
    uint32_t b = f->loc_format == 0 ? (uint32_t)bu16(f, f->loca + (size_t)(g+1)*2)*2 : bu32(f, f->loca + (size_t)(g+1)*4);
    if (a > b || !in_r(f, (size_t)f->glyf + a, b - a)) return 0;
    if (s) *s = a; if (e) *e = b; return 1;
}

static int read_outline(const DSFont *f, int g, int depth, float a, float b, float c, float d, float tx, float ty, DSOutline *dst);

static int read_simple(const DSFont *f, uint32_t off, int nc, float a, float b, float c, float d, float tx, float ty, DSOutline *dst) {
    if (nc <= 0 || nc > 4096) return 1;
    int *ends = (int *)malloc((size_t)nc * sizeof(*ends)); if (!ends) return 0;
    for (int i = 0; i < nc; i++) ends[i] = (int)bu16(f, (size_t)f->glyf + off + 10 + i*2);
    int pc = ends[nc-1] + 1;
    if (pc <= 0 || pc > 200000) { free(ends); return 0; }
    size_t cur = (size_t)f->glyf + off + 10 + (size_t)nc*2;
    cur += 2 + bu16(f, cur);
    int *flags = (int *)malloc((size_t)pc * sizeof(*flags));
    DSPoint *pts = (DSPoint *)calloc((size_t)pc, sizeof(*pts));
    if (!flags || !pts) { free(ends); free(flags); free(pts); return 0; }
    int p = 0;
    while (p < pc) {
        if (!in_r(f, cur, 1)) { free(ends); free(flags); free(pts); return 0; }
        uint8_t fl = f->data[cur++]; flags[p++] = fl;
        int rep = (fl & 8) ? (int)f->data[cur++] : 0;
        while (rep-- > 0 && p < pc) flags[p++] = fl;
    }
    int x = 0;
    for (p = 0; p < pc; p++) {
        int fl = flags[p], dlt = 0;
        if (fl & 2) { dlt = f->data[cur++]; if (!(fl & 16)) dlt = -dlt; }
        else if (!(fl & 16)) { dlt = bs16(f, cur); cur += 2; }
        x += dlt; pts[p].x = (float)x; pts[p].on_curve = (fl & 1) != 0;
    }
    int y = 0;
    for (p = 0; p < pc; p++) {
        int fl = flags[p], dlt = 0;
        if (fl & 4) { dlt = f->data[cur++]; if (!(fl & 32)) dlt = -dlt; }
        else if (!(fl & 32)) { dlt = bs16(f, cur); cur += 2; }
        y += dlt; pts[p].y = (float)y;
    }
    int st = 0;
    for (int cn = 0; cn < nc; cn++) {
        int en = ends[cn], cnt = en - st + 1;
        DSPoint *tr = (DSPoint *)malloc((size_t)cnt * sizeof(*tr));
        for (int i = 0; i < cnt; i++) {
            DSPoint in = pts[st + i]; tr[i].x = a*in.x + c*in.y + tx; tr[i].y = b*in.x + d*in.y + ty; tr[i].on_curve = in.on_curve;
        }
        ol_add(dst, tr, cnt); free(tr); st = en + 1;
    }
    free(ends); free(flags); free(pts); return 1;
}

static int read_composite(const DSFont *f, uint32_t off, int depth, float a, float b, float c, float d, float tx, float ty, DSOutline *dst) {
    size_t cur = (size_t)f->glyf + off + 10; int flags = 0x0020;
    while (flags & 0x0020) {
        flags = bu16(f, cur); int comp = bu16(f, cur + 2); cur += 4;
        int a1 = (flags & 1) ? bs16(f, cur) : (int8_t)f->data[cur];
        int a2 = (flags & 1) ? bs16(f, cur+2) : (int8_t)f->data[cur+1];
        cur += (flags & 1) ? 4 : 2;
        float ca=1, cb=0, cc=0, cd=1, dx=(flags & 2)?(float)a1:0, dy=(flags & 2)?(float)a2:0;
        if (flags & 8) { ca = cd = (float)bs16(f, cur)/16384.0f; cur += 2; }
        else if (flags & 0x0040) { ca = (float)bs16(f, cur)/16384.0f; cd = (float)bs16(f, cur+2)/16384.0f; cur += 4; }
        else if (flags & 0x0080) { ca = (float)bs16(f, cur)/16384.0f; cb = (float)bs16(f, cur+2)/16384.0f; cc = (float)bs16(f, cur+4)/16384.0f; cd = (float)bs16(f, cur+6)/16384.0f; cur += 8; }
        read_outline(f, comp, depth+1, a*ca + c*cb, b*ca + d*cb, a*cc + c*cd, b*cc + d*cd, a*dx + c*dy + tx, b*dx + d*dy + ty, dst);
    }
    return 1;
}

static int read_outline(const DSFont *f, int g, int depth, float a, float b, float c, float d, float tx, float ty, DSOutline *dst) {
    uint32_t s, e; if (depth > 16 || !g_offs(f, g, &s, &e) || s == e) return 0;
    int16_t cc = bs16(f, (size_t)f->glyf + s);
    return cc >= 0 ? read_simple(f, s, cc, a, b, c, d, tx, ty, dst) : read_composite(f, s, depth, a, b, c, d, tx, ty, dst);
}

static int flat_push(DSFC *f, float x, float y) {
    if (f->count >= f->cap) { f->cap = f->cap ? f->cap * 2 : 32; f->points = (DSFPoint*)realloc(f->points, (size_t)f->cap * sizeof(DSFPoint)); }
    f->points[f->count].x = x; f->points[f->count].y = y; f->count++; return 1;
}
static void flatten(const DSPoint *p, int n, DSFC *flat) {
    if (n <= 0) return;
    DSFPoint start = p[0].on_curve ? (DSFPoint){p[0].x, p[0].y} : (DSFPoint){(p[n-1].x+p[0].x)*0.5f, (p[n-1].y+p[0].y)*0.5f};
    DSFPoint cur = start; flat_push(flat, start.x, start.y);
    int idx = p[0].on_curve ? 1 : 0, proc = p[0].on_curve ? 1 : 0;
    while (proc < n) {
        const DSPoint *one = &p[idx % n];
        if (one->on_curve) { cur = (DSFPoint){one->x, one->y}; flat_push(flat, cur.x, cur.y); idx++; proc++; }
        else {
            const DSPoint *two = &p[(idx+1) % n];
            DSFPoint end = two->on_curve ? (DSFPoint){two->x, two->y} : (DSFPoint){(one->x+two->x)*0.5f, (one->y+two->y)*0.5f};
            for (int s = 1; s <= 8; s++) {
                float t = (float)s/8.0f, u = 1-t;
                flat_push(flat, u*u*cur.x + 2*u*t*one->x + t*t*end.x, u*u*cur.y + 2*u*t*one->y + t*t*end.y);
            }
            cur = end; idx += two->on_curve ? 2 : 1; proc += two->on_curve ? 2 : 1;
        }
    }
}
static int inside(const DSFC *c, int n, float x, float y) {
    int in = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 0, j = c[i].count-1; k < c[i].count; j = k++) {
            float yi = c[i].points[k].y, yj = c[i].points[j].y;
            if (((yi > y) != (yj > y)) && (x < (c[i].points[j].x - c[i].points[k].x)*(y - yi)/(yj - yi + 1e-6f) + c[i].points[k].x)) in = !in;
        }
    }
    return in;
}

static int g_for(const DSFont *f, uint32_t cp) {
    if (f->cmap12) {
        uint32_t n = bu32(f, f->cmap12 + 12);
        for (uint32_t i = 0; i < n; i++) {
            size_t at = f->cmap12 + 16 + i*12; uint32_t f1 = bu32(f, at), l1 = bu32(f, at+4);
            if (cp >= f1 && cp <= l1) return (int)(bu32(f, at+8) + cp - f1);
        }
    }
    if (f->cmap4 && cp <= 0xFFFF) {
        uint16_t sc = bu16(f, f->cmap4 + 6)/2;
        for (uint16_t i = 0; i < sc; i++) {
            uint16_t en = bu16(f, f->cmap4 + 14 + i*2), st = bu16(f, f->cmap4 + 16 + sc*2 + i*2);
            if (cp >= st && cp <= en) {
                int16_t dlt = bs16(f, f->cmap4 + 16 + sc*4 + i*2); uint16_t rng = bu16(f, f->cmap4 + 16 + sc*6 + i*2);
                if (rng == 0) return ((int)cp + dlt) & 0xFFFF;
                uint16_t g = bu16(f, f->cmap4 + 16 + sc*6 + i*2 + rng + (cp-st)*2);
                return g ? ((int)g + dlt) & 0xFFFF : 0;
            }
        }
    }
    return 0;
}

static int bake_glyph(DSFont *f, DSFontGlyph *g, int ax, int ay, int rh) {
    int gi = g_for(f, g->codepoint), adv = bu16(f, f->hmtx + (size_t)(gi < f->nhm ? gi : f->nhm - 1)*4);
    g->advance = adv * f->scale;
    DSOutline ol; ol_init(&ol);
    if (!read_outline(f, gi, 0, 1, 0, 0, 1, 0, 0, &ol) || !ol.pc) { ol_free(&ol); return rh; }
    int mnx = (int)ol.points[0].x, mxx = mnx, mny = (int)ol.points[0].y, mxy = mny;
    for (int p = 1; p < ol.pc; p++) {
        int x = (int)ol.points[p].x, y = (int)ol.points[p].y;
        if (x < mnx) mnx = x; if (x > mxx) mxx = x; if (y < mny) mny = y; if (y > mxy) mxy = y;
    }
    int w = (int)ceilf((mxx - mnx) * f->scale) + 2, h = (int)ceilf((mxy - mny) * f->scale) + 2;
    if (w < 1) w = 1; if (h < 1) h = 1;
    g->bearing_x = mnx * f->scale; g->bearing_top = mxy * f->scale; g->width = w; g->height = h;
    DSFC *flat = (DSFC *)calloc((size_t)ol.cc, sizeof(*flat)); int st = 0;
    for (int cn = 0; cn < ol.cc; cn++) { int en = ol.ends[cn]; flatten(ol.points + st, en - st + 1, &flat[cn]); st = en + 1; }
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int cov = 0;
            for (int sy = 0; sy < DS_FONT_SS; sy++) for (int sx = 0; sx < DS_FONT_SS; sx++) {
                float fx = mnx + ((float)px + ((float)sx+0.5f)/DS_FONT_SS)/f->scale, fy = mxy - ((float)py + ((float)sy+0.5f)/DS_FONT_SS)/f->scale;
                if (inside(flat, ol.cc, fx, fy)) cov++;
            }
            f->alpha[(size_t)(ay + py) * f->aw + (ax + px)] = (uint8_t)((cov * 255) / (DS_FONT_SS * DS_FONT_SS));
        }
    }
    g->u0 = (float)ax / f->aw; g->v0 = (float)ay / f->ah; g->u1 = (float)(ax + w) / f->aw; g->v1 = (float)(ay + h) / f->ah;
    for (int cn = 0; cn < ol.cc; cn++) free(flat[cn].points);
    free(flat); ol_free(&ol);
    return h > rh ? h : rh;
}

DSFont *ds_font_create(const uint8_t *data, size_t size, int ph) {
    if (!data || size < 12) return NULL;
    DSFont *f = (DSFont *)calloc(1, sizeof(*f)); if (!f) return NULL;
    f->data = (uint8_t *)malloc(size); f->glyphs = (DSFontGlyph *)calloc(DS_FONT_MAX, sizeof(*f->glyphs));
    f->alpha = (uint8_t *)calloc((size_t)DS_FONT_ATLAS_W * DS_FONT_ATLAS_H, 1);
    memcpy(f->data, data, size); f->size = size; f->aw = DS_FONT_ATLAS_W; f->ah = DS_FONT_ATLAS_H;
    uint32_t l;
    if (!tbound(f, TAG('h','e','a','d'), &f->head, &l) || !tbound(f, TAG('h','h','e','a'), &f->hhea, &l) ||
        !tbound(f, TAG('h','m','t','x'), &f->hmtx, &l) || !tbound(f, TAG('m','a','x','p'), &f->maxp, &l) ||
        !tbound(f, TAG('l','o','c','a'), &f->loca, &l) || !tbound(f, TAG('g','l','y','f'), &f->glyf, &l) ||
        !tbound(f, TAG('c','m','a','p'), &f->cmap, &l)) { ds_font_destroy(f); return NULL; }
    f->upem = bu16(f, f->head+18); f->loc_format = bs16(f, f->head+50); f->ng = bu16(f, f->maxp+4); f->nhm = bu16(f, f->hhea+34);
    f->asc_u = bs16(f, f->hhea+4); f->desc_u = bs16(f, f->hhea+6); f->lg_u = bs16(f, f->hhea+8);
    uint16_t n = bu16(f, f->cmap+2);
    for (uint16_t i = 0; i < n; i++) {
        uint16_t p = bu16(f, f->cmap + 4 + i*8), fmt = bu16(f, f->cmap + bu32(f, f->cmap + 4 + i*8 + 4));
        if (fmt == 12 && (p == 3 || p == 0)) f->cmap12 = f->cmap + bu32(f, f->cmap + 4 + i*8 + 4);
        else if (fmt == 4 && (p == 3 || p == 0) && !f->cmap4) f->cmap4 = f->cmap + bu32(f, f->cmap + 4 + i*8 + 4);
    }
    f->scale = (float)ph / f->upem; f->ascent = f->asc_u * f->scale; f->line_h = (f->asc_u - f->desc_u + f->lg_u) * f->scale;
    for (int cp = 32; cp <= 126; cp++) f->glyphs[f->gcount++].codepoint = (uint32_t)cp;
    for (int cp = 0x0410; cp <= 0x044F; cp++) f->glyphs[f->gcount++].codepoint = (uint32_t)cp;
    f->glyphs[f->gcount++].codepoint = '?';
    int ax = 1, ay = 1, rh = 0;
    for (int i = 0; i < f->gcount; i++) {
        int nh = bake_glyph(f, &f->glyphs[i], ax, ay, rh);
        ax += f->glyphs[i].width + 1;
        if (ax + 40 >= f->aw) { ax = 1; ay += rh + 1; rh = 0; }
        if (nh > rh) rh = nh;
    }
    return f;
}
void ds_font_destroy(DSFont *f) { if (f) { free(f->data); free(f->alpha); free(f->glyphs); free(f); } }
const DSFontGlyph *ds_font_glyph(const DSFont *f, uint32_t cp) {
    if (!f) return NULL;
    for (int i = 0; i < f->gcount; i++) if (f->glyphs[i].codepoint == cp) return &f->glyphs[i];
    return NULL;
}
int ds_font_aw(const DSFont *f) { return f ? f->aw : 0; }
int ds_font_ah(const DSFont *f) { return f ? f->ah : 0; }
const uint8_t *ds_font_alpha(const DSFont *f) { return f ? f->alpha : NULL; }
float ds_font_lineh(const DSFont *f) { return f ? f->line_h : 0; }
float ds_font_ascent(const DSFont *f) { return f ? f->ascent : 0; }
