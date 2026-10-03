#include "decoder.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* PNG                                                                */
/* ------------------------------------------------------------------ */
#ifdef HAVE_PNG
    #include <png.h>

static bool
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

    /* volatile: assigned after setjmp, read in the error path */
    uint32_t *volatile px       = NULL;
    png_bytep *volatile rows    = NULL;
    unsigned char *volatile buf = NULL;

    if (setjmp(png_jmpbuf(png)))
    {
        free(px);
        free(rows);
        free(buf);
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

    size_t rowbytes = png_get_rowbytes(png, info);
    buf             = malloc(rowbytes * height);
    px              = malloc((size_t)width * height * 4);
    rows            = malloc(sizeof(png_bytep) * height);
    if (!buf || !px || !rows)
        png_error(png, "out of memory");

    for (int y = 0; y < height; y++)
        rows[y] = buf + (size_t)y * rowbytes;

    png_read_image(png, rows);

    for (size_t i = 0; i < (size_t)width * height; i++)
    {
        unsigned char *p = buf + i * 4;
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

/* ------------------------------------------------------------------ */
/* JPEG                                                               */
/* ------------------------------------------------------------------ */
#ifdef HAVE_JPEG
    #include <jpeglib.h> /* needs <stdio.h> first */

/* libjpeg calls exit() on errors by default; jump back instead. */
struct jpeg_err
{
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void
jpeg_err_exit(j_common_ptr cinfo)
{
    struct jpeg_err *e = (struct jpeg_err *)cinfo->err;
    longjmp(e->jb, 1);
}

static bool
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
    jpeg_create_decompress(&cinfo);

    if (setjmp(jerr.jb))
    {
        jpeg_destroy_decompress(&cinfo);
        free(px);
        free(row);
        fclose(f);
        return false;
    }

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

/* ------------------------------------------------------------------ */
/* WebP                                                               */
/* ------------------------------------------------------------------ */
#ifdef HAVE_WEBP
    #include <webp/decode.h>

static bool
load_webp(const char *path, uint32_t **out, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        fclose(f);
        return false;
    }

    uint8_t *data = malloc(size);
    if (!data || fread(data, 1, size, f) != (size_t)size)
    {
        free(data);
        fclose(f);
        return false;
    }
    fclose(f);

    int width, height;
    if (!WebPGetInfo(data, size, &width, &height))
    {
        free(data);
        return false;
    }

    uint8_t *rgba = WebPDecodeRGBA(data, size, &width, &height);
    free(data);
    if (!rgba)
        return false;

    size_t count = (size_t)width * height;
    uint32_t *px = malloc(count * 4);
    if (!px)
    {
        WebPFree(rgba);
        return false;
    }

    for (size_t i = 0; i < count; i++)
    {
        uint32_t a = rgba[i * 4 + 3];
        uint32_t r = rgba[i * 4 + 0] * a / 255;
        uint32_t g = rgba[i * 4 + 1] * a / 255;
        uint32_t b = rgba[i * 4 + 2] * a / 255;
        px[i]      = (a << 24) | (r << 16) | (g << 8) | b;
    }
    WebPFree(rgba);

    *out = px;
    *w   = width;
    *h   = height;
    return true;
}
#endif

/* ------------------------------------------------------------------ */
/* GIF (multi-frame, composited onto a full-size canvas)              */
/* ------------------------------------------------------------------ */
#ifdef HAVE_GIF
    #include <gif_lib.h>

/* Map a stored row number to its real row for interlaced GIFs. */
static int
gif_row(int y, int h, bool interlaced)
{
    if (!interlaced)
        return y;
    int p1 = (h + 7) / 8, p2 = (h + 3) / 8, p3 = (h + 1) / 4;
    if (y < p1)
        return y * 8;
    y -= p1;
    if (y < p2)
        return 4 + y * 8;
    y -= p2;
    if (y < p3)
        return 2 + y * 4;
    y -= p3;
    return 1 + y * 2;
}

static bool
load_gif(const char *path, struct image *img)
{
    int err          = 0;
    GifFileType *gif = DGifOpenFileName(path, &err);
    if (!gif)
        return false;
    if (DGifSlurp(gif) != GIF_OK || gif->ImageCount < 1)
    {
        DGifCloseFile(gif, &err);
        return false;
    }

    int W = gif->SWidth, H = gif->SHeight, n = gif->ImageCount;
    size_t npx = (size_t)W * H;

    img->frames      = calloc(n, sizeof(*img->frames));
    img->delay_ms    = calloc(n, sizeof(*img->delay_ms));
    uint32_t *canvas = calloc(npx, 4);
    uint32_t *saved  = malloc(npx * 4);
    if (!img->frames || !img->delay_ms || !canvas || !saved)
        goto fail;

    int prev_disp = DISPOSAL_UNSPECIFIED;
    int pl = 0, pt = 0, pw = 0, ph = 0; /* previous frame's rectangle */

    for (int i = 0; i < n; i++)
    {
        SavedImage *si = &gif->SavedImages[i];
        GraphicsControlBlock gcb;
        DGifSavedExtensionToGCB(gif, i, &gcb); /* sets defaults if none */

        /* 1. Undo the previous frame, as it requested. */
        if (prev_disp == DISPOSE_BACKGROUND)
        {
            for (int y = pt; y < pt + ph && y < H; y++)
                for (int x = pl; x < pl + pw && x < W; x++)
                    if (x >= 0 && y >= 0)
                        canvas[(size_t)y * W + x] = 0;
        }
        else if (prev_disp == DISPOSE_PREVIOUS)
            memcpy(canvas, saved, npx * 4);

        /* 2. If this frame will be undone to "previous", remember now. */
        if (gcb.DisposalMode == DISPOSE_PREVIOUS)
            memcpy(saved, canvas, npx * 4);

        /* 3. Draw this frame's rectangle onto the canvas. */
        ColorMapObject *cmap
            = si->ImageDesc.ColorMap ? si->ImageDesc.ColorMap : gif->SColorMap;
        if (!cmap || !si->RasterBits)
            goto fail;

        int fl = si->ImageDesc.Left, ft = si->ImageDesc.Top;
        int fw = si->ImageDesc.Width, fh = si->ImageDesc.Height;
        for (int y = 0; y < fh; y++)
        {
            int cy = ft + gif_row(y, fh, si->ImageDesc.Interlace);
            if (cy < 0 || cy >= H)
                continue;
            for (int x = 0; x < fw; x++)
            {
                int cx = fl + x;
                if (cx < 0 || cx >= W)
                    continue;
                int idx = si->RasterBits[(size_t)y * fw + x];
                if (idx == gcb.TransparentColor || idx >= cmap->ColorCount)
                    continue;
                GifColorType c = cmap->Colors[idx];
                canvas[(size_t)cy * W + cx]
                    = 0xFF000000 | (c.Red << 16) | (c.Green << 8) | c.Blue;
            }
        }

        /* 4. Snapshot the canvas as this frame. */
        img->frames[i] = malloc(npx * 4);
        if (!img->frames[i])
            goto fail;
        memcpy(img->frames[i], canvas, npx * 4);
        img->n_frames = i + 1;

        int d            = gcb.DelayTime * 10; /* GIF delays are in 1/100 s */
        img->delay_ms[i] = d < 20 ? 100 : d;   /* browsers do the same */

        prev_disp = gcb.DisposalMode;
        pl        = fl;
        pt        = ft;
        pw        = fw;
        ph        = fh;
    }

    img->w = W;
    img->h = H;
    free(canvas);
    free(saved);
    DGifCloseFile(gif, &err);
    return true;

fail:
    image_free(img);
    free(canvas);
    free(saved);
    DGifCloseFile(gif, &err);
    return false;
}
#endif

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/* Try each single-frame decoder in turn. */
static bool
load_still(const char *path, uint32_t **out, int *w, int *h)
{
#ifdef HAVE_PNG
    if (load_png(path, out, w, h))
        return true;
#endif

#ifdef HAVE_JPEG
    if (load_jpeg(path, out, w, h))
        return true;
#endif

#ifdef HAVE_WEBP
    if (load_webp(path, out, w, h))
        return true;
#endif

    return false;
}

void
image_free(struct image *img)
{
    for (int i = 0; i < img->n_frames; i++)
        free(img->frames[i]);
    free(img->frames);
    free(img->delay_ms);
    memset(img, 0, sizeof(*img));
}

bool
load_image(const char *path, struct image *img)
{
    memset(img, 0, sizeof(*img));

#ifdef HAVE_GIF
    if (load_gif(path, img))
        return true;
#endif

    uint32_t *px = NULL;
    int w, h;
    if (!load_still(path, &px, &w, &h))
        return false;

    img->frames   = malloc(sizeof(*img->frames));
    img->delay_ms = malloc(sizeof(*img->delay_ms));
    if (!img->frames || !img->delay_ms)
    {
        free(img->frames);
        free(img->delay_ms);
        free(px);
        memset(img, 0, sizeof(*img));
        return false;
    }
    img->frames[0]   = px;
    img->delay_ms[0] = 0;
    img->n_frames    = 1;
    img->w           = w;
    img->h           = h;
    return true;
}
