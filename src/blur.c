#include "blur.h"

#include "decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t
chan(uint32_t px, int c)
{
    return (px >> (8 * c)) & 0xFF;
}

/* sum / count, rounded to nearest. */
static inline uint32_t
mean(uint32_t sum, uint32_t count)
{
    return (sum * 2 + count) / (count * 2);
}

/* Box blur is separable: averaging each row, then each column, gives the same
 * result as averaging the whole (2r+1)^2 square, at O(1) work per pixel
 * instead of O(r^2). Both passes keep a running sum over the window, adding
 * the pixel that enters and removing the one that leaves. Near the borders
 * the window shrinks to the pixels that exist. */

/* Horizontal pass: src -> dst. */
static void
box_h(const uint32_t *src, uint32_t *dst, int w, int h, int r)
{
    for (int y = 0; y < h; y++)
    {
        const uint32_t *row = src + (size_t)y * w;
        uint32_t *out       = dst + (size_t)y * w;
        uint32_t sum[4]     = {0};

        for (int x = 0; x <= r && x < w; x++)
            for (int c = 0; c < 4; c++)
                sum[c] += chan(row[x], c);

        for (int x = 0; x < w; x++)
        {
            int lo = x - r < 0 ? 0 : x - r;
            int hi = x + r >= w ? w - 1 : x + r;

            uint32_t o = 0;
            for (int c = 0; c < 4; c++)
                o |= mean(sum[c], hi - lo + 1) << (8 * c);
            out[x] = o;

            if (x + r + 1 < w) /* enters the window for x + 1 */
                for (int c = 0; c < 4; c++)
                    sum[c] += chan(row[x + r + 1], c);
            if (x - r >= 0) /* leaves it */
                for (int c = 0; c < 4; c++)
                    sum[c] -= chan(row[x - r], c);
        }
    }
}

/* Vertical pass: src -> dst. Works a whole row at a time, with one running
 * sum per column, so memory is read in order. `sum` holds w * 4 values. */
static void
box_v(const uint32_t *src, uint32_t *dst, int w, int h, int r, uint32_t *sum)
{
    memset(sum, 0, (size_t)w * 4 * sizeof(*sum));
    for (int y = 0; y <= r && y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++)
                sum[x * 4 + c] += chan(src[(size_t)y * w + x], c);

    for (int y = 0; y < h; y++)
    {
        int lo = y - r < 0 ? 0 : y - r;
        int hi = y + r >= h ? h - 1 : y + r;

        for (int x = 0; x < w; x++)
        {
            uint32_t o = 0;
            for (int c = 0; c < 4; c++)
                o |= mean(sum[x * 4 + c], hi - lo + 1) << (8 * c);
            dst[(size_t)y * w + x] = o;
        }

        if (y + r + 1 < h)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 4; c++)
                    sum[x * 4 + c] += chan(src[(size_t)(y + r + 1) * w + x], c);
        if (y - r >= 0)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 4; c++)
                    sum[x * 4 + c] -= chan(src[(size_t)(y - r) * w + x], c);
    }
}

/* A Gaussian is approximated by three box blurs in a row. A true kernel costs
 * O(radius) per pixel (half a minute for a 4K image); three boxes land within
 * a few percent of it at O(1), the usual fast-Gaussian trick. The box widths
 * are chosen so their combined variance equals sigma squared. Fills radii[3]
 * with box radii (0 = skip that pass). */
static void
gaussian_radii(int radius, int radii[3])
{
    /* sigma matches the variance of a box of the same radius, so "radius"
     * means roughly the same strength for either blur type. */
    double sigma = radius / sqrt(3.0);
    const int n  = 3;

    int wl = (int)floor(sqrt(12.0 * sigma * sigma / n + 1.0));
    if (wl % 2 == 0)
        wl--; /* box widths must be odd */
    if (wl < 1)
        wl = 1;
    int wu = wl + 2;

    /* How many of the three boxes use the narrower width. */
    double ideal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n)
                   / (-4.0 * wl - 4.0);
    int m = (int)lround(ideal);

    for (int i = 0; i < n; i++)
        radii[i] = ((i < m ? wl : wu) - 1) / 2;
    if (!radii[0] && !radii[1] && !radii[2])
        radii[0] = 1; /* a very small radius: one small box beats no blur */
}

/* Blur one w x h buffer in place, running each box radius in `radii` in
 * turn, `cfg->iterations` times over. `scratch` is w * h pixels and `colsum`
 * is w * 4 values; both are reused across frames. */
static void
box_buffer(uint32_t *px, int w, int h, const struct BlurConfig *cfg,
           const int *radii, int n_radii, uint32_t *scratch, uint32_t *colsum)
{
    for (int i = 0; i < cfg->iterations; i++)
        for (int k = 0; k < n_radii; k++)
        {
            if (!radii[k])
                continue;
            box_h(px, scratch, w, h, radii[k]);
            box_v(scratch, px, w, h, radii[k], colsum);
        }
}

/* Blurs a still image, or every frame of an animation (which must already be
 * whole frames, see image_to_full_frames), with the given box radii. */
static void
blur_with(struct image *img, const struct BlurConfig *cfg, const int *radii,
          int n_radii)
{
    int w = img->w, h = img->h;

    uint32_t *scratch = malloc((size_t)w * h * sizeof(*scratch));
    uint32_t *colsum  = malloc((size_t)w * 4 * sizeof(*colsum));
    if (!scratch || !colsum)
    {
        fprintf(stderr, "Out of memory, skipping blur\n");
        free(scratch);
        free(colsum);
        return;
    }

    if (img->data)
        box_buffer(img->data, w, h, cfg, radii, n_radii, scratch, colsum);

    for (int f = 0; f < img->n_frames; f++)
        if (img->patches[f].argb)
            box_buffer(img->patches[f].argb, w, h, cfg, radii, n_radii,
                       scratch, colsum);

    free(scratch);
    free(colsum);
}

void
blur_box(struct image *img, const struct BlurConfig *cfg)
{
    int radii[1] = {cfg->radius};
    blur_with(img, cfg, radii, 1);
}

void
blur_gaussian(struct image *img, const struct BlurConfig *cfg)
{
    int radii[3];
    gaussian_radii(cfg->radius, radii);
    blur_with(img, cfg, radii, 3);
}

/* Stack blur: a triangular kernel (nearby pixels weigh most, falling off
 * linearly), which is exactly two box filters convolved. Each is about half
 * the width, so run the box passes twice with half the radius. */
void
blur_stack(struct image *img, const struct BlurConfig *cfg)
{
    int r        = (cfg->radius + 1) / 2;
    int radii[2] = {r, r};
    blur_with(img, cfg, radii, 2);
}

/* ---- kawase ---- */

static inline int
clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Average of four pixels, two channels at a time, rounding to nearest. */
static inline uint32_t
avg4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    const uint32_t M = 0x00FF00FFu, half = 0x00020002u;
    uint32_t rb = (((a & M) + (b & M) + (c & M) + (d & M) + half) >> 2) & M;
    uint32_t ag = ((((a >> 8) & M) + ((b >> 8) & M) + ((c >> 8) & M)
                    + ((d >> 8) & M) + half)
                   >> 2)
                  & M;
    return rb | (ag << 8);
}

/* One Kawase pass: every pixel becomes the mean of four samples placed
 * diagonally at +-(k + 0.5) pixels. A half-pixel offset is a bilinear fetch,
 * i.e. the mean of a 2x2 block, so first build those blocks once, then each
 * output needs just four lookups. src -> dst; `blocks` is scratch. */
static void
kawase_pass(const uint32_t *src, uint32_t *blocks, uint32_t *dst, int w, int h,
            int k)
{
    for (int y = 0; y < h; y++)
    {
        const uint32_t *r0 = src + (size_t)y * w;
        const uint32_t *r1 = src + (size_t)clampi(y + 1, 0, h - 1) * w;
        for (int x = 0; x < w; x++)
        {
            int x1 = clampi(x + 1, 0, w - 1);
            blocks[(size_t)y * w + x] = avg4(r0[x], r0[x1], r1[x], r1[x1]);
        }
    }

    for (int y = 0; y < h; y++)
    {
        /* blocks[x] covers pixels x and x + 1, so -(k + 0.5) starts at
         * x - k - 1 and +(k + 0.5) at x + k. */
        const uint32_t *up   = blocks + (size_t)clampi(y - k - 1, 0, h - 1) * w;
        const uint32_t *down = blocks + (size_t)clampi(y + k, 0, h - 1) * w;
        uint32_t *out        = dst + (size_t)y * w;
        for (int x = 0; x < w; x++)
        {
            int xl = clampi(x - k - 1, 0, w - 1);
            int xr = clampi(x + k, 0, w - 1);
            out[x] = avg4(up[xl], up[xr], down[xl], down[xr]);
        }
    }
}

/* `iterations` passes with growing offsets, the last reaching `radius`.
 * Passes ping-pong between px and `scratch`; the result ends up in px. */
static void
kawase_buffer(uint32_t *px, int w, int h, const struct BlurConfig *cfg,
              uint32_t *scratch, uint32_t *blocks)
{
    uint32_t *src = px, *dst = scratch;

    for (int i = 0; i < cfg->iterations; i++)
    {
        int k = cfg->radius * (i + 1) / cfg->iterations;
        kawase_pass(src, blocks, dst, w, h, k < 1 ? 1 : k);

        uint32_t *t = src;
        src         = dst;
        dst         = t;
    }

    if (src != px)
        memcpy(px, src, (size_t)w * h * sizeof(*px));
}

void
blur_kawase(struct image *img, const struct BlurConfig *cfg)
{
    int w = img->w, h = img->h;

    uint32_t *scratch = malloc((size_t)w * h * sizeof(*scratch));
    uint32_t *blocks  = malloc((size_t)w * h * sizeof(*blocks));
    if (!scratch || !blocks)
    {
        fprintf(stderr, "Out of memory, skipping blur\n");
        free(scratch);
        free(blocks);
        return;
    }

    if (img->data)
        kawase_buffer(img->data, w, h, cfg, scratch, blocks);

    for (int f = 0; f < img->n_frames; f++)
        if (img->patches[f].argb)
            kawase_buffer(img->patches[f].argb, w, h, cfg, scratch, blocks);

    free(scratch);
    free(blocks);
}

void
image_blur(struct image *img, const struct BlurConfig *cfg)
{
    if (!img || !img->w || !img->h)
        return;

    /* GIF frames are small palette patches; blurring needs whole pictures. */
    if (img->patches && !image_to_full_frames(img))
    {
        fprintf(stderr, "Skipping blur\n");
        return;
    }

    switch (cfg->type)
    {
        case BLUR_BOX:
            blur_box(img, cfg);
            break;
        case BLUR_GAUSSIAN:
            blur_gaussian(img, cfg);
            break;
        case BLUR_STACK:
            blur_stack(img, cfg);
            break;
        case BLUR_KAWASE:
            blur_kawase(img, cfg);
            break;
        default:
            break;
    }
}
