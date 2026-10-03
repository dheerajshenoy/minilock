#include "blur.h"

#include "decoder.h"

void
blur_box(uint32_t *data, int w, int h, const struct BlurConfig *cfg)
{
    for (int i = 0; i < cfg->iterations; i++)
    {
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                int r = 0, g = 0, b = 0, a = 0;
                int count = 0;

                for (int ky = -cfg->radius; ky <= cfg->radius; ky++)
                {
                    for (int kx = -cfg->radius; kx <= cfg->radius; kx++)
                    {
                        int nx = x + kx;
                        int ny = y + ky;

                        if (nx >= 0 && nx < w && ny >= 0 && ny < h)
                        {
                            uint32_t pixel = data[ny * w + nx];
                            a += (pixel >> 24) & 0xFF;
                            r += (pixel >> 16) & 0xFF;
                            g += (pixel >> 8) & 0xFF;
                            b += pixel & 0xFF;
                            count++;
                        }
                    }
                }

                data[y * w + x] = ((a / count) << 24) | ((r / count) << 16)
                                  | ((g / count) << 8) | (b / count);
            }
        }
    }
}

void
image_blur(struct image *img, const struct BlurConfig *cfg)
{
    if (!img || !img->data || !img->w || !img->h)
        return;

    switch (cfg->type)
    {
        case BLUR_BOX:
            blur_box(img->data, img->w, img->h, cfg);
            break;
        case BLUR_GAUSSIAN:
            break;
        default:
            break;
    }
}
