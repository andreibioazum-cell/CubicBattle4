#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

stbi_uc *stbi_load(const char *filename, int *x, int *y, int *channels_in_file, int desired_channels) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;
    unsigned char h[54];
    if (fread(h, 1, 54, f) != 54 || h[0] != 'B' || h[1] != 'M') {
        fclose(f); return NULL;
    }
    int offset = h[10] | (h[11]<<8) | (h[12]<<16) | (h[13]<<24);
    int w = h[18] | (h[19]<<8) | (h[20]<<16) | (h[21]<<24);
    int hh = h[22] | (h[23]<<8) | (h[24]<<16) | (h[25]<<24);
    int bpp = h[28] | (h[29]<<8);
    if (bpp != 24 && bpp != 32) { fclose(f); return NULL; }
    int ch = (bpp == 32) ? 4 : 3;
    int abs_h = hh < 0 ? -hh : hh;
    int row = ((w * bpp / 8 + 3) / 4) * 4;
    if (desired_channels == 0) desired_channels = ch;
    stbi_uc *data = (stbi_uc*)malloc((size_t)w * abs_h * desired_channels);
    if (!data) { fclose(f); return NULL; }
    fseek(f, offset, SEEK_SET);
    for (int row_y = abs_h - 1; row_y >= 0; row_y--) {
        for (int col = 0; col < w; col++) {
            unsigned char pixel[4] = {0};
            fread(pixel, 1, bpp/8, f);
            int idx = (row_y * w + col) * desired_channels;
            if (desired_channels >= 3) {
                data[idx + 0] = pixel[2];
                data[idx + 1] = pixel[1];
                data[idx + 2] = pixel[0];
            }
            if (desired_channels == 4) data[idx + 3] = (bpp == 32) ? pixel[3] : 255;
        }
        int pad = row - w * bpp / 8;
        if (pad > 0) fseek(f, pad, SEEK_CUR);
    }
    fclose(f);
    if (x) *x = w;
    if (y) *y = abs_h;
    if (channels_in_file) *channels_in_file = ch;
    return data;
}

void stbi_image_free(void *retval_from_stbi_load) {
    free(retval_from_stbi_load);
}
