#include "ttf_font.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DS_FONT_ATLAS_WIDTH  1024
#define DS_FONT_ATLAS_HEIGHT 1024
#define DS_FONT_SUPERSAMPLE  4
#define DS_FONT_MAX_GLYPHS   256

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
    uint32_t kern;

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

static int16_t kern_advance(const DSFont *font, int left, int right) {
    uint16_t tables;
    size_t cursor;
    uint16_t table;
    if (!font->kern || !in_range(font, font->kern, 4)) return 0;
    tables = be_u16(font, font->kern + 2);
    cursor = font->kern + 4;
    for (table = 0; table < tables; ++table) {
        uint16_t length = be_u16(font, cursor + 2);
        uint16_t coverage = be_u16(font, cursor + 4);
        if ((coverage >> 8) == 0 && in_range(font, cursor, length) && length >= 14) {
            uint16_t pairs = be_u16(font, cursor + 6);
            size_t pair_at = cursor + 14;
            uint16_t i;
            for (i = 0; i < pairs; ++i) {
                size_t at = pair_at + (size_t)i * 6;
                if (be_u16(font, at) == (uint16_t)left &&
                    be_u16(font, at + 2) == (uint16_t)right) {
                    return be_s16(font, at + 4);
                }
            }
        }
        if (length < 6) break;
        cursor += length;
    }
    return 0;
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
    table_bounds(font, TAG('k','e','r','n'), &font->kern, &length);

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
    int atlas_y = 1;
    int row_height = 0;
    int i;
    static const uint32_t extra_codepoints[] = {
        0x2022, 0x2026, 0x2190, 0x2192, 0x25B2, 0x25BC, 0x25C0, 0x25B6,
        0x2605, 0x2665, 0x2713, 0x2715, 0
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
    for (codepoint = 0x0400; codepoint <= 0x045F; ++codepoint) add_codepoint(font, (uint32_t)codepoint);
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
    int previous = -1;
    if (!font || !utf8) return 0.0f;
    while (*cursor) {
        int codepoint = utf8_next(&cursor);
        const DSFontGlyph *glyph;
        int current;
        if (codepoint == '\n') {
            if (line_width > width) width = line_width;
            line_width = 0.0f;
            previous = -1;
            continue;
        }
        glyph = ds_font_glyph(font, (uint32_t)codepoint);
        current = glyph ? glyph_for_codepoint(font, (uint32_t)codepoint) : 0;
        if (!glyph) continue;
        if (previous >= 0) line_width += kern_advance(font, previous, current) * font->pixel_scale;
        line_width += glyph->advance;
        previous = current;
    }
    if (line_width > width) width = line_width;
    return width;
}

/* graphics.c embeds this implementation for the legacy two-source Android
 * build.  Keep a separately compiled ttf_font.c weak so both build layouts
 * remain link-compatible. */
#if !defined(DIMSCRIPT_TTF_EMBEDDED) && (defined(__GNUC__) || defined(__clang__))
#pragma weak ds_font_create
#pragma weak ds_font_destroy
#pragma weak ds_font_atlas_width
#pragma weak ds_font_atlas_height
#pragma weak ds_font_atlas_alpha
#pragma weak ds_font_glyph
#pragma weak ds_font_line_height
#pragma weak ds_font_ascent
#pragma weak ds_font_measure
#endif
