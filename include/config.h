#pragma once

#include <stdbool.h>

enum ImageScaling
{
    SMOOTH = 0,
    NEAREST
};

enum ImageFit
{
    FIT = 0,
    FILL,
    STRETCH,
    CENTER
};

struct BlurConfig
{
    bool blur;
    float blur_radius;
    float blur_sigma;
    float blur_threshold;
};

struct PasswordConfig
{
    bool show_indicator;
    bool show_caps_lock_indicator;
    bool show_failed_attempts;
};

struct Config
{
    const char *bgcolor;
    const char *image_path;
    enum ImageFit image_fit;
    enum ImageScaling image_scaling;
    struct BlurConfig blur_config;
    struct PasswordConfig password_config;
};

struct Config
default_config()
{
    return (struct Config){.bgcolor       = "#FF5000",
                           .image_path    = 0,
                           .image_fit     = FIT,
                           .image_scaling = SMOOTH};
}
