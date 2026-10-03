#define _GNU_SOURCE /* realpath, mkstemp, O_NOFOLLOW, st_mtim: before any include */
#include "cache.h"

#include "decoder.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Bump when the layout below changes; old entries then simply never match. */
#define CACHE_VERSION 1
#define CACHE_MAGIC "MLCACHE"

#define MAX_DIM 16384
#define MAX_FRAMES 100000

struct cache_header
{
    char magic[8]; /* CACHE_MAGIC + NUL */
    uint32_t version;
    uint32_t w, h;
    uint32_t n_frames; /* 0 for a still image */
};

/* One animation frame, followed by its pixels: w*h ARGB words if
 * `whole`, otherwise w*h palette indices and a 256-entry ARGB palette. */
struct cache_frame
{
    int32_t left, top, w, h;
    int32_t transparent;
    uint32_t disposal;
    uint32_t whole;
    float delay_ms;
};

/* ---------------- location and key ---------------- */

static bool
cache_dir(char *out, size_t size)
{
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    char parent[PATH_MAX];

    if (xdg && *xdg)
        snprintf(parent, sizeof(parent), "%s", xdg);
    else if (home && *home)
        snprintf(parent, sizeof(parent), "%s/.cache", home);
    else
        return false;

    snprintf(out, size, "%s/minilock", parent);

    /* Owner-only: the entries are the user's wallpaper, and loading one
     * trusts its contents. */
    mkdir(parent, 0700); /* EEXIST is fine */
    if (mkdir(out, 0700) < 0 && errno != EEXIST)
    {
        fprintf(stderr, "Cache: can't create %s: %s\n", out, strerror(errno));
        return false;
    }
    return true;
}

static uint64_t
fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

/* Fills `out` with the entry's file name; false if the image can't be stat'd
 * or no cache directory is available. */
static bool
cache_file(const char *path, const struct Config *cfg, char *out, size_t size)
{
    char dir[PATH_MAX];
    if (!cache_dir(dir, sizeof(dir)))
        return false;

    char *real = realpath(path, NULL);
    struct stat st;
    if (!real || stat(real, &st) < 0)
    {
        free(real);
        return false;
    }

    /* Everything that changes the stored pixels goes into the key. The
     * scaling mode and colors used at draw time do not. */
    char key[PATH_MAX + 256];
    snprintf(key, sizeof(key), "v%d|%s|%lld|%lld.%09ld|tint=%08x|blur=%d,%d,%d,%d",
             CACHE_VERSION, real, (long long)st.st_size,
             (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec,
             cfg->image.tint_argb, cfg->image.blur.enable,
             cfg->image.blur.radius, cfg->image.blur.iterations,
             (int)cfg->image.blur.type);
    free(real);

    snprintf(out, size, "%s/%016llx.cache", dir,
             (unsigned long long)fnv1a(key));
    return true;
}

/* ---------------- load ---------------- */

/* Bounds-checked reader over the mapped file. */
struct cursor
{
    const unsigned char *p;
    size_t left;
};

static const void *
take(struct cursor *c, size_t n)
{
    if (n > c->left)
        return NULL;
    const void *r = c->p;
    c->p += n;
    c->left -= n;
    return r;
}

static void
free_partial(struct image *img)
{
    image_free(img);
}

struct image *
cache_load(const char *path, const struct Config *cfg)
{
    char file[PATH_MAX];
    if (!cache_file(path, cfg, file, sizeof(file)))
        return NULL;

    int fd = open(file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)
        || st.st_uid != geteuid() || st.st_size < (off_t)sizeof(struct cache_header))
    {
        close(fd);
        return NULL;
    }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return NULL;

    struct cursor cur = {map, (size_t)st.st_size};
    struct image *img = NULL;
    const struct cache_header *hdr = take(&cur, sizeof(*hdr));

    if (!hdr || memcmp(hdr->magic, CACHE_MAGIC, sizeof(CACHE_MAGIC)) != 0
        || hdr->version != CACHE_VERSION || !hdr->w || !hdr->h
        || hdr->w > MAX_DIM || hdr->h > MAX_DIM || hdr->n_frames > MAX_FRAMES)
        goto bad;

    img = calloc(1, sizeof(*img));
    if (!img)
        goto bad;
    img->w      = hdr->w;
    img->h      = hdr->h;
    img->stride = hdr->w * 4;

    size_t frame_px = (size_t)hdr->w * hdr->h;

    if (hdr->n_frames == 0)
    {
        const void *px = take(&cur, frame_px * 4);
        if (!px || cur.left != 0)
            goto bad;
        img->data = malloc(frame_px * 4);
        if (!img->data)
            goto bad;
        memcpy(img->data, px, frame_px * 4);
    }
    else
    {
        img->patches = calloc(hdr->n_frames, sizeof(*img->patches));
        if (!img->patches)
            goto bad;

        for (uint32_t i = 0; i < hdr->n_frames; i++)
        {
            const struct cache_frame *f = take(&cur, sizeof(*f));
            if (!f)
                goto bad;

            /* The renderer indexes by these, so reject anything off-canvas. */
            if (f->left < 0 || f->top < 0 || f->w < 0 || f->h < 0
                || (int64_t)f->left + f->w > (int64_t)hdr->w
                || (int64_t)f->top + f->h > (int64_t)hdr->h
                || f->disposal > FRAME_RESTORE || f->whole > 1
                || f->transparent < -1 || f->transparent > 255
                || !(f->delay_ms >= 0 && f->delay_ms < 1e7f))
                goto bad;

            struct frame_patch *p = &img->patches[i];
            img->n_frames         = i + 1; /* so cleanup frees this one */
            p->left = f->left;
            p->top  = f->top;
            p->w    = f->w;
            p->h    = f->h;
            p->transparent = f->transparent;
            p->disposal    = (enum frame_disposal)f->disposal;
            p->delay_ms    = f->delay_ms;

            size_t n = (size_t)f->w * f->h;
            if (f->whole)
            {
                /* whole frames cover the canvas, like the decoders make them */
                if (f->w != (int32_t)hdr->w || f->h != (int32_t)hdr->h)
                    goto bad;
                const void *px = take(&cur, n * 4);
                p->argb        = px ? malloc(n * 4) : NULL;
                if (!p->argb)
                    goto bad;
                memcpy(p->argb, px, n * 4);
            }
            else
            {
                const void *idx = take(&cur, n);
                const void *pal = idx ? take(&cur, 256 * 4) : NULL;
                if (!pal)
                    goto bad;
                p->idx = n ? malloc(n) : NULL;
                p->pal = malloc(256 * 4);
                if ((n && !p->idx) || !p->pal)
                    goto bad;
                if (n)
                    memcpy(p->idx, idx, n);
                memcpy(p->pal, pal, 256 * 4);
            }
        }
        if (cur.left != 0 || img->n_frames != (int)hdr->n_frames)
            goto bad;
    }

    munmap(map, st.st_size);
    return img;

bad:
    munmap(map, st.st_size);
    free_partial(img);
    fprintf(stderr, "Cache: ignoring invalid entry %s\n", file);
    return NULL;
}

/* ---------------- store ---------------- */

static bool
put(FILE *f, const void *p, size_t n)
{
    return n == 0 || fwrite(p, 1, n, f) == n;
}

void
cache_store(const char *path, const struct Config *cfg, const struct image *img)
{
    char file[PATH_MAX];
    if (!img || !cache_file(path, cfg, file, sizeof(file)))
        return;

    /* Write to a private temp file and rename it into place, so a crash or a
     * second instance never leaves a half-written entry behind. */
    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", file);
    int fd = mkstemp(tmp); /* mode 0600 */
    if (fd < 0)
    {
        fprintf(stderr, "Cache: can't write %s: %s\n", file, strerror(errno));
        return;
    }
    FILE *f = fdopen(fd, "wb");
    if (!f)
    {
        close(fd);
        unlink(tmp);
        return;
    }

    struct cache_header hdr = {.version = CACHE_VERSION,
                               .w       = img->w,
                               .h       = img->h,
                               .n_frames = img->patches ? img->n_frames : 0};
    memcpy(hdr.magic, CACHE_MAGIC, sizeof(CACHE_MAGIC));

    bool ok = put(f, &hdr, sizeof(hdr));
    if (!img->patches)
        ok = ok && put(f, img->data, (size_t)img->w * img->h * 4);

    for (int i = 0; ok && img->patches && i < img->n_frames; i++)
    {
        const struct frame_patch *p = &img->patches[i];
        struct cache_frame cf = {.left = p->left, .top = p->top, .w = p->w,
                                 .h = p->h, .transparent = p->transparent,
                                 .disposal = p->disposal,
                                 .whole    = p->argb != NULL,
                                 .delay_ms = p->delay_ms};
        size_t n = (size_t)p->w * p->h;
        ok       = put(f, &cf, sizeof(cf));
        if (p->argb)
            ok = ok && put(f, p->argb, n * 4);
        else
            ok = ok && put(f, p->idx, n) && put(f, p->pal, 256 * 4);
    }

    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, file) < 0)
    {
        fprintf(stderr, "Cache: failed to save %s\n", file);
        unlink(tmp);
    }
}
