#ifndef STB_IMAGE_H
#define STB_IMAGE_H
#include <stddef.h>
typedef unsigned char stbi_uc;
stbi_uc *stbi_load(const char *filename, int *x, int *y, int *channels_in_file, int desired_channels);
void stbi_image_free(void *retval_from_stbi_load);
#endif
