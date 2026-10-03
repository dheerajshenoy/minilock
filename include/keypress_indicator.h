#pragma once

#include "config.h"

#include <cairo.h>

enum KeypressIndicatorState
{
    KEYPRESS_INDICATOR_STATE_IDLE = 0,
    KEYPRESS_INDICATOR_STATE_TYPING,
    KEYPRESS_INDICATOR_STATE_VERIFYING,
    KEYPRESS_INDICATOR_STATE_FAILED,
};

/* Draws the indicator, centered, onto a width x height surface. */
void
render_keypress_indicator(cairo_surface_t *cairo_surface, int width, int height,
                          const struct KeypressIndicatorConfig *config,
                          enum KeypressIndicatorState state);

/* The square (top-left x, y and side) the indicator can touch, for damage. */
void
keypress_indicator_bounds(int width, int height,
                          const struct KeypressIndicatorConfig *config, int *x,
                          int *y, int *size);
