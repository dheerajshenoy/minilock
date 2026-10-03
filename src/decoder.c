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
    for (size_t i = 0; i < (size_t)img->w * img->h; i++)
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

    img->w  = cinfo.output_width;
    img->h = cinfo.output_height;
    img->stride = img->w * 4; /* XRGB8888, native-endian */

    img->data = malloc((size_t)img->stride * img->h);
    JSAMPROW row = malloc((size_t)img->w * 3);

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
        for (uint32_t x = 0; x < img->w; x++)
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
    img->w  = png.width;
    img->h = png.height;
    img->stride = png.width * 4;
    img->data   = malloc((size_t)img->stride * img->h);
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

    img->w  = w;
    img->h = h;
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

    img->w  = w;
    img->h = h;
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

#ifdef HAVE_SVG
bool
load_svg(const char *path, struct image *img)
{
    GError *err       = NULL;
    RsvgHandle *svg   = rsvg_handle_new_from_file(path, &err);
    if (!svg)
    {
        fprintf(stderr, "SVG: %s\n", err ? err->message : "load failed");
        g_clear_error(&err);
        return false;
    }

    double iw, ih;
    if (!rsvg_handle_get_intrinsic_size_in_pixels(svg, &iw, &ih) || iw < 1
        || ih < 1)
    {
        /* No intrinsic size (e.g. only a viewBox): pick a sane default. */
        iw = 1920;
        ih = 1080;
    }

    /* Vectors have no native size, so rasterize at least 1920px wide. */
    double scale = iw < 1920 ? 1920 / iw : 1.0;
    int w        = (int)(iw * scale + 0.5);
    int h        = (int)(ih * scale + 0.5);
    if (w > 16384 || h > 16384)
    {
        fprintf(stderr, "SVG: image too large\n");
        g_object_unref(svg);
        return false;
    }

    img->w  = w;
    img->h = h;
    img->stride = (uint32_t)w * 4;
    img->data   = calloc(h, img->stride);
    if (!img->data)
    {
        fprintf(stderr, "Failed to allocate memory for image\n");
        g_object_unref(svg);
        return false;
    }

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        img->data, CAIRO_FORMAT_ARGB32, w, h, img->stride);
    cairo_t *cr = cairo_create(surf);

    /* Opaque fallback background (#1E1E2E) under any transparency. */
    cairo_set_source_rgb(cr, 0x1E / 255.0, 0x1E / 255.0, 0x2E / 255.0);
    cairo_paint(cr);

    RsvgRectangle viewport = {0, 0, w, h};
    bool ok = rsvg_handle_render_document(svg, cr, &viewport, &err);
    if (!ok)
    {
        fprintf(stderr, "SVG: %s\n", err ? err->message : "render failed");
        g_clear_error(&err);
        free(img->data);
        img->data = NULL;
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    g_object_unref(svg);
    return ok;
}
#endif

#ifdef HAVE_AVIF
bool
load_avif(const char *path, struct image *img)
{
    avifDecoder *dec = avifDecoderCreate();
    if (!dec)
        return false;

    avifResult r = avifDecoderSetIOFile(dec, path);
    if (r == AVIF_RESULT_OK)
        r = avifDecoderParse(dec);
    if (r == AVIF_RESULT_OK)
        r = avifDecoderNextImage(dec); /* first frame only */
    if (r != AVIF_RESULT_OK)
    {
        fprintf(stderr, "AVIF: %s\n", avifResultToString(r));
        avifDecoderDestroy(dec);
        return false;
    }

    uint32_t w = dec->image->width, h = dec->image->height;
    if (!w || !h || w > 16384 || h > 16384)
    {
        fprintf(stderr, "AVIF: unsupported size\n");
        avifDecoderDestroy(dec);
        return false;
    }

    img->w  = w;
    img->h = h;
    img->stride = w * 4;
    img->data   = malloc((size_t)img->stride * h);
    if (!img->data)
    {
        fprintf(stderr, "Failed to allocate memory for image\n");
        avifDecoderDestroy(dec);
        return false;
    }

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, dec->image);
    rgb.format   = AVIF_RGB_FORMAT_BGRA; /* 0xAARRGGBB on little-endian */
    rgb.depth    = 8;
    rgb.pixels   = img->data;
    rgb.rowBytes = img->stride;

    r = avifImageYUVToRGB(dec->image, &rgb);
    avifDecoderDestroy(dec);
    if (r != AVIF_RESULT_OK)
    {
        fprintf(stderr, "AVIF: %s\n", avifResultToString(r));
        free(img->data);
        img->data = NULL;
        return false;
    }

    flatten_alpha(img);
    return true;
}
#endif

#ifdef HAVE_HEIF
bool
load_heif(const char *path, struct image *img)
{
    struct heif_context *ctx = heif_context_alloc();
    struct heif_image_handle *handle = NULL;
    struct heif_image *him           = NULL;
    bool ok                          = false;

    struct heif_error e = heif_context_read_from_file(ctx, path, NULL);
    if (e.code == heif_error_Ok)
        e = heif_context_get_primary_image_handle(ctx, &handle);
    if (e.code == heif_error_Ok) /* applies rotation/mirroring by default */
        e = heif_decode_image(handle, &him, heif_colorspace_RGB,
                              heif_chroma_interleaved_RGBA, NULL);
    if (e.code != heif_error_Ok)
    {
        fprintf(stderr, "HEIF: %s\n", e.message);
        goto out;
    }

    int w = heif_image_get_width(him, heif_channel_interleaved);
    int h = heif_image_get_height(him, heif_channel_interleaved);
    int stride;
    const uint8_t *src
        = heif_image_get_plane_readonly(him, heif_channel_interleaved, &stride);
    if (!src || w <= 0 || h <= 0 || w > 16384 || h > 16384)
    {
        fprintf(stderr, "HEIF: unsupported image\n");
        goto out;
    }

    img->w  = w;
    img->h = h;
    img->stride = (uint32_t)w * 4;
    img->data   = malloc((size_t)img->stride * h);
    if (!img->data)
    {
        fprintf(stderr, "Failed to allocate memory for image\n");
        goto out;
    }

    for (int y = 0; y < h; y++)
    {
        const uint8_t *row = src + (size_t)y * stride;
        uint32_t *dst = (uint32_t *)((char *)img->data + (size_t)y * img->stride);
        for (int x = 0; x < w; x++)
            dst[x] = (uint32_t)row[x * 4 + 3] << 24 | row[x * 4] << 16
                     | row[x * 4 + 1] << 8 | row[x * 4 + 2];
    }

    flatten_alpha(img);
    ok = true;
out:
    if (him)
        heif_image_release(him);
    if (handle)
        heif_image_handle_release(handle);
    heif_context_free(ctx);
    return ok;
}
#endif

/* ---------------- BMP (no external library) ---------------- */

static uint32_t
rd16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t
rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Extract a masked channel and scale it to 8 bits. */
static uint32_t
bmp_channel(uint32_t px, uint32_t mask)
{
    if (!mask)
        return 0;
    int shift = __builtin_ctz(mask);
    uint32_t max = mask >> shift;
    return ((px & mask) >> shift) * 255 / max;
}

/* Uncompressed BMP: 1/4/8-bit palette, 16/24/32-bit, BI_RGB or BITFIELDS. */
bool
load_bmp(const char *path, struct image *img)
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

    uint8_t *buf = size > 54 && size < (256L << 20) ? malloc(size) : NULL;
    if (!buf || fread(buf, 1, size, f) != (size_t)size)
    {
        fprintf(stderr, "BMP: failed to read %s\n", path);
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    bool ok = false;
    uint32_t hdr = rd32(buf + 14), off = rd32(buf + 10);
    int32_t sw = (int32_t)rd32(buf + 18), sh = (int32_t)rd32(buf + 22);
    uint32_t bpp = rd16(buf + 28), comp = rd32(buf + 30);

    if (hdr < 40 || 14 + hdr > (uint32_t)size || sw <= 0 || sh == 0
        || sh == INT32_MIN || sw > 16384 || (sh < 0 ? -sh : sh) > 16384
        || (comp != 0 && comp != 3) || off > (uint32_t)size)
    {
        fprintf(stderr, "BMP: unsupported or corrupt file\n");
        goto out;
    }
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24
        && bpp != 32)
    {
        fprintf(stderr, "BMP: unsupported bit depth %u\n", bpp);
        goto out;
    }

    uint32_t w = sw, h = sh < 0 ? -sh : sh;
    bool top_down = sh < 0;

    uint32_t masks[3] = {0xFF0000, 0x00FF00, 0x0000FF};
    if (bpp == 16)
    {
        masks[0] = 0x7C00;
        masks[1] = 0x03E0;
        masks[2] = 0x001F;
    }
    if (comp == 3)
    {
        if (bpp != 16 && bpp != 32)
            goto out;
        if (14 + 40 + 12 > (size_t)size)
            goto out;
        for (int i = 0; i < 3; i++)
            masks[i] = rd32(buf + 54 + i * 4);
    }

    uint32_t pal[256] = {0};
    if (bpp <= 8)
    {
        uint32_t n = rd32(buf + 46);
        if (!n || n > (1u << bpp))
            n = 1u << bpp;
        const uint8_t *pp = buf + 14 + hdr;
        if ((size_t)(pp - buf) + (size_t)n * 4 > (size_t)size)
            goto out;
        for (uint32_t i = 0; i < n; i++)
            pal[i] = 0xFF000000u | pp[i * 4 + 2] << 16 | pp[i * 4 + 1] << 8
                     | pp[i * 4];
    }

    size_t row_bytes = ((size_t)w * bpp + 31) / 32 * 4;
    if (row_bytes * h > (size_t)size - off)
    {
        fprintf(stderr, "BMP: truncated pixel data\n");
        goto out;
    }

    img->w  = w;
    img->h = h;
    img->stride = w * 4;
    img->data   = malloc((size_t)img->stride * h);
    if (!img->data)
    {
        fprintf(stderr, "Failed to allocate memory for image\n");
        goto out;
    }

    for (uint32_t y = 0; y < h; y++)
    {
        const uint8_t *src = buf + off + row_bytes * (top_down ? y : h - 1 - y);
        uint32_t *dst
            = (uint32_t *)((char *)img->data + (size_t)y * img->stride);
        for (uint32_t x = 0; x < w; x++)
        {
            switch (bpp)
            {
            case 1:
                dst[x] = pal[(src[x / 8] >> (7 - x % 8)) & 1];
                break;
            case 4:
                dst[x] = pal[(src[x / 2] >> (x % 2 ? 0 : 4)) & 0xF];
                break;
            case 8:
                dst[x] = pal[src[x]];
                break;
            case 24:
                dst[x] = 0xFF000000u | src[x * 3 + 2] << 16
                         | src[x * 3 + 1] << 8 | src[x * 3];
                break;
            default: /* 16 and 32 */
            {
                uint32_t px = bpp == 16 ? rd16(src + x * 2) : rd32(src + x * 4);
                dst[x] = 0xFF000000u | bmp_channel(px, masks[0]) << 16
                         | bmp_channel(px, masks[1]) << 8
                         | bmp_channel(px, masks[2]);
            }
            }
        }
    }
    ok = true;
out:
    free(buf);
    return ok;
}
