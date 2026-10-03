#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct image
{
    uint32_t w, h;
    uint32_t stride;
    void *data;

#ifdef HAVE_GIF
    int n_frames;
    int current_frame;
    float *delay_ms;
#endif
};

#ifdef HAVE_JPEG
    #include <jpeglib.h>
    #include <setjmp.h>

struct my_error_mgr
{
    struct jpeg_error_mgr pub; /* "public" fields */
    jmp_buf setjmp_buffer;     /* for return to caller */
};

typedef struct my_error_mgr *my_error_ptr;

bool
load_jpeg(const char *path, struct image *img);
#endif

#ifdef HAVE_PNG
    #include <png.h>

bool
load_png(const char *path, struct image *img);
#endif

#ifdef HAVE_WEBP
    #include <webp/decode.h>

bool
load_webp(const char *path, struct image *img);
#endif

#ifdef HAVE_TIFF
    #include <tiffio.h>

bool
load_tiff(const char *path, struct image *img);
#endif

#ifdef HAVE_SVG
    #include <librsvg/rsvg.h>

bool
load_svg(const char *path, struct image *img);
#endif

#ifdef HAVE_AVIF
    #include <avif/avif.h>

bool
load_avif(const char *path, struct image *img);
#endif

#ifdef HAVE_HEIF
    #include <libheif/heif.h>

bool
load_heif(const char *path, struct image *img);
#endif

bool
load_bmp(const char *path, struct image *img);

#ifdef HAVE_GIF
    #include <gif_lib.h>
bool
load_gif(const char *path, struct image *img);
#endif
