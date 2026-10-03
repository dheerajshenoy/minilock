#include "blur.h"

#include "decoder.h"

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

/* Blur one w x h buffer in place. `scratch` is w * h pixels, `colsum` is
 * w * 4 values; both are reused across frames. */
static void
box_buffer(uint32_t *px, int w, int h, const struct BlurConfig *cfg,
           uint32_t *scratch, uint32_t *colsum)
{
    for (int i = 0; i < cfg->iterations; i++)
    {
        box_h(px, scratch, w, h, cfg->radius);
        box_v(scratch, px, w, h, cfg->radius, colsum);
    }
}

/* Blurs a still image, or every frame of an animation (which must already be
 * whole frames, see image_to_full_frames). */
void
blur_box(struct image *img, const struct BlurConfig *cfg)
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
        box_buffer(img->data, w, h, cfg, scratch, colsum);

    for (int f = 0; f < img->n_frames; f++)
        if (img->patches[f].argb)
            box_buffer(img->patches[f].argb, w, h, cfg, scratch, colsum);

    free(scratch);
    free(colsum);
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
            break;
        default:
            break;
    }
}
