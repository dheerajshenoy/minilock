#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct image
{
    uint32_t width, height;
    uint32_t stride;
    void *data;
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
