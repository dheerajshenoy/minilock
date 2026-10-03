#pragma once

#include <stdbool.h>

struct Config
{
    bool smooth_scaling;
    const char *bgcolor;
};

struct Config
default_config()
{
    return (struct Config){.smooth_scaling = true, .bgcolor = "#FF5000"};
}
