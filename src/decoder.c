#include "decoder.h"

#include <stdlib.h>

#ifdef HAVE_PNG
bool
load_png(const char *path, uint32_t **out, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    unsigned char sig[8];
    if (fread(sig, 1, 8, f) != 8 || png_sig_cmp(sig, 0, 8))
    {
        fclose(f);
        return false;
    }

    png_structp png
        = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info)
    {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return false;
    }

    uint32_t *px    = NULL;
    png_bytep *rows = NULL;

    if (setjmp(png_jmpbuf(png)))
    {
        free(px);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return false;
    }

    png_init_io(png, f);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    int width  = png_get_image_width(png, info);
    int height = png_get_image_height(png, info);

    /* Normalize everything to 8-bit RGBA. */
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    png_size_t rowbytes = png_get_rowbytes(png, info);
    unsigned char *buf  = malloc(rowbytes * height);
    px                  = malloc((size_t)width * height * 4);
    rows                = malloc(sizeof(png_bytep) * height);
    if (!buf || !px || !rows)
    {
        free(buf);
        png_error(png, "out of memory");
    }
    for (int y = 0; y < height; y++)
        rows[y] = buf + y * rowbytes;

    png_read_image(png, rows);

    for (int i = 0; i < width * height; i++)
    {
        unsigned char *p = buf + (size_t)i * 4;
        uint32_t a       = p[3];
        uint32_t r       = p[0] * a / 255;
        uint32_t g       = p[1] * a / 255;
        uint32_t b       = p[2] * a / 255;
        px[i]            = (a << 24) | (r << 16) | (g << 8) | b;
    }

    free(buf);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);

    *out = px;
    *w   = width;
    *h   = height;
    return true;
}
#endif

#ifdef HAVE_JPEG
bool
load_jpeg(const char *path, uint32_t **out, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_err jerr;
    uint32_t *volatile px       = NULL;
    unsigned char *volatile row = NULL;

    cinfo.err           = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_err_exit;

    if (setjmp(jerr.jb))
    {
        jpeg_destroy_decompress(&cinfo);
        free(px);
        free(row);
        fclose(f);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB; /* 3 bytes per pixel, any source */
    jpeg_start_decompress(&cinfo);

    int width  = cinfo.output_width;
    int height = cinfo.output_height;

    px  = malloc((size_t)width * height * 4);
    row = malloc((size_t)width * 3);
    if (!px || !row)
        longjmp(jerr.jb, 1);

    while (cinfo.output_scanline < cinfo.output_height)
    {
        unsigned char *rp = row;
        int y             = cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, &rp, 1);
        for (int x = 0; x < width; x++)
        {
            uint32_t r                = row[x * 3 + 0];
            uint32_t g                = row[x * 3 + 1];
            uint32_t b                = row[x * 3 + 2];
            px[(size_t)y * width + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(row);
    fclose(f);

    *out = px;
    *w   = width;
    *h   = height;
    return true;
}
#endif
