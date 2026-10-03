#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define IMAGE_BG 0xFF1E1E2Eu /* opaque fallback / disposal background */

enum frame_disposal
{
    FRAME_KEEP,    /* leave the canvas as is */
    FRAME_CLEAR,   /* clear the patch rectangle to the background */
    FRAME_RESTORE, /* restore the canvas from before this frame */
};

/* One animation frame. GIF frames are kept as the small rectangle they change
 * (palette indices in idx/pal). Formats that decode whole frames (animated
 * WebP, AVIF) instead set argb to a full-canvas image and leave idx/pal NULL.
 * Either way the renderer composites them on demand. */
struct frame_patch
{
    int left, top, w, h; /* rectangle on the canvas, already clipped to it */
    uint8_t *idx;        /* w*h palette indices, rows in display order */
    uint32_t *pal;       /* 256 ARGB entries */
    uint32_t *argb;      /* full-canvas pixels, if not palette-based */
    int transparent;     /* palette index to skip, or -1 */
    enum frame_disposal disposal;
    float delay_ms;
};

struct image
{
    uint32_t w, h;
    uint32_t stride;
    void *data; /* XRGB canvas for still images; NULL for animations */

    /* Animation (NULL/0 for stills). */
    struct frame_patch *patches;
    int n_frames;
    int current_frame;
};

/* Blend a 0xAARRGGBB tint over every pixel of the image (all frames, and the
 * palettes of palette-based animations). The tint's alpha is its strength. */
void
image_tint(struct image *img, uint32_t tint);

/* Turn a palette-based (GIF) animation into whole-canvas frames, applying the
 * disposal rules once. Needed before per-pixel effects such as blur. Keeps
 * only the first frames that fit the memory cap. */
bool
image_to_full_frames(struct image *img);

/* Free an image and everything it owns. */
void
image_free(struct image *img);

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

void
image_copy(struct image *dst, const struct image *src);
