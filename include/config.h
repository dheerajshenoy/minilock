#pragma once

#include <stdbool.h>
#include <stdint.h>

struct ImageConfig
{
    const char *bgcolor;
    const char *path;
    bool smooth;
    const char *tint;   /* "#RRGGBBAA", blended over the image */
    uint32_t tint_argb; /* parsed from tint; alpha 0 means no tint */
};

struct InputIndicatorConfig
{
    bool show;
    const char *color;
    const char *type;
    int radius;
};

struct Config
{
    const char *path;
    struct ImageConfig image;
    struct InputIndicatorConfig input_indicator;
    uint32_t color;
};
