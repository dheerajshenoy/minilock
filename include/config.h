#pragma once

#include <stdbool.h>
#include <stdint.h>

struct BehaviorConfig
{
    bool ignore_empty_password; /* Enter on an empty field does nothing */
    float fail_delay_s;         /* seconds to wait after a wrong password */
    bool daemonize;             /* fork into the background once locked */
};

struct PAM
{
    const char *service;
    // const char *user;
    // bool allow_null_auth;
};

enum BlurType
{
    BLUR_BOX = 0,
    BLUR_GAUSSIAN,
    BLUR_STACK,
    BLUR_KAWASE,
};

enum KeypressIndicatorShape
{
    SHAPE_CIRCLE = 0,
    SHAPE_SQUARE,
};

struct BlurConfig
{
    bool enable;
    int radius;
    int iterations;
    enum BlurType type;
};

/* All colors are 0xAARRGGBB; 0 means "not set". */
struct ImageConfig
{
    bool cache;       // whether to cache the image in memory (default: true)
    uint32_t bgcolor; /* shown if there is no image (or it failed to load) */
    const char *path;
    bool smooth;
    uint32_t tint_argb; /* image.tint "#RRGGBBAA": the alpha is the strength */

    struct BlurConfig blur;
};

struct KeypressIndicatorConfig
{
    bool show;
    uint32_t color;
    enum KeypressIndicatorShape shape;
    uint32_t color_idle;
    uint32_t color_typing;
    uint32_t color_verifying;
    uint32_t color_failed;
    bool hide_length;
    int radius;
};

struct Config
{
    const char *path;
    struct ImageConfig image;
    struct KeypressIndicatorConfig keypress_indicator;
    struct BehaviorConfig behavior;
    struct PAM pam;
};
