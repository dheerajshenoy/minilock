#include "decoder.h"

#include <stdio.h>
#include <stdlib.h>

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
