#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef HAVE_PNG
    #include <png.h>

bool
load_png(const char *path, uint32_t **out, int *w, int *h);
#endif
