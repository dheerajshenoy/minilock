#pragma once

#include <stdbool.h>
#include <stdint.h>

struct image
{
    int w, h;
    int n_frames;
    uint32_t **frames; /* n_frames full-size ARGB buffers */
    int *delay_ms;     /* how long each frame is shown */
};

bool
load_image(const char *path, struct image *img);
void
image_free(struct image *img);
