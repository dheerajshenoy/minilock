#pragma once

#include <stdbool.h>
#include <stdint.h>

struct ImageConfig
{
    const char *path;
    bool smooth;
    const char *tint;
};

struct Config
{
    const char *path;
    const char *bgcolor;
    struct ImageConfig image;
    uint32_t color;
};
