#define _GNU_SOURCE /* M_PI under -std=c11 */
#include "keypress_indicator.h"

#include <cairo.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>

/* Room around the shape for anti-aliased edges. */
#define MARGIN 2

static const char *
state_text(const struct KeypressIndicatorConfig *c,
           enum KeypressIndicatorState state)
{
    const char *t;
    switch (state)
    {
        case KEYPRESS_INDICATOR_STATE_TYPING:
            t = c->text_typing;
            break;
        case KEYPRESS_INDICATOR_STATE_VERIFYING:
            t = c->text_verifying;
            break;
        case KEYPRESS_INDICATOR_STATE_FAILED:
            t = c->text_failed;
            break;
        case KEYPRESS_INDICATOR_STATE_IDLE:
        default:
            t = c->text_idle;
            break;
    }
    return t ? t : "";
}

static void
set_font(cairo_t *cr, const struct KeypressIndicatorConfig *c)
{
    cairo_select_font_face(cr, c->text_font ? c->text_font : "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, c->font_size);
}

/* Half-sizes of the shape. Without text it follows `radius`. With text it
 * ignores `radius` and fits the longest of the four texts, so the shape keeps
 * one steady size as the state changes. A circle encloses the text box, a
 * square becomes a rectangle around it. */
static void
shape_half_size(const struct KeypressIndicatorConfig *c, double *hw, double *hh)
{
    if (!c->state_text)
    {
        *hw = *hh = c->radius;
        return;
    }

    /* Measuring needs a context; a 1x1 scratch surface will do. Config never
     * changes after startup, so measure once. */
    static bool measured;
    static double text_w, text_h;
    if (!measured)
    {
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *cr        = cairo_create(s);
        set_font(cr, c);

        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        text_h = fe.ascent + fe.descent;

        for (int st = 0; st < 4; st++)
        {
            cairo_text_extents_t te;
            cairo_text_extents(cr, state_text(c, (enum KeypressIndicatorState)st),
                               &te);
            if (te.x_advance > text_w)
                text_w = te.x_advance;
        }
        cairo_destroy(cr);
        cairo_surface_destroy(s);
        measured = true;
    }

    if (c->shape == SHAPE_NONE)
    {
        /* Just the text box, plus a little room for glyph overshoot. */
        *hw = text_w / 2 + c->font_size * 0.25;
        *hh = text_h / 2 + c->font_size * 0.25;
        return;
    }

    double pad = c->font_size * 0.6;
    if (c->shape == SHAPE_CIRCLE)
    {
        *hw = *hh = hypot(text_w / 2, text_h / 2) + pad;
    }
    else
    {
        *hw = text_w / 2 + pad;
        *hh = text_h / 2 + pad;
    }
}

/* Gap kept between the indicator and the edge for top/bottom/left/right. */
#define EDGE_MARGIN 32

/* Where the center goes on one axis, kept on the output. `half` is half the
 * indicator's size on that axis, needed to sit against an edge. */
static double
axis_center(const struct AxisPosition *p, int length, double half)
{
    double c;
    switch (p->kind)
    {
        case POS_PIXELS:
            c = p->value >= 0 ? p->value : length + p->value;
            break;
        case POS_PERCENT:
            c = length * p->value / 100.0;
            break;
        case POS_START:
            c = EDGE_MARGIN + half;
            break;
        case POS_END:
            c = length - EDGE_MARGIN - half;
            break;
        case POS_CENTER:
        default:
            c = length / 2.0;
            break;
    }
    return c < 0 ? 0 : c > length ? length : c;
}

static void
indicator_center(int width, int height,
                 const struct KeypressIndicatorConfig *c, double hw, double hh,
                 double *cx, double *cy)
{
    *cx = axis_center(&c->location.x, width, hw);
    *cy = axis_center(&c->location.y, height, hh);
}

void
keypress_indicator_bounds(int width, int height,
                          const struct KeypressIndicatorConfig *config, int *x,
                          int *y, int *w, int *h)
{
    double hw, hh;
    shape_half_size(config, &hw, &hh);

    double cx, cy;
    indicator_center(width, height, config, hw, hh, &cx, &cy);
    int x0 = (int)floor(cx - hw) - MARGIN;
    int y0 = (int)floor(cy - hh) - MARGIN;
    int x1 = (int)ceil(cx + hw) + MARGIN;
    int y1 = (int)ceil(cy + hh) + MARGIN;

    /* Keep it on the surface. */
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > width ? width : x1;
    y1 = y1 > height ? height : y1;

    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
}

static void
set_color(cairo_t *cr, uint32_t c)
{
    cairo_set_source_rgba(cr, ((c >> 16) & 0xFF) / 255.0,
                          ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0,
                          ((c >> 24) & 0xFF) / 255.0);
}

/* Black or white, whichever reads better on `fill`. */
static uint32_t
contrast_color(uint32_t fill)
{
    double r = ((fill >> 16) & 0xFF) / 255.0, g = ((fill >> 8) & 0xFF) / 255.0,
           b = (fill & 0xFF) / 255.0;
    double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return luma > 0.6 ? 0xFF000000u : 0xFFFFFFFFu;
}

/* The text color for a state. An explicit per-state color wins, then the
 * shared text_color. Otherwise: with a shape, black or white for contrast
 * against it; text only, the state's own color. */
static uint32_t
text_color_for(const struct KeypressIndicatorConfig *c,
               enum KeypressIndicatorState state, uint32_t fill)
{
    uint32_t own;
    switch (state)
    {
        case KEYPRESS_INDICATOR_STATE_TYPING:
            own = c->text_color_typing;
            break;
        case KEYPRESS_INDICATOR_STATE_VERIFYING:
            own = c->text_color_verifying;
            break;
        case KEYPRESS_INDICATOR_STATE_FAILED:
            own = c->text_color_failed;
            break;
        case KEYPRESS_INDICATOR_STATE_IDLE:
        default:
            own = c->text_color_idle;
            break;
    }
    if (own)
        return own;
    if (c->text_color)
        return c->text_color;
    return c->shape == SHAPE_NONE ? fill : contrast_color(fill);
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

    /* Only the indicator's area is ever touched. */
    int bx, by, bw, bh;
    keypress_indicator_bounds(width, height, config, &bx, &by, &bw, &bh);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_clip(cr);

    double hw, hh;
    shape_half_size(config, &hw, &hh);
    double cx, cy;
    indicator_center(width, height, config, hw, hh, &cx, &cy);

    if (config->shape != SHAPE_NONE)
    {
        set_color(cr, color);
        if (config->shape == SHAPE_CIRCLE)
            cairo_arc(cr, cx, cy, hw, 0, 2 * M_PI);
        else
            cairo_rectangle(cr, cx - hw, cy - hh, 2 * hw, 2 * hh);
        cairo_fill(cr);
    }

    if (config->state_text)
    {
        const char *text = state_text(config, state);
        if (*text)
        {
            set_font(cr, config);
            cairo_font_extents_t fe;
            cairo_font_extents(cr, &fe);
            cairo_text_extents_t te;
            cairo_text_extents(cr, text, &te);

            /* Centre horizontally on the advance, and put the baseline at a
             * fixed height so the text doesn't bounce between states. */
            double x = cx - te.x_advance / 2;
            double y = cy + (fe.ascent - fe.descent) / 2;

            set_color(cr, text_color_for(config, state, color));
            cairo_move_to(cr, x, y);
            cairo_show_text(cr, text);
        }
    }

    cairo_destroy(cr);
    cairo_surface_flush(cairo_surface);
}

/* ---------------- location parsing ---------------- */


/* Copy of s[0..n) trimmed and lower-cased into buf. */
static void
clean(const char *s, size_t n, char *buf, size_t size)
{
    while (n && isspace((unsigned char)*s))
        s++, n--;
    while (n && isspace((unsigned char)s[n - 1]))
        n--;
    if (n >= size)
        n = size - 1;
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)s[i]);
    buf[n] = '\0';
}

/* "center", "-40", "25%", "12.5%". NULL if fine. */
static const char *
parse_axis_value(const char *v, struct AxisPosition *out)
{
    if (!strcmp(v, "center"))
    {
        *out = (struct AxisPosition){POS_CENTER, 0};
        return NULL;
    }

    char *end;
    double d = strtod(v, &end);
    if (end == v || d != d)
        return "expected a number, a percentage (25%) or `center`";

    if (*end == '%' && end[1] == '\0')
        *out = (struct AxisPosition){POS_PERCENT, d};
    else if (*end == '\0')
        *out = (struct AxisPosition){POS_PIXELS, d};
    else
        return "expected a number, a percentage (25%) or `center`";

    if (d < -1e6 || d > 1e6)
        return "number out of range";
    return NULL;
}

const char *
keypress_location_parse(const char *text, struct Location *out)
{
    static const struct
    {
        const char *name;
        enum PositionKind x, y;
    } keywords[] = {
        {"center", POS_CENTER, POS_CENTER},
        {"top", POS_CENTER, POS_START},
        {"bottom", POS_CENTER, POS_END},
        {"left", POS_START, POS_CENTER},
        {"right", POS_END, POS_CENTER},
        {"top-left", POS_START, POS_START},
        {"top-right", POS_END, POS_START},
        {"bottom-left", POS_START, POS_END},
        {"bottom-right", POS_END, POS_END},
    };

    char whole[64];
    clean(text, strlen(text), whole, sizeof(whole));
    if (!*whole)
        return "empty location";

    for (size_t i = 0; i < sizeof(keywords) / sizeof(*keywords); i++)
        if (!strcmp(whole, keywords[i].name))
        {
            out->x = (struct AxisPosition){keywords[i].x, 0};
            out->y = (struct AxisPosition){keywords[i].y, 0};
            return NULL;
        }

    /* "x:...,y:..." */
    struct Location loc = {0};
    bool seen_x = false, seen_y = false;
    const char *p = text;
    while (*p)
    {
        const char *comma = strchr(p, ',');
        size_t len        = comma ? (size_t)(comma - p) : strlen(p);

        char part[48];
        clean(p, len, part, sizeof(part));
        char *colon = strchr(part, ':');
        if (!colon)
            return "expected a keyword (center, top, bottom-left, ...) or "
                   "x:...,y:...";

        *colon = '\0';
        char key[8];
        clean(part, strlen(part), key, sizeof(key));
        char val[40];
        clean(colon + 1, strlen(colon + 1), val, sizeof(val));

        struct AxisPosition *axis;
        if (!strcmp(key, "x") && !seen_x)
            axis = &loc.x, seen_x = true;
        else if (!strcmp(key, "y") && !seen_y)
            axis = &loc.y, seen_y = true;
        else
            return !strcmp(key, "x") || !strcmp(key, "y")
                       ? "x or y given twice"
                       : "unknown key (only x and y)";

        const char *err = parse_axis_value(val, axis);
        if (err)
            return err;

        if (comma && !comma[1])
            return "trailing comma";
        p = comma ? comma + 1 : p + len;
    }

    *out = loc; /* an axis that wasn't given stays centered */
    return NULL;
}
