#include "runtime.h"

/* TrueType loader API (was ttf_font.h): the loader below is
 * embedded in this translation unit so text rendering needs no extra
 * source files. */
typedef struct DSFont DSFont;
typedef struct {
    uint32_t codepoint;
    float advance;
    float bearing_x;
    float bearing_top;
    int width;
    int height;
    float u0;
    float v0;
    float u1;
    float v1;
} DSFontGlyph;

void ds_font_destroy(DSFont *font);

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
#include <sys/types.h>

/* TrueType font loader, embedded (was a separate ttf_font.c). */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DS_FONT_ATLAS_WIDTH  1024
#define DS_FONT_ATLAS_HEIGHT 1024
#define DS_FONT_SUPERSAMPLE  4
#define DS_FONT_MAX_GLYPHS   128

#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | \
                         (uint32_t)(c) << 8 | (uint32_t)(d))

#define ARG_1_AND_2_ARE_WORDS    0x0001
#define ARGS_ARE_XY_VALUES       0x0002
#define WE_HAVE_A_SCALE          0x0008
#define MORE_COMPONENTS          0x0020
#define WE_HAVE_AN_XY_SCALE      0x0040
#define WE_HAVE_A_TWO_BY_TWO     0x0080

/* A glyph outline is kept in font units until rasterisation.  `ends` stores
 * the last point index for every contour. */
typedef struct {
    float x;
    float y;
    int on_curve;
} DSPoint;

typedef struct {
    DSPoint *points;
    int point_count;
    int point_capacity;
    int *ends;
    int contour_count;
    int contour_capacity;
} DSOutline;

typedef struct {
    float x;
    float y;
} DSFlatPoint;

typedef struct {
    DSFlatPoint *points;
    int count;
    int capacity;
} DSFlatContour;

struct DSFont {
    unsigned char *data;
    size_t size;
    int units_per_em;
    int index_to_loc_format;
    int num_glyphs;
    int num_hmetrics;
    int ascent_units;
    int descent_units;
    int line_gap_units;

    uint32_t head;
    uint32_t hhea;
    uint32_t hmtx;
    uint32_t maxp;
    uint32_t loca;
    uint32_t glyf;
    uint32_t cmap;
    uint32_t cmap4;
    uint32_t cmap12;

    float pixel_scale;
    float ascent;
    float line_height;
    int atlas_width;
    int atlas_height;
    unsigned char *atlas_alpha;
    DSFontGlyph *glyphs;
    int glyph_count;
};

static int in_range(const DSFont *font, size_t offset, size_t length) {
    return font && offset <= font->size && length <= font->size - offset;
}

static uint16_t be_u16(const DSFont *font, size_t offset) {
    if (!in_range(font, offset, 2)) return 0;
    return (uint16_t)((font->data[offset] << 8) | font->data[offset + 1]);
}

static int16_t be_s16(const DSFont *font, size_t offset) {
    return (int16_t)be_u16(font, offset);
}

static uint32_t be_u32(const DSFont *font, size_t offset) {
    if (!in_range(font, offset, 4)) return 0;
    return ((uint32_t)font->data[offset] << 24) |
           ((uint32_t)font->data[offset + 1] << 16) |
           ((uint32_t)font->data[offset + 2] << 8) |
           (uint32_t)font->data[offset + 3];
}

static int table_bounds(const DSFont *font, uint32_t tag, uint32_t *offset, uint32_t *length) {
    uint16_t count;
    size_t i;

    if (!font || !in_range(font, 0, 12)) return 0;
    count = be_u16(font, 4);
    for (i = 0; i < count; ++i) {
        size_t record = 12 + i * 16;
        if (!in_range(font, record, 16)) return 0;
        if (be_u32(font, record) == tag) {
            uint32_t at = be_u32(font, record + 8);
            uint32_t size = be_u32(font, record + 12);
            if (!in_range(font, at, size)) return 0;
            if (offset) *offset = at;
            if (length) *length = size;
            return 1;
        }
    }
    return 0;
}

static void outline_init(DSOutline *outline) {
    memset(outline, 0, sizeof(*outline));
}

static void outline_free(DSOutline *outline) {
    if (!outline) return;
    free(outline->points);
    free(outline->ends);
    memset(outline, 0, sizeof(*outline));
}

static int outline_reserve_points(DSOutline *outline, int extra) {
    int required = outline->point_count + extra;
    int capacity;
    DSPoint *points;

    if (required <= outline->point_capacity) return 1;
    capacity = outline->point_capacity ? outline->point_capacity : 32;
    while (capacity < required) {
        if (capacity > 1000000) return 0;
        capacity *= 2;
    }
    points = (DSPoint *)realloc(outline->points, (size_t)capacity * sizeof(*points));
    if (!points) return 0;
    outline->points = points;
    outline->point_capacity = capacity;
    return 1;
}

static int outline_reserve_contours(DSOutline *outline, int extra) {
    int required = outline->contour_count + extra;
    int capacity;
    int *ends;

    if (required <= outline->contour_capacity) return 1;
    capacity = outline->contour_capacity ? outline->contour_capacity : 8;
    while (capacity < required) capacity *= 2;
    ends = (int *)realloc(outline->ends, (size_t)capacity * sizeof(*ends));
    if (!ends) return 0;
    outline->ends = ends;
    outline->contour_capacity = capacity;
    return 1;
}

static int outline_add_contour(DSOutline *outline, const DSPoint *points, int count) {
    if (count <= 0 || !outline_reserve_points(outline, count) ||
        !outline_reserve_contours(outline, 1)) {
        return 0;
    }
    memcpy(outline->points + outline->point_count, points, (size_t)count * sizeof(*points));
    outline->point_count += count;
    outline->ends[outline->contour_count++] = outline->point_count - 1;
    return 1;
}

static int glyph_offsets(const DSFont *font, int glyph, uint32_t *start, uint32_t *end) {
    uint32_t a;
    uint32_t b;

    if (!font || glyph < 0 || glyph >= font->num_glyphs) return 0;
    if (font->index_to_loc_format == 0) {
        a = (uint32_t)be_u16(font, font->loca + (size_t)glyph * 2) * 2u;
        b = (uint32_t)be_u16(font, font->loca + (size_t)(glyph + 1) * 2) * 2u;
    } else {
        a = be_u32(font, font->loca + (size_t)glyph * 4);
        b = be_u32(font, font->loca + (size_t)(glyph + 1) * 4);
    }
    if (a > b || !in_range(font, (size_t)font->glyf + a, b - a)) return 0;
    if (start) *start = a;
    if (end) *end = b;
    return 1;
}

static int read_glyph_outline(
    const DSFont *font,
    int glyph,
    int depth,
    float pa, float pb, float pc, float pd, float ptx, float pty,
    DSOutline *destination
);

static int read_simple_glyph(
    const DSFont *font,
    uint32_t offset,
    int contour_count,
    float a, float b, float c, float d, float tx, float ty,
    DSOutline *destination
) {
    int *ends = NULL;
    int *flags = NULL;
    DSPoint *points = NULL;
    int point_count;
    int instruction_length;
    size_t cursor;
    int contour;
    int point;
    int x = 0;
    int y = 0;
    int ok = 0;

    if (contour_count <= 0 || contour_count > 4096) return 1;
    ends = (int *)malloc((size_t)contour_count * sizeof(*ends));
    if (!ends) goto done;
    for (contour = 0; contour < contour_count; ++contour) {
        ends[contour] = (int)be_u16(font, (size_t)font->glyf + offset + 10 + contour * 2);
    }
    point_count = ends[contour_count - 1] + 1;
    if (point_count <= 0 || point_count > 200000) goto done;
    cursor = (size_t)font->glyf + offset + 10 + (size_t)contour_count * 2;
    instruction_length = be_u16(font, cursor);
    cursor += 2 + (size_t)instruction_length;
    if (!in_range(font, cursor, 1)) goto done;

    flags = (int *)malloc((size_t)point_count * sizeof(*flags));
    points = (DSPoint *)calloc((size_t)point_count, sizeof(*points));
    if (!flags || !points) goto done;

    point = 0;
    while (point < point_count) {
        unsigned char flag;
        int repeat;
        if (!in_range(font, cursor, 1)) goto done;
        flag = font->data[cursor++];
        flags[point++] = flag;
        repeat = (flag & 8) ? (int)font->data[cursor++] : 0;
        while (repeat-- > 0 && point < point_count) flags[point++] = flag;
    }

    x = 0;
    for (point = 0; point < point_count; ++point) {
        int flag = flags[point];
        int delta = 0;
        if (flag & 2) {
            if (!in_range(font, cursor, 1)) goto done;
            delta = font->data[cursor++];
            if (!(flag & 16)) delta = -delta;
        } else if (!(flag & 16)) {
            if (!in_range(font, cursor, 2)) goto done;
            delta = be_s16(font, cursor);
            cursor += 2;
        }
        x += delta;
        points[point].x = (float)x;
        points[point].on_curve = (flag & 1) != 0;
    }

    y = 0;
    for (point = 0; point < point_count; ++point) {
        int flag = flags[point];
        int delta = 0;
        if (flag & 4) {
            if (!in_range(font, cursor, 1)) goto done;
            delta = font->data[cursor++];
            if (!(flag & 32)) delta = -delta;
        } else if (!(flag & 32)) {
            if (!in_range(font, cursor, 2)) goto done;
            delta = be_s16(font, cursor);
            cursor += 2;
        }
        y += delta;
        points[point].y = (float)y;
    }

    /* Apply the parent transform while copying each contour. */
    {
        int start = 0;
        for (contour = 0; contour < contour_count; ++contour) {
            int end = ends[contour];
            int count = end - start + 1;
            DSPoint *transformed = (DSPoint *)malloc((size_t)count * sizeof(*transformed));
            if (!transformed) goto done;
            for (point = 0; point < count; ++point) {
                DSPoint in = points[start + point];
                transformed[point].x = a * in.x + c * in.y + tx;
                transformed[point].y = b * in.x + d * in.y + ty;
                transformed[point].on_curve = in.on_curve;
            }
            if (!outline_add_contour(destination, transformed, count)) {
                free(transformed);
                goto done;
            }
            free(transformed);
            start = end + 1;
        }
    }
    ok = 1;

done:
    free(ends);
    free(flags);
    free(points);
    return ok;
}

static int read_composite_glyph(
    const DSFont *font,
    uint32_t offset,
    int depth,
    float a, float b, float c, float d, float tx, float ty,
    DSOutline *destination
) {
    size_t cursor = (size_t)font->glyf + offset + 10;
    int flags = MORE_COMPONENTS;

    while (flags & MORE_COMPONENTS) {
        int component;
        int arg1;
        int arg2;
        float ca = 1.0f, cb = 0.0f, cc = 0.0f, cd = 1.0f;
        float dx = 0.0f, dy = 0.0f;
        float na, nb, nc, nd, ntx, nty;

        if (!in_range(font, cursor, 4)) return 0;
        flags = be_u16(font, cursor);
        component = be_u16(font, cursor + 2);
        cursor += 4;
        if (flags & ARG_1_AND_2_ARE_WORDS) {
            arg1 = be_s16(font, cursor);
            arg2 = be_s16(font, cursor + 2);
            cursor += 4;
        } else {
            arg1 = (int8_t)font->data[cursor];
            arg2 = (int8_t)font->data[cursor + 1];
            cursor += 2;
        }
        if (flags & ARGS_ARE_XY_VALUES) {
            dx = (float)arg1;
            dy = (float)arg2;
        }
        if (flags & WE_HAVE_A_SCALE) {
            int16_t scale = be_s16(font, cursor);
            ca = cd = (float)scale / 16384.0f;
            cursor += 2;
        } else if (flags & WE_HAVE_AN_XY_SCALE) {
            ca = (float)be_s16(font, cursor) / 16384.0f;
            cd = (float)be_s16(font, cursor + 2) / 16384.0f;
            cursor += 4;
        } else if (flags & WE_HAVE_A_TWO_BY_TWO) {
            ca = (float)be_s16(font, cursor) / 16384.0f;
            cb = (float)be_s16(font, cursor + 2) / 16384.0f;
            cc = (float)be_s16(font, cursor + 4) / 16384.0f;
            cd = (float)be_s16(font, cursor + 6) / 16384.0f;
            cursor += 8;
        }

        /* P * C: component coordinates are in the glyph's local space. */
        na = a * ca + c * cb;
        nb = b * ca + d * cb;
        nc = a * cc + c * cd;
        nd = b * cc + d * cd;
        ntx = a * dx + c * dy + tx;
        nty = b * dx + d * dy + ty;
        if (!read_glyph_outline(font, component, depth + 1, na, nb, nc, nd, ntx, nty, destination)) {
            return 0;
        }
    }
    return 1;
}

static int read_glyph_outline(
    const DSFont *font,
    int glyph,
    int depth,
    float pa, float pb, float pc, float pd, float ptx, float pty,
    DSOutline *destination
) {
    uint32_t start;
    uint32_t end;
    int16_t contour_count;

    if (depth > 16 || !glyph_offsets(font, glyph, &start, &end)) return 0;
    if (start == end) return 1; /* whitespace and other empty glyphs */
    if (!in_range(font, (size_t)font->glyf + start, 10)) return 0;
    contour_count = be_s16(font, (size_t)font->glyf + start);
    if (contour_count >= 0) {
        return read_simple_glyph(font, start, contour_count, pa, pb, pc, pd, ptx, pty, destination);
    }
    return read_composite_glyph(font, start, depth, pa, pb, pc, pd, ptx, pty, destination);
}

static int flat_reserve(DSFlatContour *flat, int extra) {
    int required = flat->count + extra;
    int capacity;
    DSFlatPoint *points;
    if (required <= flat->capacity) return 1;
    capacity = flat->capacity ? flat->capacity : 32;
    while (capacity < required) capacity *= 2;
    points = (DSFlatPoint *)realloc(flat->points, (size_t)capacity * sizeof(*points));
    if (!points) return 0;
    flat->points = points;
    flat->capacity = capacity;
    return 1;
}

static int flat_push(DSFlatContour *flat, float x, float y) {
    if (!flat_reserve(flat, 1)) return 0;
    flat->points[flat->count].x = x;
    flat->points[flat->count].y = y;
    flat->count++;
    return 1;
}

static int flat_quad(DSFlatContour *flat, DSFlatPoint from, DSPoint control, DSFlatPoint to) {
    int step;
    for (step = 1; step <= 8; ++step) {
        float t = (float)step / 8.0f;
        float u = 1.0f - t;
        if (!flat_push(flat,
                       u * u * from.x + 2.0f * u * t * control.x + t * t * to.x,
                       u * u * from.y + 2.0f * u * t * control.y + t * t * to.y)) {
            return 0;
        }
    }
    return 1;
}

static int flatten_contour(
    const DSPoint *points, int count, DSFlatContour *flat
) {
    int first_on;
    int index;
    int processed;
    DSFlatPoint start;
    DSFlatPoint current;

    if (count <= 0) return 1;
    first_on = points[0].on_curve;
    if (first_on) {
        start.x = points[0].x;
        start.y = points[0].y;
        index = 1;
        processed = 1;
    } else if (points[count - 1].on_curve) {
        start.x = points[count - 1].x;
        start.y = points[count - 1].y;
        index = 0;
        processed = 0;
    } else {
        start.x = (points[count - 1].x + points[0].x) * 0.5f;
        start.y = (points[count - 1].y + points[0].y) * 0.5f;
        index = 0;
        processed = 0;
    }
    current = start;
    if (!flat_push(flat, start.x, start.y)) return 0;

    while (processed < count) {
        const DSPoint *one = &points[index % count];
        if (one->on_curve) {
            current.x = one->x;
            current.y = one->y;
            if (!flat_push(flat, current.x, current.y)) return 0;
            ++index;
            ++processed;
        } else {
            const DSPoint *two = &points[(index + 1) % count];
            DSFlatPoint end;
            if (two->on_curve) {
                end.x = two->x;
                end.y = two->y;
                index += 2;
                processed += 2;
            } else {
                end.x = (one->x + two->x) * 0.5f;
                end.y = (one->y + two->y) * 0.5f;
                ++index;
                ++processed;
            }
            if (!flat_quad(flat, current, *one, end)) return 0;
            current = end;
        }
    }
    if (fabsf(current.x - start.x) > 0.001f || fabsf(current.y - start.y) > 0.001f) {
        if (!flat_push(flat, start.x, start.y)) return 0;
    }
    return 1;
}

static int point_inside(const DSFlatContour *contours, int count, float x, float y) {
    int inside = 0;
    int contour;
    for (contour = 0; contour < count; ++contour) {
        const DSFlatContour *polygon = &contours[contour];
        int i;
        int j;
        for (i = 0, j = polygon->count - 1; i < polygon->count; j = i++) {
            float yi = polygon->points[i].y;
            float yj = polygon->points[j].y;
            if (((yi > y) != (yj > y)) &&
                x < (polygon->points[j].x - polygon->points[i].x) *
                        (y - yi) / (yj - yi + 0.000001f) + polygon->points[i].x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

static int font_glyph_metrics(const DSFont *font, int glyph, int *advance, int *lsb) {
    size_t offset;
    int metric;
    if (!font || glyph < 0 || glyph >= font->num_glyphs || !font->hmtx) return 0;
    metric = glyph < font->num_hmetrics ? glyph : font->num_hmetrics - 1;
    offset = (size_t)font->hmtx + (size_t)metric * 4;
    if (advance) *advance = be_u16(font, offset);
    if (lsb) {
        size_t lsb_offset = glyph < font->num_hmetrics
            ? offset + 2
            : (size_t)font->hmtx + (size_t)font->num_hmetrics * 4 +
              (size_t)(glyph - font->num_hmetrics) * 2;
        *lsb = be_s16(font, lsb_offset);
    }
    return 1;
}

static int cmap4_lookup(const DSFont *font, uint32_t codepoint) {
    uint16_t seg_count;
    uint16_t i;
    size_t base;
    if (!font->cmap4 || codepoint > 0xFFFF) return 0;
    base = font->cmap4;
    seg_count = be_u16(font, base + 6) / 2;
    for (i = 0; i < seg_count; ++i) {
        uint16_t end = be_u16(font, base + 14 + (size_t)i * 2);
        uint16_t start = be_u16(font, base + 16 + (size_t)seg_count * 2 + (size_t)i * 2);
        if (codepoint < start || codepoint > end) continue;
        {
            int16_t delta = be_s16(font, base + 16 + (size_t)seg_count * 4 + (size_t)i * 2);
            uint16_t range = be_u16(font, base + 16 + (size_t)seg_count * 6 + (size_t)i * 2);
            if (range == 0) return ((int)codepoint + delta) & 0xFFFF;
            {
                size_t range_address = base + 16 + (size_t)seg_count * 6 + (size_t)i * 2;
                size_t glyph_address = range_address + range + (codepoint - start) * 2;
                uint16_t glyph = be_u16(font, glyph_address);
                return glyph ? ((int)glyph + delta) & 0xFFFF : 0;
            }
        }
    }
    return 0;
}

static int cmap12_lookup(const DSFont *font, uint32_t codepoint) {
    uint32_t groups;
    uint32_t i;
    size_t base = font->cmap12;
    if (!base) return 0;
    groups = be_u32(font, base + 12);
    for (i = 0; i < groups; ++i) {
        size_t at = base + 16 + (size_t)i * 12;
        uint32_t first = be_u32(font, at);
        uint32_t last = be_u32(font, at + 4);
        if (codepoint >= first && codepoint <= last) {
            return (int)(be_u32(font, at + 8) + codepoint - first);
        }
    }
    return 0;
}

static int glyph_for_codepoint(const DSFont *font, uint32_t codepoint) {
    int glyph = cmap12_lookup(font, codepoint);
    if (!glyph) glyph = cmap4_lookup(font, codepoint);
    if (glyph < 0 || glyph >= font->num_glyphs) glyph = 0;
    return glyph;
}

static int utf8_next(const char **cursor) {
    const unsigned char *p = (const unsigned char *)*cursor;
    uint32_t codepoint;
    if (!p || !*p) return -1;
    if (*p < 0x80) {
        codepoint = *p++;
    } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        codepoint = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
        p += 2;
    } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80) {
        codepoint = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3;
    } else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        codepoint = ((*p & 7) << 18) | ((p[1] & 0x3F) << 12) |
                    ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        p += 4;
    } else {
        codepoint = *p++;
    }
    *cursor = (const char *)p;
    return (int)codepoint;
}

static DSFontGlyph *find_glyph(DSFont *font, uint32_t codepoint) {
    int i;
    for (i = 0; i < font->glyph_count; ++i) {
        if (font->glyphs[i].codepoint == codepoint) return &font->glyphs[i];
    }
    return NULL;
}

static int bake_glyph(DSFont *font, DSFontGlyph *glyph, int atlas_x, int atlas_y, int row_height) {
    int glyph_index = glyph_for_codepoint(font, glyph->codepoint);
    int advance_units = 0;
    DSOutline outline;
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    int has_point = 0;
    int contour;
    int start = 0;
    int width;
    int height;
    DSFlatContour *flat = NULL;
    int flat_count = 0;
    float scale = font->pixel_scale;

    font_glyph_metrics(font, glyph_index, &advance_units, NULL);
    glyph->advance = advance_units * scale;
    glyph->bearing_x = 0.0f;
    glyph->bearing_top = 0.0f;
    glyph->width = 0;
    glyph->height = 0;
    glyph->u0 = glyph->u1 = (float)atlas_x / (float)font->atlas_width;
    glyph->v0 = glyph->v1 = (float)atlas_y / (float)font->atlas_height;

    outline_init(&outline);
    if (!read_glyph_outline(font, glyph_index, 0, 1, 0, 0, 1, 0, 0, &outline)) {
        outline_free(&outline);
        return row_height;
    }
    for (contour = 0; contour < outline.contour_count; ++contour) {
        int end = outline.ends[contour];
        int point;
        for (point = start; point <= end; ++point) {
            int x = (int)lrintf(outline.points[point].x);
            int y = (int)lrintf(outline.points[point].y);
            if (!has_point || x < min_x) min_x = x;
            if (!has_point || x > max_x) max_x = x;
            if (!has_point || y < min_y) min_y = y;
            if (!has_point || y > max_y) max_y = y;
            has_point = 1;
        }
        start = end + 1;
    }
    if (!has_point) {
        outline_free(&outline);
        return row_height;
    }

    width = (int)ceilf((max_x - min_x) * scale) + 2;
    height = (int)ceilf((max_y - min_y) * scale) + 2;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    glyph->bearing_x = min_x * scale;
    glyph->bearing_top = max_y * scale;
    glyph->width = width;
    glyph->height = height;

    flat = (DSFlatContour *)calloc((size_t)outline.contour_count, sizeof(*flat));
    if (!flat) {
        outline_free(&outline);
        return row_height;
    }
    start = 0;
    for (contour = 0; contour < outline.contour_count; ++contour) {
        int end = outline.ends[contour];
        if (!flatten_contour(outline.points + start, end - start + 1, &flat[flat_count])) {
            int i;
            for (i = 0; i <= flat_count; ++i) free(flat[i].points);
            free(flat);
            outline_free(&outline);
            return row_height;
        }
        ++flat_count;
        start = end + 1;
    }

    {
        int py;
        for (py = 0; py < height; ++py) {
            int px;
            for (px = 0; px < width; ++px) {
                int covered = 0;
                int sy;
                for (sy = 0; sy < DS_FONT_SUPERSAMPLE; ++sy) {
                    int sx;
                    for (sx = 0; sx < DS_FONT_SUPERSAMPLE; ++sx) {
                        float fx = min_x + ((float)px + ((float)sx + 0.5f) / DS_FONT_SUPERSAMPLE) / scale;
                        float fy = max_y - ((float)py + ((float)sy + 0.5f) / DS_FONT_SUPERSAMPLE) / scale;
                        if (point_inside(flat, flat_count, fx, fy)) ++covered;
                    }
                }
                font->atlas_alpha[(size_t)(atlas_y + py) * (size_t)font->atlas_width +
                                  (size_t)(atlas_x + px)] =
                    (unsigned char)((covered * 255) / (DS_FONT_SUPERSAMPLE * DS_FONT_SUPERSAMPLE));
            }
        }
    }

    glyph->u0 = (float)atlas_x / (float)font->atlas_width;
    glyph->v0 = (float)atlas_y / (float)font->atlas_height;
    glyph->u1 = (float)(atlas_x + width) / (float)font->atlas_width;
    glyph->v1 = (float)(atlas_y + height) / (float)font->atlas_height;

    for (contour = 0; contour < flat_count; ++contour) free(flat[contour].points);
    free(flat);
    outline_free(&outline);
    return height > row_height ? height : row_height;
}

static int initialise_tables(DSFont *font) {
    uint32_t length;
    uint16_t cmap_records;
    uint16_t i;

    if (!table_bounds(font, TAG('h','e','a','d'), &font->head, &length) || length < 54 ||
        !table_bounds(font, TAG('h','h','e','a'), &font->hhea, &length) || length < 36 ||
        !table_bounds(font, TAG('h','m','t','x'), &font->hmtx, &length) ||
        !table_bounds(font, TAG('m','a','x','p'), &font->maxp, &length) || length < 6 ||
        !table_bounds(font, TAG('l','o','c','a'), &font->loca, &length) ||
        !table_bounds(font, TAG('g','l','y','f'), &font->glyf, &length) ||
        !table_bounds(font, TAG('c','m','a','p'), &font->cmap, &length) || length < 4) {
        return 0;
    }

    font->units_per_em = be_u16(font, font->head + 18);
    font->index_to_loc_format = be_s16(font, font->head + 50);
    font->num_glyphs = be_u16(font, font->maxp + 4);
    font->num_hmetrics = be_u16(font, font->hhea + 34);
    font->ascent_units = be_s16(font, font->hhea + 4);
    font->descent_units = be_s16(font, font->hhea + 6);
    font->line_gap_units = be_s16(font, font->hhea + 8);
    if (font->units_per_em <= 0 || font->num_glyphs <= 0 || font->num_hmetrics <= 0) return 0;

    cmap_records = be_u16(font, font->cmap + 2);
    for (i = 0; i < cmap_records; ++i) {
        size_t record = font->cmap + 4 + (size_t)i * 8;
        uint16_t platform = be_u16(font, record);
        uint16_t encoding = be_u16(font, record + 2);
        uint32_t subtable = font->cmap + be_u32(font, record + 4);
        uint16_t format = be_u16(font, subtable);
        if (format == 12 && (platform == 3 || platform == 0)) {
            if (!font->cmap12 || (platform == 3 && encoding == 10)) font->cmap12 = subtable;
        } else if (format == 4 && (platform == 3 || platform == 0)) {
            if (!font->cmap4 || (platform == 3 && encoding == 1)) font->cmap4 = subtable;
        }
    }
    return font->cmap4 || font->cmap12;
}

static int add_codepoint(DSFont *font, uint32_t codepoint) {
    if (font->glyph_count >= DS_FONT_MAX_GLYPHS || find_glyph(font, codepoint)) return 1;
    font->glyphs[font->glyph_count].codepoint = codepoint;
    ++font->glyph_count;
    return 1;
}

DSFont *ds_font_create(const unsigned char *data, size_t size, int pixel_height) {
    DSFont *font;
    int codepoint;
    int atlas_x = 1;
    int row_height = 0;
    int atlas_y = 1;
    int i;
    static const uint32_t extra_codepoints[] = {
        /* only the glyphs the game text uses: ◀ ▶ */
        0x25C0, 0x25B6, 0
    };

    if (!data || size < 12 || pixel_height <= 0 || pixel_height > 256) return NULL;
    font = (DSFont *)calloc(1, sizeof(*font));
    if (!font) return NULL;
    font->data = (unsigned char *)malloc(size);
    font->glyphs = (DSFontGlyph *)calloc(DS_FONT_MAX_GLYPHS, sizeof(*font->glyphs));
    font->atlas_alpha = (unsigned char *)calloc((size_t)DS_FONT_ATLAS_WIDTH * DS_FONT_ATLAS_HEIGHT, 1);
    if (!font->data || !font->glyphs || !font->atlas_alpha) {
        ds_font_destroy(font);
        return NULL;
    }
    memcpy(font->data, data, size);
    font->size = size;
    font->atlas_width = DS_FONT_ATLAS_WIDTH;
    font->atlas_height = DS_FONT_ATLAS_HEIGHT;
    if (!initialise_tables(font)) {
        ds_font_destroy(font);
        return NULL;
    }
    font->pixel_scale = (float)pixel_height / (float)font->units_per_em;
    font->ascent = font->ascent_units * font->pixel_scale;
    font->line_height = (font->ascent_units - font->descent_units + font->line_gap_units) * font->pixel_scale;
    if (font->line_height < pixel_height) font->line_height = (float)pixel_height;

    for (codepoint = 32; codepoint <= 126; ++codepoint) add_codepoint(font, (uint32_t)codepoint);
    for (i = 0; extra_codepoints[i]; ++i) add_codepoint(font, extra_codepoints[i]);
    add_codepoint(font, '?');

    for (i = 0; i < font->glyph_count; ++i) {
        DSFontGlyph *glyph = &font->glyphs[i];
        int glyph_width = 0;
        int next_row;
        /* A conservative cell estimate keeps atlas packing deterministic. */
        int glyph_index = glyph_for_codepoint(font, glyph->codepoint);
        DSOutline outline;
        int start = 0;
        int min_x = 0, max_x = 0;
        int has_point = 0;
        outline_init(&outline);
        if (read_glyph_outline(font, glyph_index, 0, 1, 0, 0, 1, 0, 0, &outline)) {
            int contour;
            for (contour = 0; contour < outline.contour_count; ++contour) {
                int end = outline.ends[contour];
                int p;
                for (p = start; p <= end; ++p) {
                    int x = (int)lrintf(outline.points[p].x);
                    if (!has_point || x < min_x) min_x = x;
                    if (!has_point || x > max_x) max_x = x;
                    has_point = 1;
                }
                start = end + 1;
            }
            if (has_point) glyph_width = (int)ceilf((max_x - min_x) * font->pixel_scale) + 2;
        }
        outline_free(&outline);
        if (glyph_width < 1) glyph_width = 1;
        if (atlas_x + glyph_width + 1 >= font->atlas_width) {
            atlas_x = 1;
            atlas_y += row_height + 1;
            row_height = 0;
        }
        if (atlas_y + pixel_height + 2 >= font->atlas_height) {
            ds_font_destroy(font);
            return NULL;
        }
        next_row = bake_glyph(font, glyph, atlas_x, atlas_y, row_height);
        atlas_x += glyph->width + 1;
        if (next_row > row_height) row_height = next_row;
    }
    return font;
}

void ds_font_destroy(DSFont *font) {
    if (!font) return;
    free(font->data);
    free(font->atlas_alpha);
    free(font->glyphs);
    free(font);
}

int ds_font_atlas_width(const DSFont *font) { return font ? font->atlas_width : 0; }
int ds_font_atlas_height(const DSFont *font) { return font ? font->atlas_height : 0; }
const unsigned char *ds_font_atlas_alpha(const DSFont *font) { return font ? font->atlas_alpha : NULL; }

const DSFontGlyph *ds_font_glyph(const DSFont *font, uint32_t codepoint) {
    int i;
    const DSFontGlyph *fallback = NULL;
    if (!font) return NULL;
    for (i = 0; i < font->glyph_count; ++i) {
        if (font->glyphs[i].codepoint == codepoint) return &font->glyphs[i];
        if (font->glyphs[i].codepoint == '?') fallback = &font->glyphs[i];
    }
    return fallback;
}

float ds_font_line_height(const DSFont *font) { return font ? font->line_height : 0.0f; }
float ds_font_ascent(const DSFont *font) { return font ? font->ascent : 0.0f; }

float ds_font_measure(const DSFont *font, const char *utf8) {
    const char *cursor = utf8;
    float width = 0.0f;
    float line_width = 0.0f;
    if (!font || !utf8) return 0.0f;
    while (*cursor) {
        int codepoint = utf8_next(&cursor);
        const DSFontGlyph *glyph;
        if (codepoint == '\n') {
            if (line_width > width) width = line_width;
            line_width = 0.0f;
            continue;
        }
        glyph = ds_font_glyph(font, (uint32_t)codepoint);
        if (!glyph) continue;
        line_width += glyph->advance;
    }
    if (line_width > width) width = line_width;
    return width;
}


/*
 * DimScript software renderer
 * ---------------------------
 *
 * The Android window is locked only once per frame by main.c.  Script calls
 * append high-level commands to a small frame list; the list is then rasterised
 * into the window in draw order.  This keeps the script API independent from
 * the window stride and lets the software path optimise spans, clears and
 * opaque texture rows instead of running a generic per-pixel operation for
 * every primitive.
 *
 * The TTF atlas is generated once at load time.  Text rendering still blends
 * glyph coverage into the software frame, but no font outline is parsed and no
 * glyph bitmap is rebuilt during a frame.
 */

typedef struct Texture Texture;
struct Texture {
    Texture *next;
    char *name;
    int width;
    int height;
    int opaque;
    uint32_t *pixels; /* packed as RGBA bytes in little-endian memory */
};

typedef enum {
    DS_CMD_CLEAR,
    DS_CMD_RECT,
    DS_CMD_ROUNDRECT,
    DS_CMD_CIRCLE,
    DS_CMD_RING,
    DS_CMD_TEXTURE,
    DS_CMD_TEXT
} DSCommandType;

typedef struct {
    DSCommandType type;
    union {
        struct { uint32_t colour; } clear;
        struct { float x, y, width, height; uint32_t colour; } rect;
        struct { float x, y, width, height, radius; uint32_t colour; } roundrect;
        struct { float x, y, radius; uint32_t colour; } circle;
        struct { float x, y, radius, thickness; uint32_t colour; } ring;
        struct { float x, y, angle, scale; Texture *texture; } texture;
        struct { const char *string; float x, y, scale; uint32_t colour; } text;
    } value;
} DSCommand;

static Buffer *current_buffer = NULL;
static DSCommand *commands = NULL;
static size_t command_count = 0;
static size_t command_capacity = 0;
static int frame_open = 0;

static AAssetManager *asset_manager = NULL;
static Texture *textures = NULL;
static DSFont *font = NULL;
static int font_attempted = 0;

static void graphics_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_ERROR, "DimScript.Graphics", format, args);
    va_end(args);
}

static char *graphics_strdup(const char *source) {
    size_t length;
    char *copy;
    if (!source) return NULL;
    length = strlen(source) + 1;
    copy = (char *)malloc(length);
    if (copy) memcpy(copy, source, length);
    return copy;
}

static const char *normalise_asset_name(const char *name) {
    const char *cursor;
    if (!name) return NULL;
    while (strncmp(name, "./", 2) == 0) name += 2;
    if (strncmp(name, "game/assets/", 12) == 0) name += 12;
    else if (strncmp(name, "assets/", 7) == 0) name += 7;
    if (!*name || *name == '/' || strchr(name, '\\')) return NULL;
    cursor = name;
    while (*cursor) {
        const char *segment = cursor;
        while (*cursor && *cursor != '/') ++cursor;
        if (cursor - segment == 2 && segment[0] == '.' && segment[1] == '.') return NULL;
        if (*cursor == '/') ++cursor;
    }
    return name;
}

static uint32_t pack_colour(uint32_t colour) {
    unsigned int alpha = (colour >> 24) & 0xffu;
    unsigned int red = (colour >> 16) & 0xffu;
    unsigned int green = (colour >> 8) & 0xffu;
    unsigned int blue = colour & 0xffu;
    if (alpha == 0) alpha = 255;
    /* Android RGBA_8888 is R,G,B,A in memory on the ARM little-endian
     * targets used by the project. */
    return red | (green << 8) | (blue << 16) | (alpha << 24);
}

static unsigned int channel_red(uint32_t colour) { return colour & 0xffu; }
static unsigned int channel_green(uint32_t colour) { return (colour >> 8) & 0xffu; }
static unsigned int channel_blue(uint32_t colour) { return (colour >> 16) & 0xffu; }
static unsigned int channel_alpha(uint32_t colour) { return (colour >> 24) & 0xffu; }

static uint32_t blend_pixel(uint32_t destination, uint32_t source) {
    unsigned int alpha = channel_alpha(source);
    unsigned int inverse;
    unsigned int red;
    unsigned int green;
    unsigned int blue;
    unsigned int out_alpha;
    if (alpha == 0) return destination;
    if (alpha == 255) return source;
    inverse = 255 - alpha;
    red = (channel_red(source) * alpha + channel_red(destination) * inverse + 127) / 255;
    green = (channel_green(source) * alpha + channel_green(destination) * inverse + 127) / 255;
    blue = (channel_blue(source) * alpha + channel_blue(destination) * inverse + 127) / 255;
    out_alpha = alpha + (channel_alpha(destination) * inverse + 127) / 255;
    return red | (green << 8) | (blue << 16) | (out_alpha << 24);
}

static uint32_t blend_coverage(uint32_t destination, uint32_t colour, unsigned int coverage) {
    unsigned int alpha;
    if (coverage == 0) return destination;
    alpha = channel_alpha(colour) * coverage / 255;
    return blend_pixel(destination, (colour & 0x00ffffffu) | (alpha << 24));
}

static void fill_span(uint32_t *destination, int count, uint32_t colour) {
    /* A tight unrolled fill is substantially faster than a branch-heavy
     * pixel helper while remaining valid for any 4-byte alignment. */
    while (count >= 8) {
        destination[0] = colour;
        destination[1] = colour;
        destination[2] = colour;
        destination[3] = colour;
        destination[4] = colour;
        destination[5] = colour;
        destination[6] = colour;
        destination[7] = colour;
        destination += 8;
        count -= 8;
    }
    while (count-- > 0) *destination++ = colour;
}

static void clear_buffer(Buffer *buffer, uint32_t colour) {
    int y;
    if (!buffer || !buffer->pixels || buffer->width <= 0 || buffer->height <= 0 ||
        buffer->stride < buffer->width) return;
    for (y = 0; y < buffer->height; ++y) {
        fill_span(buffer->pixels + y * buffer->stride, buffer->width, colour);
    }
}

static int clamp_floor(float value, int limit) {
    int result;
    if (value <= 0.0f) return 0;
    if (value >= (float)limit) return limit;
    result = (int)floorf(value);
    if (result < 0) return 0;
    if (result > limit) return limit;
    return result;
}

static int clamp_ceil(float value, int limit) {
    int result;
    if (value <= 0.0f) return 0;
    if (value >= (float)limit) return limit;
    result = (int)ceilf(value);
    if (result < 0) return 0;
    if (result > limit) return limit;
    return result;
}

static void render_rect(Buffer *buffer, float x, float y, float width, float height, uint32_t colour) {
    int left;
    int top;
    int right;
    int bottom;
    int row;
    if (!buffer || !isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) ||
        width <= 0.0f || height <= 0.0f) return;
    if (x >= buffer->width || y >= buffer->height || x + width <= 0.0f || y + height <= 0.0f) return;
    left = clamp_floor(floorf(x), buffer->width);
    top = clamp_floor(floorf(y), buffer->height);
    right = clamp_ceil(ceilf(x + width), buffer->width);
    bottom = clamp_ceil(ceilf(y + height), buffer->height);
    if (channel_alpha(colour) >= 255) {
        /* Fully opaque: the tight unrolled fill. */
        for (row = top; row < bottom; ++row) {
            fill_span(buffer->pixels + row * buffer->stride + left, right - left, colour);
        }
    } else {
        /* Semi-transparent (e.g. the scene-transition fade overlay):
         * blend every covered pixel instead of overwriting it. */
        for (row = top; row < bottom; ++row) {
            uint32_t *destination = buffer->pixels + row * buffer->stride + left;
            int column;
            for (column = 0; column < right - left; ++column) {
                destination[column] = blend_pixel(destination[column], colour);
            }
        }
    }
}

static void render_roundrect(Buffer *buffer, float x, float y, float width, float height,
                             float radius, uint32_t colour) {
    float r;
    int row;
    if (!buffer || !isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) ||
        !isfinite(radius) || width <= 0.0f || height <= 0.0f) return;
    if (x >= buffer->width || y >= buffer->height || x + width <= 0.0f || y + height <= 0.0f) return;
    r = radius;
    if (r < 0.0f) r = 0.0f;
    if (r > width * 0.5f) r = width * 0.5f;
    if (r > height * 0.5f) r = height * 0.5f;
    for (row = clamp_floor(floorf(y), buffer->height);
         row < clamp_ceil(ceilf(y + height), buffer->height); ++row) {
        float y_in = (float)row + 0.5f - y;
        float inset = 0.0f;
        int start;
        int end;
        if (r > 0.0f) {
            if (y_in < r) {
                float d = r - y_in;
                if (d > r) d = r;
                inset = r - sqrtf(r * r - d * d);
            } else if (y_in > height - r) {
                float d = y_in - (height - r);
                if (d > r) d = r;
                inset = r - sqrtf(r * r - d * d);
            }
        }
        start = (int)ceilf(x + inset);
        if (start < 0) start = 0;
        end = (int)ceilf(x + width - inset);
        if (end > buffer->width) end = buffer->width;
        if (end > start) {
            fill_span(buffer->pixels + row * buffer->stride + start, end - start, colour);
        }
    }
}

static void render_circle(Buffer *buffer, float x, float y, float radius, uint32_t colour) {
    int r;
    int centre_x;
    int centre_y;
    int dy;
    long long radius_squared;
    if (!buffer || !isfinite(x) || !isfinite(y) || !isfinite(radius) || radius <= 0.0f) return;
    r = (int)ceilf(radius);
    if (r <= 0) return;
    centre_x = (int)floorf(x + 0.5f);
    centre_y = (int)floorf(y + 0.5f);
    radius_squared = (long long)r * r;
    for (dy = -r; dy <= r; ++dy) {
        int screen_y = centre_y + dy;
        int half_width;
        int left;
        int right;
        if (screen_y < 0 || screen_y >= buffer->height) continue;
        half_width = (int)sqrt((double)(radius_squared - (long long)dy * dy));
        left = centre_x - half_width;
        right = centre_x + half_width + 1;
        if (left < 0) left = 0;
        if (right > buffer->width) right = buffer->width;
        if (left < right) fill_span(buffer->pixels + screen_y * buffer->stride + left,
                                     right - left, colour);
    }
}

static void render_ring(Buffer *buffer, float x, float y, float radius, float thickness,
                        uint32_t colour) {
    int outer;
    int inner;
    int centre_x;
    int centre_y;
    int dy;
    long long outer_squared;
    long long inner_squared;
    if (!buffer || !isfinite(x) || !isfinite(y) || !isfinite(radius) ||
        !isfinite(thickness) || radius <= 0.0f || thickness <= 0.0f) return;
    outer = (int)ceilf(radius);
    inner = (int)floorf(radius - thickness);
    if (inner <= 0) {
        render_circle(buffer, x, y, radius, colour);
        return;
    }
    centre_x = (int)floorf(x + 0.5f);
    centre_y = (int)floorf(y + 0.5f);
    outer_squared = (long long)outer * outer;
    inner_squared = (long long)inner * inner;
    for (dy = -outer; dy <= outer; ++dy) {
        int screen_y = centre_y + dy;
        int outer_half;
        int inner_half = -1;
        int left;
        int right;
        if (screen_y < 0 || screen_y >= buffer->height) continue;
        outer_half = (int)sqrt((double)(outer_squared - (long long)dy * dy));
        if (abs(dy) <= inner) {
            inner_half = (int)sqrt((double)(inner_squared - (long long)dy * dy));
        }
        left = centre_x - outer_half;
        right = centre_x + outer_half + 1;
        if (left < 0) left = 0;
        if (right > buffer->width) right = buffer->width;
        if (inner_half < 0) {
            if (left < right) fill_span(buffer->pixels + screen_y * buffer->stride + left,
                                         right - left, colour);
        } else {
            int inner_left = centre_x - inner_half;
            int inner_right = centre_x + inner_half + 1;
            int left_right = inner_left < right ? inner_left : right;
            int right_left = inner_right > left ? inner_right : left;
            if (left < left_right) fill_span(buffer->pixels + screen_y * buffer->stride + left,
                                             left_right - left, colour);
            if (right_left < right) fill_span(buffer->pixels + screen_y * buffer->stride + right_left,
                                               right - right_left, colour);
        }
    }
}

static Texture *find_texture(const char *name) {
    Texture *texture = textures;
    while (texture) {
        if (strcmp(texture->name, name) == 0) return texture;
        texture = texture->next;
    }
    return NULL;
}

static int open_asset(const char *name, unsigned char **data, size_t *size) {
    AAsset *asset;
    off_t length;
    size_t offset = 0;
    unsigned char *copy;
    if (!asset_manager || !data || !size) return 0;
    asset = AAssetManager_open(asset_manager, name, AASSET_MODE_BUFFER);
    if (!asset) return 0;
    length = AAsset_getLength(asset);
    if (length <= 0 || (uint64_t)length > (uint64_t)SIZE_MAX) {
        AAsset_close(asset);
        return 0;
    }
    copy = (unsigned char *)malloc((size_t)length);
    if (!copy) {
        AAsset_close(asset);
        return 0;
    }
    while (offset < (size_t)length) {
        int read_count = AAsset_read(asset, copy + offset, (size_t)length - offset);
        if (read_count <= 0) break;
        offset += (size_t)read_count;
    }
    AAsset_close(asset);
    if (offset != (size_t)length) {
        free(copy);
        return 0;
    }
    *data = copy;
    *size = (size_t)length;
    return 1;
}

static Texture *load_png_texture(const char *requested_name) {
    const char *name = normalise_asset_name(requested_name);
    Texture *texture;
    unsigned char *encoded = NULL;
    size_t encoded_size = 0;
    stbi_uc *decoded;
    int channels = 0;
    size_t pixel_count;
    size_t i;

    if (!name) {
        ds_runtime_error("invalid PNG asset path: '%s'", requested_name ? requested_name : "(null)");
        return NULL;
    }
    texture = find_texture(name);
    if (texture) return texture->pixels ? texture : NULL;
    if (!asset_manager) {
        ds_runtime_error("cannot load PNG '%s' without an Android asset manager", name);
        return NULL;
    }
    texture = (Texture *)calloc(1, sizeof(*texture));
    if (!texture) {
        ds_runtime_error("out of memory while caching PNG '%s'", name);
        return NULL;
    }
    texture->name = graphics_strdup(name);
    if (!texture->name) {
        free(texture);
        ds_runtime_error("out of memory while caching PNG name '%s'", name);
        return NULL;
    }
    texture->next = textures;
    textures = texture;
    if (!open_asset(name, &encoded, &encoded_size) || encoded_size > (size_t)INT_MAX) {
        free(encoded);
        ds_runtime_error("PNG asset not found or unreadable: '%s'", name);
        return NULL;
    }
    decoded = stbi_load_from_memory(encoded, (int)encoded_size, &texture->width, &texture->height,
                                    &channels, STBI_rgb_alpha);
    free(encoded);
    if (!decoded || texture->width <= 0 || texture->height <= 0) {
        ds_runtime_error("could not decode PNG '%s': %s", name,
                         stbi_failure_reason() ? stbi_failure_reason() : "unknown PNG error");
        stbi_image_free(decoded);
        return NULL;
    }
    pixel_count = (size_t)texture->width * (size_t)texture->height;
    texture->pixels = (uint32_t *)malloc(pixel_count * sizeof(*texture->pixels));
    if (!texture->pixels) {
        stbi_image_free(decoded);
        ds_runtime_error("out of memory while uploading PNG '%s'", name);
        return NULL;
    }
    texture->opaque = 1;
    for (i = 0; i < pixel_count; ++i) {
        const stbi_uc *source = decoded + i * 4;
        unsigned int alpha = source[3];
        texture->pixels[i] = source[0] | ((unsigned int)source[1] << 8) |
                             ((unsigned int)source[2] << 16) | (alpha << 24);
        if (alpha != 255) texture->opaque = 0;
    }
    stbi_image_free(decoded);
    ds_log("loaded PNG asset '%s' (%dx%d) for software renderer",
           name, texture->width, texture->height);
    return texture;
}

static int ensure_font(void) {
    unsigned char *data = NULL;
    size_t size = 0;
    if (font) return 1;
    if (font_attempted) return 0;
    font_attempted = 1;
    if (!open_asset("fonts/ChillRoundGothic_Heavy.ttf", &data, &size)) {
        ds_runtime_error("TTF asset not found: fonts/ChillRoundGothic_Heavy.ttf (put a TTF in game/assets/fonts)");
        return 0;
    }
    font = ds_font_create(data, size, 32);
    free(data);
    if (!font) {
        ds_runtime_error("could not parse TrueType font fonts/ChillRoundGothic_Heavy.ttf");
        return 0;
    }
    return 1;
}

static DSCommand *command_push(DSCommandType type) {
    DSCommand *command;
    if (!frame_open) return NULL;
    if (command_count == command_capacity) {
        size_t capacity = command_capacity ? command_capacity * 2 : 256;
        DSCommand *new_commands;
        if (capacity < command_count || capacity > SIZE_MAX / sizeof(*commands)) {
            ds_runtime_error("too many software-renderer commands in one frame");
            return NULL;
        }
        new_commands = (DSCommand *)realloc(commands, capacity * sizeof(*commands));
        if (!new_commands) {
            ds_runtime_error("out of memory while building the software frame");
            return NULL;
        }
        commands = new_commands;
        command_capacity = capacity;
    }
    command = &commands[command_count++];
    memset(command, 0, sizeof(*command));
    command->type = type;
    return command;
}

void ds_release_assets(void) {
    Texture *texture = textures;
    while (texture) {
        Texture *next = texture->next;
        free(texture->pixels);
        free(texture->name);
        free(texture);
        texture = next;
    }
    textures = NULL;
    ds_font_destroy(font);
    font = NULL;
    font_attempted = 0;
    asset_manager = NULL;
}

void ds_set_asset_manager(AAssetManager *assets) {
    if (asset_manager != assets) {
        ds_release_assets();
        asset_manager = assets;
    }
    if (!asset_manager) ds_runtime_error("Android asset manager is unavailable");
}

int png_load(const char *name) {
    return load_png_texture(name) != NULL;
}

void cls(uint32_t colour) {
    DSCommand *command = command_push(DS_CMD_CLEAR);
    if (command) command->value.clear.colour = pack_colour(colour);
}

void rect(float x, float y, float width, float height, uint32_t colour) {
    DSCommand *command = command_push(DS_CMD_RECT);
    if (!command) return;
    command->value.rect.x = x;
    command->value.rect.y = y;
    command->value.rect.width = width;
    command->value.rect.height = height;
    command->value.rect.colour = pack_colour(colour);
}

void roundrect(float x, float y, float width, float height, float radius, uint32_t colour) {
    DSCommand *command = command_push(DS_CMD_ROUNDRECT);
    if (!command) return;
    command->value.roundrect.x = x;
    command->value.roundrect.y = y;
    command->value.roundrect.width = width;
    command->value.roundrect.height = height;
    command->value.roundrect.radius = radius;
    command->value.roundrect.colour = pack_colour(colour);
}

void circle(float x, float y, float radius, uint32_t colour) {
    DSCommand *command = command_push(DS_CMD_CIRCLE);
    if (!command) return;
    command->value.circle.x = x;
    command->value.circle.y = y;
    command->value.circle.radius = radius;
    command->value.circle.colour = pack_colour(colour);
}

void ring(float x, float y, float radius, float thickness, uint32_t colour) {
    DSCommand *command = command_push(DS_CMD_RING);
    if (!command) return;
    command->value.ring.x = x;
    command->value.ring.y = y;
    command->value.ring.radius = radius;
    command->value.ring.thickness = thickness;
    command->value.ring.colour = pack_colour(colour);
}

void tex(float x, float y, const char *name, float angle, float scale) {
    DSCommand *command;
    Texture *texture;
    if (!frame_open) return;
    texture = load_png_texture(name);
    if (!texture) return;
    command = command_push(DS_CMD_TEXTURE);
    if (!command) return;
    command->value.texture.x = x;
    command->value.texture.y = y;
    command->value.texture.angle = angle;
    command->value.texture.scale = scale;
    command->value.texture.texture = texture;
}

static int utf8_next_graphics(const char **cursor) {
    const unsigned char *p = (const unsigned char *)*cursor;
    int result;
    if (!p || !*p) return -1;
    if (*p < 0x80) result = *p++;
    else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
        result = ((*p & 0x1f) << 6) | (p[1] & 0x3f); p += 2;
    } else if ((*p & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        result = ((*p & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); p += 3;
    } else if ((*p & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 &&
               (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        result = ((*p & 7) << 18) | ((p[1] & 0x3f) << 12) |
                 ((p[2] & 0x3f) << 6) | (p[3] & 0x3f); p += 4;
    } else result = *p++;
    *cursor = (const char *)p;
    return result;
}

static void blend_text_pixel(uint32_t *destination, uint32_t colour, unsigned int coverage) {
    *destination = blend_coverage(*destination, colour, coverage);
}

static void render_text_now(Buffer *buffer, const char *string, float x, float y,
                            uint32_t colour, float scale) {
    const char *cursor;
    float pen_x;
    float baseline;
    int atlas_width;
    int atlas_height;
    const unsigned char *atlas;
    float cap_ascent;
    float left_bearing;
    const DSFontGlyph *ref_glyph;
    if (!buffer || !font || !string || !isfinite(x) || !isfinite(y) ||
        !isfinite(scale) || scale <= 0.0f) return;
    atlas_width = ds_font_atlas_width(font);
    atlas_height = ds_font_atlas_height(font);
    atlas = ds_font_atlas_alpha(font);
    cap_ascent = ds_font_ascent(font);
    left_bearing = 0.0f;
    ref_glyph = ds_font_glyph(font, 'S');
    if (ref_glyph) {
        cap_ascent = ref_glyph->bearing_top;
        left_bearing = ref_glyph->bearing_x;
    }
    pen_x = x - left_bearing * scale;
    baseline = y + cap_ascent * scale;
    cursor = string;
    while (*cursor) {
        int codepoint = utf8_next_graphics(&cursor);
        const DSFontGlyph *glyph;
        int source_x;
        int source_y;
        int dest_width;
        int dest_height;
        int dest_x;
        int dest_y;
        int sy;
        if (codepoint == '\n') {
            pen_x = x - left_bearing * scale;
            baseline += ds_font_line_height(font) * scale;
            continue;
        }
        glyph = ds_font_glyph(font, (uint32_t)codepoint);
        if (!glyph) continue;
        source_x = (int)floorf(glyph->u0 * atlas_width + 0.5f);
        source_y = (int)floorf(glyph->v0 * atlas_height + 0.5f);
        dest_width = (int)ceilf(glyph->width * scale);
        dest_height = (int)ceilf(glyph->height * scale);
        dest_x = (int)floorf(pen_x + glyph->bearing_x * scale);
        dest_y = (int)floorf(baseline - glyph->bearing_top * scale);
        for (sy = 0; sy < dest_height; ++sy) {
            int screen_y = dest_y + sy;
            int source_row;
            int sx;
            if (screen_y < 0 || screen_y >= buffer->height) continue;
            source_row = source_y + (int)(sy / scale);
            if (source_row < 0 || source_row >= atlas_height) continue;
            for (sx = 0; sx < dest_width; ++sx) {
                int screen_x = dest_x + sx;
                int source_column;
                unsigned int coverage;
                if (screen_x < 0 || screen_x >= buffer->width) continue;
                source_column = source_x + (int)(sx / scale);
                if (source_column < 0 || source_column >= atlas_width) continue;
                coverage = atlas[source_row * atlas_width + source_column];
                if (coverage) {
                    blend_text_pixel(buffer->pixels + screen_y * buffer->stride + screen_x,
                                     colour, coverage);
                }
            }
        }
        pen_x += glyph->advance * scale;
    }
}

void text(const char *string, float x, float y, uint32_t colour) {
    text_scaled(string, x, y, colour, 1.0f);
}

void text_scaled(const char *string, float x, float y, uint32_t colour, float scale) {
    DSCommand *command;
    if (!frame_open || !string || !ensure_font()) return;
    command = command_push(DS_CMD_TEXT);
    if (!command) return;
    command->value.text.string = string;
    command->value.text.x = x;
    command->value.text.y = y;
    command->value.text.scale = scale;
    command->value.text.colour = pack_colour(colour);
}

int text_width(const char *string) {
    if (!string || !ensure_font()) return 0;
    return (int)(ds_font_measure(font, string) + 0.5f);
}

/* Ink bounding box of a string, mirroring exactly how draw_text positions
 * glyphs (pen starts at x - 'S'.bearing_x, baseline at y + 'S'.bearing_top).
 * text_width() sums advances, which is what the advance-based layout needs,
 * but it is not the visible width: centring a label with it leaves an error
 * equal to the side bearings.  These two functions return the real ink box,
 * so `screen_w / 2 - text_ink_width(label) / 2` centres the visible text. */
int text_ink_width(const char *string) {
    const char *cursor;
    const DSFontGlyph *ref;
    float pen;
    int first;
    float min_left;
    float max_right;
    if (!string || !ensure_font()) return 0;
    ref = ds_font_glyph(font, 'S');
    pen = -(ref ? ref->bearing_x : 0.0f);
    cursor = string;
    first = 1;
    min_left = 0.0f;
    max_right = 0.0f;
    while (*cursor) {
        int codepoint = utf8_next_graphics(&cursor);
        const DSFontGlyph *glyph = ds_font_glyph(font, (uint32_t)codepoint);
        float dleft;
        float dright;
        if (!glyph) continue;
        dleft = pen + glyph->bearing_x;
        dright = dleft + (float)glyph->width;
        if (first || dleft < min_left) min_left = dleft;
        if (first || dright > max_right) max_right = dright;
        first = 0;
        pen += glyph->advance;
    }
    if (first) return 0;
    return (int)(max_right - min_left + 0.5f);
}

int text_ink_height(const char *string) {
    const char *cursor;
    const DSFontGlyph *ref;
    float baseline;
    int first;
    float min_top;
    float max_bottom;
    if (!string || !ensure_font()) return 0;
    ref = ds_font_glyph(font, 'S');
    baseline = ref ? ref->bearing_top : 0.0f;
    cursor = string;
    first = 1;
    min_top = 0.0f;
    max_bottom = 0.0f;
    while (*cursor) {
        int codepoint = utf8_next_graphics(&cursor);
        const DSFontGlyph *glyph = ds_font_glyph(font, (uint32_t)codepoint);
        float dtop;
        float dbottom;
        if (!glyph) continue;
        dtop = baseline - glyph->bearing_top;
        dbottom = dtop + (float)glyph->height;
        if (first || dtop < min_top) min_top = dtop;
        if (first || dbottom > max_bottom) max_bottom = dbottom;
        first = 0;
    }
    if (first) return 0;
    return (int)(max_bottom - min_top + 0.5f);
}

int text_height(void) {
    if (!ensure_font()) return 32;
    return (int)(ds_font_line_height(font) + 0.5f);
}

static void render_texture_unrotated(Buffer *buffer, const Texture *texture,
                                     float x, float y, float scale) {
    float width;
    float height;
    int left;
    int top;
    int right;
    int bottom;
    int screen_y;
    if (!buffer || !texture || !texture->pixels || !isfinite(x) || !isfinite(y) ||
        !isfinite(scale) || scale <= 0.0f) return;
    width = texture->width * scale;
    height = texture->height * scale;
    if (x >= buffer->width || y >= buffer->height || x + width <= 0.0f || y + height <= 0.0f) return;
    left = clamp_floor(floorf(x), buffer->width);
    top = clamp_floor(floorf(y), buffer->height);
    right = clamp_ceil(ceilf(x + width), buffer->width);
    bottom = clamp_ceil(ceilf(y + height), buffer->height);

    if (scale == 1.0f && x == floorf(x) && y == floorf(y) && texture->opaque &&
        left == (int)x && top == (int)y) {
        int source_left = left - (int)x;
        int source_top = top - (int)y;
        for (screen_y = top; screen_y < bottom; ++screen_y) {
            int source_y = source_top + screen_y - top;
            int copy_width = right - left;
            if (source_y < 0 || source_y >= texture->height) continue;
            if (source_left < 0) {
                copy_width += source_left;
                source_left = 0;
            }
            if (source_left + copy_width > texture->width) copy_width = texture->width - source_left;
            if (copy_width > 0) {
                memcpy(buffer->pixels + screen_y * buffer->stride + left,
                       texture->pixels + source_y * texture->width + source_left,
                       (size_t)copy_width * sizeof(uint32_t));
            }
        }
        return;
    }

    for (screen_y = top; screen_y < bottom; ++screen_y) {
        int source_y = (int)(((float)screen_y + 0.5f - y) / scale);
        int screen_x;
        if (source_y < 0) source_y = 0;
        if (source_y >= texture->height) source_y = texture->height - 1;
        for (screen_x = left; screen_x < right; ++screen_x) {
            int source_x = (int)(((float)screen_x + 0.5f - x) / scale);
            uint32_t source;
            if (source_x < 0) source_x = 0;
            if (source_x >= texture->width) source_x = texture->width - 1;
            source = texture->pixels[source_y * texture->width + source_x];
            buffer->pixels[screen_y * buffer->stride + screen_x] =
                blend_pixel(buffer->pixels[screen_y * buffer->stride + screen_x], source);
        }
    }
}

static void render_texture_rotated(Buffer *buffer, const Texture *texture,
                                   float x, float y, float angle, float scale) {
    float half_w = texture->width * 0.5f * scale;
    float half_h = texture->height * 0.5f * scale;
    float center_x = x + half_w;
    float center_y = y + half_h;
    float cos_a = cosf(angle);
    float sin_a = sinf(angle);
    /* Axis-aligned extents of the rotated sprite. */
    float dx = fabsf(half_w * cos_a) + fabsf(half_h * sin_a);
    float dy = fabsf(half_w * sin_a) + fabsf(half_h * cos_a);
    int left;
    int top;
    int right;
    int bottom;
    int screen_x;
    int screen_y;

    if (center_x + dx <= 0.0f || center_y + dy <= 0.0f ||
        center_x - dx >= buffer->width || center_y - dy >= buffer->height) return;
    left = clamp_floor(floorf(center_x - dx), buffer->width);
    top = clamp_floor(floorf(center_y - dy), buffer->height);
    right = clamp_ceil(ceilf(center_x + dx), buffer->width);
    bottom = clamp_ceil(ceilf(center_y + dy), buffer->height);

    for (screen_y = top; screen_y < bottom; ++screen_y) {
        uint32_t *row = buffer->pixels + screen_y * buffer->stride;
        float py = (float)screen_y + 0.5f - center_y;
        for (screen_x = left; screen_x < right; ++screen_x) {
            float px = (float)screen_x + 0.5f - center_x;
            /* Inverse rotation maps the destination pixel back into
             * texture space: R(-angle) * (px, py). */
            float u = px * cos_a + py * sin_a;
            float v = -px * sin_a + py * cos_a;
            int source_x = (int)floorf(u / scale + texture->width * 0.5f);
            int source_y = (int)floorf(v / scale + texture->height * 0.5f);
            uint32_t source;
            if (source_x < 0 || source_x >= texture->width) continue;
            if (source_y < 0 || source_y >= texture->height) continue;
            source = texture->pixels[source_y * texture->width + source_x];
            row[screen_x] = blend_pixel(row[screen_x], source);
        }
    }
}

static void render_texture(Buffer *buffer, const Texture *texture,
                           float x, float y, float angle, float scale) {
    if (fabsf(angle) < 0.0005f) {  /* effectively no rotation */
        render_texture_unrotated(buffer, texture, x, y, scale);
        return;
    }
    render_texture_rotated(buffer, texture, x, y, angle, scale);
}

static void flush_commands(void) {
    size_t i;
    if (!current_buffer) return;
    for (i = 0; i < command_count; ++i) {
        DSCommand *command = &commands[i];
        switch (command->type) {
            case DS_CMD_CLEAR:
                clear_buffer(current_buffer, command->value.clear.colour);
                break;
            case DS_CMD_RECT:
                render_rect(current_buffer, command->value.rect.x, command->value.rect.y,
                            command->value.rect.width, command->value.rect.height,
                            command->value.rect.colour);
                break;
            case DS_CMD_ROUNDRECT:
                render_roundrect(current_buffer, command->value.roundrect.x,
                                 command->value.roundrect.y,
                                 command->value.roundrect.width,
                                 command->value.roundrect.height,
                                 command->value.roundrect.radius,
                                 command->value.roundrect.colour);
                break;
            case DS_CMD_CIRCLE:
                render_circle(current_buffer, command->value.circle.x, command->value.circle.y,
                              command->value.circle.radius, command->value.circle.colour);
                break;
            case DS_CMD_RING:
                render_ring(current_buffer, command->value.ring.x, command->value.ring.y,
                            command->value.ring.radius, command->value.ring.thickness,
                            command->value.ring.colour);
                break;
            case DS_CMD_TEXTURE:
                render_texture(current_buffer, command->value.texture.texture,
                               command->value.texture.x, command->value.texture.y,
                               command->value.texture.angle, command->value.texture.scale);
                break;
            case DS_CMD_TEXT:
                render_text_now(current_buffer, command->value.text.string,
                                command->value.text.x, command->value.text.y,
                                command->value.text.colour, command->value.text.scale);
                break;
        }
    }
}

int ds_graphics_init(AAssetManager *assets) {
    if (asset_manager != assets) {
        ds_release_assets();
        asset_manager = assets;
    }
    return 1;
}

int ds_graphics_begin_frame(Buffer *buffer) {
    if (!buffer || !buffer->pixels || buffer->width <= 0 || buffer->height <= 0 ||
        buffer->stride < buffer->width) return 0;
    current_buffer = buffer;
    frame_open = 1;
    command_count = 0;
    /* A script may omit cls().  Clear once per frame so stale Android buffer
     * contents never leak through; a later cls command still preserves draw
     * order exactly. */
    clear_buffer(buffer, pack_colour(0x00000000u));
    return 1;
}

void ds_graphics_end_frame(void) {
    if (!frame_open) return;
    flush_commands();
    command_count = 0;
    frame_open = 0;
    current_buffer = NULL;
}

void ds_graphics_cancel_frame(void) {
    command_count = 0;
    frame_open = 0;
    current_buffer = NULL;
}

void ds_graphics_error_screen(const char *message) {
    if (!current_buffer) return;
    clear_buffer(current_buffer, pack_colour(0xff2e070b));
    if (font && message) render_text_now(current_buffer, message, 16.0f, 16.0f,
                                         pack_colour(0xffffffffu), 0.65f);
}

void ds_graphics_shutdown(void) {
    ds_graphics_cancel_frame();
    ds_release_assets();
    free(commands);
    commands = NULL;
    command_capacity = 0;
    command_count = 0;
}
