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
set_font(cairo_t *cr, const struct KeypressIndicatorConfig *c, double size)
{
    cairo_select_font_face(cr, c->text_font ? c->text_font : "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

/* The widest text and the line height at the configured font size, over all
 * four states. Measured once per indicator (the config is constant after
 * startup); the two indicators have different fonts, so each gets its own. */
struct measure
{
    const struct KeypressIndicatorConfig *config;
    double text_w, text_h;
};

static const struct measure *
measure_for(const struct KeypressIndicatorConfig *c)
{
    static struct measure cache[4];
    struct measure *m = NULL;
    for (size_t i = 0; i < sizeof(cache) / sizeof(*cache); i++)
    {
        if (cache[i].config == c)
            return &cache[i];
        if (!m && !cache[i].config)
            m = &cache[i];
    }
    if (!m)
        m = &cache[0]; /* more indicators than slots: reuse one */

    /* Measuring needs a context; a 1x1 scratch surface will do. */
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr        = cairo_create(s);
    set_font(cr, c, c->font_size);

    m->config = c;
    m->text_w = 0;
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    m->text_h = fe.ascent + fe.descent;
    for (int st = 0; st < 4; st++)
    {
        cairo_text_extents_t te;
        cairo_text_extents(cr, state_text(c, (enum KeypressIndicatorState)st),
                           &te);
        if (te.x_advance > m->text_w)
            m->text_w = te.x_advance;
    }
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return m;
}

static double
text_padding(const struct KeypressIndicatorConfig *c)
{
    return c->padding > 0 ? c->padding : c->font_size * 0.6;
}

/* How much to shrink the text so its box fits inside the shape (never > 1).
 * Corner-cut shapes leave less room, hence the smaller factors. */
static double
text_fit_factor(const struct KeypressIndicatorConfig *c, double hw, double hh)
{
    const struct measure *m = measure_for(c);
    if (m->text_w <= 0 || m->text_h <= 0 || hw <= 0 || hh <= 0)
        return 1;

    double tw = m->text_w / 2, th = m->text_h / 2, f;
    switch (c->shape)
    {
        case SHAPE_CIRCLE:
            f = 0.9 * hw / hypot(tw, th);
            break;
        case SHAPE_ELLIPSE:
            f = 0.9 / hypot(tw / hw, th / hh);
            break;
        case SHAPE_DIAMOND:
            f = 0.9 / (tw / hw + th / hh);
            break;
        case SHAPE_ROUNDED:
        case SHAPE_PILL:
            f = 0.8 * fmin(hw / tw, hh / th);
            break;
        default:
            f = 0.9 * fmin(hw / tw, hh / th);
            break;
    }
    return f < 1 ? f : 1;
}

/* Half-sizes of the shape. Without text they follow width/height, else
 * `radius`. With text the shape fits the longest of the four texts (so it
 * keeps one steady size as the state changes) unless width/height/radius fix
 * the size. `*fit` says whether the text may need shrinking to fit. */
static void
shape_half_size(const struct KeypressIndicatorConfig *c, double *hw, double *hh,
                bool *fit)
{
    *fit = false;

    if (c->shape == SHAPE_NONE && c->state_text)
    {
        /* Just the text box, plus a little room for glyph overshoot. */
        const struct measure *m = measure_for(c);
        *hw = m->text_w / 2 + c->font_size * 0.25;
        *hh = m->text_h / 2 + c->font_size * 0.25;
        return;
    }

    bool fixed = !c->state_text || c->fixed_size;
    double aw = 0, ah = 0; /* automatic (text-fitting) half-sizes */
    if (!fixed)
    {
        const struct measure *m = measure_for(c);
        double pad = text_padding(c), tw = m->text_w / 2 + pad,
               th = m->text_h / 2 + pad;
        switch (c->shape)
        {
            case SHAPE_CIRCLE:
                aw = ah = hypot(m->text_w / 2, m->text_h / 2) + pad;
                break;
            case SHAPE_ELLIPSE: /* encloses the text box */
                aw = tw * M_SQRT2;
                ah = th * M_SQRT2;
                break;
            case SHAPE_DIAMOND:
                aw = tw * 2;
                ah = th * 2;
                break;
            default:
                aw = tw;
                ah = th;
                break;
        }
    }

    double dw = fixed ? c->radius : aw, dh = fixed ? c->radius : ah;
    /* Wide shapes default to 2:1 when only `radius` sizes them. */
    if (fixed && (c->shape == SHAPE_RECTANGLE || c->shape == SHAPE_PILL
                  || c->shape == SHAPE_ELLIPSE))
        dw = 2.0 * c->radius;
    if (c->width > 0)
        dw = c->width / 2.0;
    if (c->height > 0)
        dh = c->height / 2.0;

    switch (c->shape)
    {
        case SHAPE_CIRCLE: /* width/height don't apply to a circle */
            dw = dh = fixed ? c->radius : aw;
            break;
        default:
            break;
    }

    *hw  = dw;
    *hh  = dh;
    /* Text may overflow when anything but the text decided the size. */
    *fit = c->state_text && (fixed || c->width > 0 || c->height > 0);
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
    bool fit;
    shape_half_size(config, &hw, &hh, &fit);

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

/* Rounded rectangle path with corner radius r (already limited to fit). */
static void
rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

/* Adds the shape's outline, centered on (cx, cy) with half-sizes hw x hh. */
static void
shape_path(cairo_t *cr, const struct KeypressIndicatorConfig *c, double cx,
           double cy, double hw, double hh)
{
    switch (c->shape)
    {
        case SHAPE_CIRCLE:
            cairo_arc(cr, cx, cy, hw, 0, 2 * M_PI);
            break;
        case SHAPE_ROUNDED:
        case SHAPE_PILL:
        {
            double shorter = fmin(hw, hh);
            double r       = c->shape == SHAPE_PILL ? shorter
                             : c->corner_radius > 0 ? fmin(c->corner_radius, shorter)
                                                    : shorter * 0.5;
            rounded_rect(cr, cx - hw, cy - hh, 2 * hw, 2 * hh, r);
            break;
        }
        case SHAPE_ELLIPSE:
            cairo_save(cr);
            cairo_translate(cr, cx, cy);
            cairo_scale(cr, hw, hh);
            cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
            cairo_restore(cr); /* the path keeps its shape */
            break;
        case SHAPE_DIAMOND:
            cairo_move_to(cr, cx - hw, cy);
            cairo_line_to(cr, cx, cy - hh);
            cairo_line_to(cr, cx + hw, cy);
            cairo_line_to(cr, cx, cy + hh);
            cairo_close_path(cr);
            break;
        case SHAPE_RECTANGLE:
        default:
            cairo_rectangle(cr, cx - hw, cy - hh, 2 * hw, 2 * hh);
            break;
    }
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
    bool fit;
    shape_half_size(config, &hw, &hh, &fit);
    double cx, cy;
    indicator_center(width, height, config, hw, hh, &cx, &cy);

    if (config->shape != SHAPE_NONE)
    {
        shape_path(cr, config, cx, cy, hw, hh);
        set_color(cr, color);
        if (config->border_width > 0)
        {
            cairo_fill_preserve(cr);

            /* Clip to the shape and stroke twice the width, so the border
             * is drawn inside the edge and the size doesn't change. */
            cairo_save(cr);
            cairo_clip_preserve(cr);
            set_color(cr, config->border_color ? config->border_color
                                               : contrast_color(color));
            cairo_set_line_width(cr, 2.0 * config->border_width);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
        else
            cairo_fill(cr);
        cairo_new_path(cr);
    }

    if (config->state_text)
    {
        const char *text = state_text(config, state);
        if (*text)
        {
            double font_px = config->font_size;
            if (fit && config->shape != SHAPE_NONE)
                font_px *= text_fit_factor(config, hw, hh);
            set_font(cr, config, font_px);
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
