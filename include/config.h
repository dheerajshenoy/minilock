#pragma once

#include <stdbool.h>
#include <stdint.h>

struct BehaviorConfig
{
    bool ignore_empty_password; /* Enter on an empty field does nothing */
    float fail_delay_s;         /* seconds to wait after a wrong password */
};

struct PAM
{
    const char *service;
    // const char *user;
    // bool allow_null_auth;
};

enum BlurType
{
    BLUR_GAUSSIAN,
    BLUR_BOX,
    BLUR_KAWASE,
    BLUR_BILATERAL,
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
    uint32_t bgcolor; /* shown if there is no image (or it failed to load) */
    const char *path;
    bool smooth;
    uint32_t tint_argb; /* image.tint "#RRGGBBAA": the alpha is the strength */

    struct BlurConfig blur;
};

struct InputIndicatorConfig
{
    bool show;
    uint32_t color;
    const char *type;
    uint32_t color_idle;
    uint32_t color_typing;
    uint32_t color_wrong;
    uint32_t color_correct;
    uint32_t color_verifying;
    bool hide_length;
    int radius;
};

struct Config
{
    const char *path;
    struct ImageConfig image;
    struct InputIndicatorConfig input_indicator;
    struct BehaviorConfig behavior;
    struct PAM pam;
};
