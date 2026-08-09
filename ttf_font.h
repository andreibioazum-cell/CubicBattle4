#ifndef DS_TTF_FONT_H
#define DS_TTF_FONT_H

/*
 * Small TrueType loader used by the software renderer.  It intentionally keeps
 * the font file and the rasterised atlas separate: parsing/rasterising is
 * performed once when a font is first used, while frames only draw textured
 * quads.  The loader supports the TrueType sfnt tables used by regular TTF
 * fonts (cmap format 4/12, simple and composite glyf outlines, hmtx and
 * kern).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* The returned font owns a copy of data and can therefore outlive an Android
 * AAsset.  pixel_height is the logical height of the glyph atlas. */
DSFont *ds_font_create(const unsigned char *data, size_t size, int pixel_height);
void ds_font_destroy(DSFont *font);

int ds_font_atlas_width(const DSFont *font);
int ds_font_atlas_height(const DSFont *font);
const unsigned char *ds_font_atlas_alpha(const DSFont *font);

const DSFontGlyph *ds_font_glyph(const DSFont *font, uint32_t codepoint);
float ds_font_line_height(const DSFont *font);
float ds_font_ascent(const DSFont *font);
float ds_font_measure(const DSFont *font, const char *utf8);

#ifdef __cplusplus
}
#endif

#endif
