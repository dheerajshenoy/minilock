#pragma once

#include <stdbool.h>

struct Config
{
    bool smooth_scaling;
};

struct Config
default_config()
{
    return (struct Config){
        .smooth_scaling = true,
    };
}
