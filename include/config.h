#pragma once

#include <limits.h>
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
    SHAPE_RECTANGLE, /* width x height (equal for a square) */
    SHAPE_ROUNDED,   /* rectangle with rounded corners (corner_radius) */
    SHAPE_PILL,      /* rectangle with fully rounded ends */
    SHAPE_ELLIPSE,
    SHAPE_DIAMOND,
    SHAPE_NONE, /* text only; needs state_text */
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

/* Where the indicator goes on one axis. */
enum PositionKind
{
    POS_CENTER = 0, /* the middle of the output */
    POS_PIXELS,     /* pixels to its center; negative = from the far edge */
    POS_PERCENT,    /* percent of the output size to its center */
    POS_START,      /* against the top/left edge, with a margin */
    POS_END,        /* against the bottom/right edge, with a margin */
};

struct AxisPosition
{
    enum PositionKind kind;
    double value; /* pixels or percent */
};

struct Location
{
    struct AxisPosition x, y;
};

struct KeypressIndicatorConfig
{
    struct Location location; /* default: centered */
    bool show;
    uint32_t color;
    enum KeypressIndicatorShape shape;
    uint32_t color_idle;
    uint32_t color_typing;
    uint32_t color_verifying;
    uint32_t color_failed;
    /* Text inside the indicator. When enabled the shape is sized to fit the
     * longest of the four texts and `radius` is ignored. */
    bool state_text;
    const char *text_font; /* family name, e.g. "sans-serif" */
    int font_size;         /* pixels */
    /* Text color per state. Unset: text_color, else (with a shape) black or
     * white, whichever contrasts, or (text only) the state's own color. */
    uint32_t text_color;
    uint32_t text_color_idle;
    uint32_t text_color_typing;
    uint32_t text_color_verifying;
    uint32_t text_color_failed;
    const char *text_idle;
    const char *text_typing;
    const char *text_verifying;
    const char *text_failed;
    bool hide_length;
    int radius;

    /* Shape geometry, in pixels. 0 = automatic: fit the text (or follow
     * `radius` without text). An explicit width/height fixes that axis and
     * shrinks the text if it no longer fits. Circles ignore width/height. */
    int width, height;
    int corner_radius; /* rounded: 0 = a quarter of the shorter side */
    int padding;       /* around the text; 0 = 0.6 x the font size */
    int border_width;  /* drawn inside the edge; 0 = none */
    uint32_t border_color; /* 0 = black or white, whichever contrasts */
    bool hide_idle;   /* draw nothing while idle */
    bool fixed_size;  /* keep the shape at `radius` even with text; the text
                         shrinks to fit (set for the caps lock indicator when
                         it has an explicit radius) */
};

/* The caps lock indicator. It is drawn by the same code as the keypress
 * indicator: finish_config() maps it onto a KeypressIndicatorConfig (off =
 * idle, on = typing) kept in Config.capslock_view. */
struct CapslockIndicatorConfig
{
    bool show;
    bool show_when_off; /* otherwise it only appears while caps lock is on */
    struct Location location;
    enum KeypressIndicatorShape shape;
    int radius; /* 0: size the shape to the text; else fixed (text shrinks to fit) */
    int width, height, corner_radius, padding, border_width;
    uint32_t border_color;
    bool state_text;
    const char *text_font;
    int font_size;
    uint32_t color;                /* shorthand for color_on */
    uint32_t color_on, color_off;
    const char *text_on, *text_off;
    uint32_t text_color, text_color_on, text_color_off;
};

struct Config
{
    const char *path;
    struct ImageConfig image;
    struct KeypressIndicatorConfig keypress_indicator;
    struct CapslockIndicatorConfig capslock_indicator;
    struct KeypressIndicatorConfig capslock_view; /* derived, see above */
    struct BehaviorConfig behavior;
    struct PAM pam;
};
