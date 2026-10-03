#define _GNU_SOURCE /* M_PI under -std=c11 */
#include "keypress_indicator.h"

#include <cairo.h>
#include <math.h>

/* Room around the shape for anti-aliased edges. */
#define MARGIN 2

void
keypress_indicator_bounds(int width, int height,
                          const struct KeypressIndicatorConfig *config, int *x,
                          int *y, int *size)
{
    int half = config->radius + MARGIN;
    int x0 = width / 2 - half, y0 = height / 2 - half;
    int x1 = x0 + 2 * half, y1 = y0 + 2 * half;

    /* Keep it on the surface. */
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > width ? width : x1;
    y1 = y1 > height ? height : y1;

    *x    = x0;
    *y    = y0;
    *size = (x1 - x0 > y1 - y0) ? x1 - x0 : y1 - y0;
}

void
render_keypress_indicator(cairo_surface_t *cairo_surface, int width, int height,
                          const struct KeypressIndicatorConfig *config,
                          enum KeypressIndicatorState state)
{
    uint32_t color;
    switch (state)
    {
        case KEYPRESS_INDICATOR_STATE_TYPING:
            color = config->color_typing;
            break;
        case KEYPRESS_INDICATOR_STATE_VERIFYING:
            color = config->color_verifying;
            break;
        case KEYPRESS_INDICATOR_STATE_FAILED:
            color = config->color_failed;
            break;
        case KEYPRESS_INDICATOR_STATE_IDLE:
        default:
            color = config->color_idle;
            break;
    }

    cairo_t *cr = cairo_create(cairo_surface);

    /* Only the indicator's square is ever touched. */
    int bx, by, bsize;
    keypress_indicator_bounds(width, height, config, &bx, &by, &bsize);
    cairo_rectangle(cr, bx, by, bsize, bsize);
    cairo_clip(cr);

    cairo_set_source_rgba(cr, ((color >> 16) & 0xFF) / 255.0,
                          ((color >> 8) & 0xFF) / 255.0, (color & 0xFF) / 255.0,
                          ((color >> 24) & 0xFF) / 255.0);

    double cx = width / 2.0, cy = height / 2.0;
    if (config->shape == SHAPE_CIRCLE)
        cairo_arc(cr, cx, cy, config->radius, 0, 2 * M_PI);
    else
        cairo_rectangle(cr, cx - config->radius, cy - config->radius,
                        2 * config->radius, 2 * config->radius);
    cairo_fill(cr);

    cairo_destroy(cr);
    cairo_surface_flush(cairo_surface);
}
