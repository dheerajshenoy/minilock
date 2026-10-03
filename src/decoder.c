#include "decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The surface is opaque: composite 0xAARRGGBB pixels over the fallback color
 * so transparency doesn't reach the compositor as non-premultiplied ARGB. */
static void
flatten_alpha(struct image *img)
{
    const uint32_t bg = 0x1E1E2E;
    uint32_t *px      = img->data;
    for (size_t i = 0; i < (size_t)img->width * img->height; i++)
    {
        uint32_t a = px[i] >> 24, out = 0;
        for (int sh = 0; sh <= 16; sh += 8)
        {
            uint32_t c = (px[i] >> sh) & 0xFF, b = (bg >> sh) & 0xFF;
            out |= ((c * a + b * (255 - a)) / 255) << sh;
        }
        px[i] = 0xFF000000u | out;
    }
}

#ifdef HAVE_JPEG

static void
my_error_exit(j_common_ptr cinfo)
{
    /* cinfo->err really points to a my_error_mgr struct, so coerce pointer */
    my_error_ptr myerr = (my_error_ptr)cinfo->err;

    /* Always display the message. */
    /* We could postpone this until after returning, if we chose. */
    (*cinfo->err->output_message)(cinfo);

    /* Return control to the setjmp point */
    longjmp(myerr->setjmp_buffer, 1);
}

bool
load_jpeg(const char *path, struct image *img)
{
    struct jpeg_decompress_struct cinfo;

    struct my_error_mgr jerr;

    FILE *infile;
    if ((infile = fopen(path, "rb")) == NULL)
    {
        fprintf(stderr, "Can't open %s\n", path);
        return false;
    }

    cinfo.err           = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;

    if (setjmp(jerr.setjmp_buffer))
    {
        /* If we get here, the JPEG code has signaled an error. */
        jpeg_destroy_decompress(&cinfo);
        fclose(infile);
        free(img->data);
        img->data = NULL;
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, infile);
    (void)jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB; /* also converts grayscale/CMYK */
    (void)jpeg_start_decompress(&cinfo);

    img->width  = cinfo.output_width;
    img->height = cinfo.output_height;
    img->stride = img->width * 4; /* XRGB8888, native-endian */

    img->data = malloc((size_t)img->stride * img->height);
    JSAMPROW row = malloc((size_t)img->width * 3);

    if (!img->data || !row)
    {
        fprintf(stderr, "Failed to allocate memory for image\n");
        free(row);
        free(img->data);
        img->data = NULL;
        jpeg_destroy_decompress(&cinfo);
        fclose(infile);
        return false;
    }

    while (cinfo.output_scanline < cinfo.output_height)
    {
        uint32_t *dst
            = (uint32_t *)((char *)img->data
                           + (size_t)cinfo.output_scanline * img->stride);
        (void)jpeg_read_scanlines(&cinfo, &row, 1);
        for (uint32_t x = 0; x < img->width; x++)
            dst[x] = 0xFF000000u | (uint32_t)row[x * 3] << 16
                     | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3 + 2];
    }
    free(row);

    (void)jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(infile);
    return true;
}
#endif

#ifdef HAVE_PNG
bool
load_png(const char *path, struct image *img)
{
    png_image png;
    memset(&png, 0, sizeof(png));
    png.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_file(&png, path))
    {
        fprintf(stderr, "PNG: %s\n", png.message);
        return false;
    }

    /* BGRA bytes == 0xAARRGGBB as a little-endian uint32. */
    png.format  = PNG_FORMAT_BGRA;
    img->width  = png.width;
    img->height = png.height;
    img->stride = png.width * 4;
    img->data   = malloc((size_t)img->stride * img->height);
    if (!img->data)
    {
        fprintf(stderr, "Failed to allocate memory for image\n");
        png_image_free(&png);
        return false;
    }

    if (!png_image_finish_read(&png, NULL, img->data, img->stride, NULL))
    {
        fprintf(stderr, "PNG: %s\n", png.message);
        png_image_free(&png);
        free(img->data);
        img->data = NULL;
        return false;
    }

    flatten_alpha(img);
    return true;
}
#endif

#ifdef HAVE_WEBP
bool
load_webp(const char *path, struct image *img)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "Can't open %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    uint8_t *buf = size > 0 ? malloc(size) : NULL;
    if (!buf || fread(buf, 1, size, f) != (size_t)size)
    {
        fprintf(stderr, "Failed to read %s\n", path);
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    int w, h;
    if (!WebPGetInfo(buf, size, &w, &h))
    {
        fprintf(stderr, "WebP: invalid image\n");
        free(buf);
        return false;
    }

    img->width  = w;
    img->height = h;
    img->stride = (uint32_t)w * 4;
    img->data   = malloc((size_t)img->stride * h);
    if (!img->data)
    {
        fprintf(stderr, "Failed to allocate memory for image\n");
        free(buf);
        return false;
    }

    /* BGRA bytes == 0xAARRGGBB as a little-endian uint32. */
    if (!WebPDecodeBGRAInto(buf, size, img->data,
                            (size_t)img->stride * h, img->stride))
    {
        fprintf(stderr, "WebP: decode failed\n");
        free(buf);
        free(img->data);
        img->data = NULL;
        return false;
    }
    free(buf);

    flatten_alpha(img);
    return true;
}
#endif

#ifdef HAVE_TIFF
bool
load_tiff(const char *path, struct image *img)
{
    TIFF *tif = TIFFOpen(path, "r");
    if (!tif)
    {
        fprintf(stderr, "Can't open %s\n", path);
        return false;
    }

    uint32_t w, h;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);

    img->width  = w;
    img->height = h;
    img->stride = w * 4;
    img->data   = w && h ? malloc((size_t)img->stride * h) : NULL;
    if (!img->data)
    {
        fprintf(stderr, "Failed to allocate memory for image\n");
        TIFFClose(tif);
        return false;
    }

    /* Decodes only the first page, as packed ABGR (R in the low byte). */
    if (!TIFFReadRGBAImageOriented(tif, w, h, img->data, ORIENTATION_TOPLEFT,
                                   0))
    {
        fprintf(stderr, "TIFF: decode failed\n");
        TIFFClose(tif);
        free(img->data);
        img->data = NULL;
        return false;
    }
    TIFFClose(tif);

    uint32_t *px = img->data;
    for (size_t i = 0; i < (size_t)w * h; i++)
        px[i] = (uint32_t)TIFFGetA(px[i]) << 24 | TIFFGetR(px[i]) << 16
                | TIFFGetG(px[i]) << 8 | TIFFGetB(px[i]);

    flatten_alpha(img);
    return true;
}
#endif
