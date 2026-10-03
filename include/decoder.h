#pragma once

#include <stdbool.h>

struct image;

#ifdef HAVE_JPEG
bool
load_jpeg(const char *path, struct image *img);
#endif

#ifdef HAVE_PNG
bool
load_png(const char *path, struct image *img);
#endif
