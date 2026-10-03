#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef HAVE_PNG
    #include <png.h>

bool
load_png(const char *path, uint32_t **out, int *w, int *h);

#endif

#ifdef HAVE_JPEG
    #include <jpeglib.h>

// libjpeg reports errors by calling exit() by default, which would kill the
// locker. So we install our own error handler that jumps back with longjmp, the
// same idea as PNG's setjmp.
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

bool
load_jpeg(const char *path, uint32_t **out, int *w, int *h);
#endif
