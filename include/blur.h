#pragma once

#include "config.h"

struct image;

void
image_blur(struct image *img, const struct BlurConfig *cfg);

void
blur_box(struct image *img, const struct BlurConfig *cfg);
