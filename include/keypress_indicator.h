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

/* The rectangle (top-left x, y, width, height) the indicator can touch in any
 * state, for damage. */
void
keypress_indicator_bounds(int width, int height,
                          const struct KeypressIndicatorConfig *config, int *x,
                          int *y, int *w, int *h);

/* Parses a `location` value into *out:
 *   "center", "top", "bottom", "left", "right",
 *   "top-left", "top-right", "bottom-left", "bottom-right"
 *   "x:100,y:-40"   pixels from the top-left to the center (negative: from the
 *                   right/bottom edge)
 *   "x:25%,y:80%"   percent of the output size
 *   "x:center"      either axis may be left out or be `center`
 * Returns NULL on success, otherwise a message describing the problem. */
const char *
keypress_location_parse(const char *text, struct Location *out);
