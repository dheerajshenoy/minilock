#include "minilock.h"

#include "config.h"
#include "decoder.h"
#include "tomlc17.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

/* Defaults; parse_config overrides what the file sets. */
static struct Config CONFIG = {.behavior = {.fail_delay_s = 2.0f}};

static void
free_image(struct state *state);

bool
load_image(const char *path, struct state *state)
{
    state->img = calloc(1, sizeof(*state->img));
    if (!state->img)
        return false;

    unsigned char magic[12] = {0};
    FILE *f                 = fopen(path, "rb");
    if (f)
    {
        if (fread(magic, 1, sizeof(magic), f) < sizeof(magic))
            memset(magic, 0, sizeof(magic));
        fclose(f);
    }

    bool ok = false;
#ifdef HAVE_JPEG
    if (magic[0] == 0xFF && magic[1] == 0xD8)
        ok = load_jpeg(path, state->img);
#endif
#ifdef HAVE_WEBP
    if (!memcmp(magic, "RIFF", 4) && !memcmp(magic + 8, "WEBP", 4))
        ok = load_webp(path, state->img);
#endif
#ifdef HAVE_TIFF
    if (!memcmp(magic, "II*\0", 4) || !memcmp(magic, "MM\0*", 4)
        || !memcmp(magic, "II+\0", 4) || !memcmp(magic, "MM\0+", 4))
        ok = load_tiff(path, state->img);
#endif
#ifdef HAVE_SVG
    /* SVG is text with no magic bytes, so go by extension. */
    const char *ext = strrchr(path, '.');
    if (ext && (!strcasecmp(ext, ".svg") || !strcasecmp(ext, ".svgz")))
        ok = load_svg(path, state->img);
#endif
    if (!memcmp(magic, "BM", 2))
        ok = load_bmp(path, state->img);
    if (!memcmp(magic + 4, "ftyp", 4))
    {
        /* ISO-BMFF container: AVIF and HEIC share it, told apart by brand. */
        bool avif_brand
            = !memcmp(magic + 8, "avif", 4) || !memcmp(magic + 8, "avis", 4);
#ifdef HAVE_AVIF
        if (avif_brand)
            ok = load_avif(path, state->img);
#endif
#ifdef HAVE_HEIF
        if (!ok && !avif_brand)
            ok = load_heif(path, state->img);
#endif
        (void)avif_brand;
    }
#ifdef HAVE_PNG
    if (!memcmp(magic, "\x89PNG\r\n\x1a\n", 8))
        ok = load_png(path, state->img);
#endif
#ifdef HAVE_GIF
    if (magic[0] == 'G' && magic[1] == 'I' && magic[2] == 'F')
        ok = load_gif(path, state->img);
#endif
    if (!ok)
    {
        fprintf(stderr, "Could not load image: %s\n", path);
        free_image(state);
    }
    return ok;
}

void
free_image(struct state *state)
{
    image_free(state->img);
    state->img = NULL;
}

/* ---------------- lock ---------------- */

static void
handle_locked(void *data, struct ext_session_lock_v1 *lock)
{
    struct state *state = data;
    state->locked       = true;
}

static void
handle_finished(void *data, struct ext_session_lock_v1 *lock)
{
    struct state *state = data;
    state->finished     = true;
}

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked   = handle_locked,
    .finished = handle_finished,
};

/* ---------------- buffers and surfaces ---------------- */

static void
buffer_release(void *data, struct wl_buffer *buf)
{
    struct shm_buf *b = data;
    b->busy           = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static bool
shm_buf_create(struct state *s, struct shm_buf *b, uint32_t w, uint32_t h)
{
    int stride  = w * 4;
    size_t size = (size_t)stride * h;

    int fd = memfd_create(PROJECT_NAME "-buf", 0);
    if (fd < 0 || ftruncate(fd, size) < 0)
    {
        perror("shm");
        if (fd >= 0)
            close(fd);
        return false;
    }

    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED)
    {
        perror("mmap");
        close(fd);
        return false;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(s->shm, fd, size);
    b->buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                       WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    b->px   = px;
    b->size = size;
    b->busy = false;
    wl_buffer_add_listener(b->buf, &buffer_listener, b);
    return true;
}

static void
shm_buf_destroy(struct shm_buf *b)
{
    if (b->buf)
        wl_buffer_destroy(b->buf);
    if (b->px)
        munmap(b->px, b->size);
    memset(b, 0, sizeof(*b));
}

/* ---- scaling ---- */

/* Scale-to-cover axis map: the image is centered and cropped, keeping its
 * aspect ratio. With `smooth`, each position samples two neighbours with a
 * 0-255 weight (bilinear); otherwise it picks the nearest one. */
static void
build_axis(struct axis *a, uint32_t out_n, uint32_t in_n, uint64_t scaled,
           int64_t offset, bool smooth)
{
    for (uint32_t x = 0; x < out_n; x++)
    {
        if (!smooth)
        {
            uint64_t i = (uint64_t)(x + offset) * in_n / scaled;
            if (i >= in_n)
                i = in_n - 1;
            a[x] = (struct axis){(uint32_t)i, (uint32_t)i, 0};
            continue;
        }

        /* Sample centre in 1/256 pixel units: (x + 0.5) * in/scaled - 0.5 */
        int64_t pos = ((2 * ((int64_t)x + offset) + 1) * (int64_t)in_n * 128)
                          / (int64_t)scaled
                      - 128;
        int64_t max = (int64_t)(in_n - 1) * 256;
        pos         = pos < 0 ? 0 : pos > max ? max : pos;
        uint32_t i0 = pos >> 8;
        a[x]        = (struct axis){i0, i0 + 1 < in_n ? i0 + 1 : i0,
                                    (uint8_t)(pos & 255)};
    }
}

static void
build_axes(uint32_t w, uint32_t h, const struct image *img, struct axis *xm,
           struct axis *ym, bool smooth)
{
    uint64_t sw = w, sh = h;
    bool wide   = sw * img->h > sh * img->w;
    uint64_t dw = wide ? sw : (sh * img->w + img->h - 1) / img->h;
    uint64_t dh = wide ? (sw * img->h + img->w - 1) / img->w : sh;

    build_axis(xm, w, img->w, dw, ((int64_t)dw - (int64_t)w) / 2, smooth);
    build_axis(ym, h, img->h, dh, ((int64_t)dh - (int64_t)h) / 2, smooth);
}

/* a*(256-f) + b*f per channel, two channels at a time. */
static inline uint32_t
lerp_px(uint32_t a, uint32_t b, uint32_t f)
{
    uint32_t rb = ((a & 0x00FF00FFu) * (256 - f) + (b & 0x00FF00FFu) * f) >> 8;
    uint32_t ag = (((a >> 8) & 0x00FF00FFu) * (256 - f)
                   + ((b >> 8) & 0x00FF00FFu) * f);
    return (rb & 0x00FF00FFu) | (ag & 0xFF00FF00u);
}

/* Scale the part [x0,x1) x [y0,y1) of the output from src (src_w pixels wide)
 * into dst (dst_w pixels wide). */
static void
scale_rect(uint32_t *dst, uint32_t dst_w, const struct axis *xm,
           const struct axis *ym, const struct rect *r, const uint32_t *src,
           uint32_t src_w, bool smooth)
{
    for (int y = r->y0; y < r->y1; y++)
    {
        const struct axis *ya = &ym[y];
        const uint32_t *r0 = src + (size_t)ya->i0 * src_w;
        const uint32_t *r1 = src + (size_t)ya->i1 * src_w;
        uint32_t *out      = dst + (size_t)y * dst_w;

        if (!smooth)
        {
            for (int x = r->x0; x < r->x1; x++)
                out[x] = r0[xm[x].i0];
            continue;
        }

        for (int x = r->x0; x < r->x1; x++)
        {
            const struct axis *xa = &xm[x];
            uint32_t px = lerp_px(r0[xa->i0], r0[xa->i1], xa->f);
            if (ya->f)
                px = lerp_px(px, lerp_px(r1[xa->i0], r1[xa->i1], xa->f), ya->f);
            out[x] = px;
        }
    }
}

/* Still image (or solid color): draw it into px. */
static void
draw_image(uint32_t *px, uint32_t w, uint32_t h, const struct image *img)
{
    struct axis *axes = img && img->data && img->w && img->h
                            ? malloc(((size_t)w + h) * sizeof(*axes))
                            : NULL;
    if (!axes)
    {
        /* The surface is opaque, so ignore any alpha in bgcolor. */
        uint32_t fill = CONFIG.image.bgcolor
                            ? CONFIG.image.bgcolor | 0xFF000000u
                            : IMAGE_BG;
        for (size_t i = 0; i < (size_t)w * h; i++)
            px[i] = fill;
        return;
    }

    build_axes(w, h, img, axes, axes + w, CONFIG.image.smooth);
    struct rect all = {0, 0, (int)w, (int)h};
    scale_rect(px, w, axes, axes + w, &all, img->data, img->w,
               CONFIG.image.smooth);
    free(axes);
}

/* ---- animation ---- */

static void
rect_add(struct rect *r, int x0, int y0, int x1, int y1)
{
    if (r->x1 <= r->x0) /* currently empty */
    {
        *r = (struct rect){x0, y0, x1, y1};
        return;
    }
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}

/* Draw palette patch p onto the source canvas, or clear its rectangle. */
static void
src_paint(struct state *s, const struct frame_patch *p, bool clear)
{
    const struct image *img = s->img;
    for (int r = 0; r < p->h; r++)
    {
        const uint8_t *row = p->idx + (size_t)r * p->w;
        uint32_t *dst = s->src_canvas + (size_t)(p->top + r) * img->w + p->left;
        for (int x = 0; x < p->w; x++)
        {
            if (clear)
                dst[x] = IMAGE_BG;
            else if (row[x] != p->transparent)
                dst[x] = p->pal[row[x]];
        }
    }
    if (p->w && p->h)
        rect_add(&s->src_dirty, p->left, p->top, p->left + p->w,
                 p->top + p->h);
}

/* Advance the shared source from the frame shown so far to frame k, leaving
 * the region that changed in s->src_dirty. */
static void
src_step(struct state *s, int k)
{
    const struct image *img = s->img;
    size_t px               = (size_t)img->w * img->h;
    const struct frame_patch *p = &img->patches[k];

    s->src_dirty = (struct rect){0};

    if (p->argb) /* whole frame: nothing to composite, just switch to it */
    {
        s->src_cur = p->argb;
        rect_add(&s->src_dirty, 0, 0, img->w, img->h);
        s->src_shown = k;
        return;
    }

    if (s->src_shown >= 0)
    {
        const struct frame_patch *old = &img->patches[s->src_shown];
        if (old->disposal == FRAME_CLEAR)
            src_paint(s, old, true);
        else if (old->disposal == FRAME_RESTORE)
        {
            memcpy(s->src_canvas, s->src_prev, px * sizeof(*s->src_canvas));
            if (old->w && old->h)
                rect_add(&s->src_dirty, old->left, old->top, old->left + old->w,
                         old->top + old->h);
        }
    }

    if (k == 0) /* (re)starting the loop */
    {
        for (size_t i = 0; i < px; i++)
            s->src_canvas[i] = IMAGE_BG;
        rect_add(&s->src_dirty, 0, 0, img->w, img->h);
    }

    if (p->disposal == FRAME_RESTORE)
        memcpy(s->src_prev, s->src_canvas, px * sizeof(*s->src_canvas));
    src_paint(s, p, false);
    s->src_shown = k;
}

/* Set up the shared source for an animated image. False on failure. */
static bool
anim_src_init(struct state *s)
{
    const struct image *img = s->img;
    s->src_shown            = -1;

    if (!img->patches[0].argb) /* palette frames need a canvas to draw on */
    {
        size_t bytes  = (size_t)img->w * img->h * sizeof(uint32_t);
        s->src_canvas = malloc(bytes);
        s->src_prev   = malloc(bytes);
        if (!s->src_canvas || !s->src_prev)
        {
            free(s->src_canvas);
            free(s->src_prev);
            s->src_canvas = s->src_prev = NULL;
            return false;
        }
        s->src_cur = s->src_canvas;
    }

    for (int k = 0; k <= img->current_frame; k++)
        src_step(s, k);
    return true;
}

static void
anim_teardown(struct output *o)
{
    free(o->canvas);
    free(o->xm);
    free(o->ym);
    o->canvas = NULL;
    o->xm = o->ym = NULL;
}

/* Allocate this output's canvas and scale the current source onto it. */
static void
anim_setup(struct output *o)
{
    const struct state *s   = o->state;
    const struct image *img = s->img;
    anim_teardown(o);

    o->canvas = malloc((size_t)o->width * o->height * sizeof(*o->canvas));
    o->xm     = malloc(o->width * sizeof(*o->xm));
    o->ym     = malloc(o->height * sizeof(*o->ym));
    if (!o->canvas || !o->xm || !o->ym)
    {
        anim_teardown(o);
        return;
    }

    build_axes(o->width, o->height, img, o->xm, o->ym, CONFIG.image.smooth);
    o->dirty = (struct rect){0, 0, (int)o->width, (int)o->height};
    scale_rect(o->canvas, o->width, o->xm, o->ym, &o->dirty, s->src_cur,
               img->w, CONFIG.image.smooth);
}

/* Re-scale the part of this output affected by a change in source region r. */
static void
anim_update(struct output *o, const struct rect *r)
{
    if (r->x1 <= r->x0 || r->y1 <= r->y0)
        return;

    /* Sample positions are monotonic, so the affected output columns/rows
     * (those reading any source column/row in [r0, r1)) are contiguous. */
    struct rect out = {0, 0, 0, 0};
    int x = 0, y = 0;
    while (x < (int)o->width && (int)o->xm[x].i1 < r->x0)
        x++;
    out.x0 = x;
    while (x < (int)o->width && (int)o->xm[x].i0 < r->x1)
        x++;
    out.x1 = x;
    while (y < (int)o->height && (int)o->ym[y].i1 < r->y0)
        y++;
    out.y0 = y;
    while (y < (int)o->height && (int)o->ym[y].i0 < r->y1)
        y++;
    out.y1 = y;
    if (out.x1 <= out.x0 || out.y1 <= out.y0)
        return;

    scale_rect(o->canvas, o->width, o->xm, o->ym, &out, o->state->src_cur,
               o->state->img->w, CONFIG.image.smooth);
    rect_add(&o->dirty, out.x0, out.y0, out.x1, out.y1);
}

/* Render into a free buffer and commit it, damaging only what changed. */
static void
render_output(struct output *o)
{
    if (!o->width || !o->height)
        return;

    bool animated = o->canvas != NULL;
    if (animated && o->dirty.x1 <= o->dirty.x0)
        return; /* nothing changed since the last present */

    struct shm_buf *b = NULL;
    for (int i = 0; i < 2 && !b; i++)
        if (!o->bufs[i].busy)
            b = &o->bufs[i];
    if (!b)
        return; /* compositor still holds both; dirty area is kept */

    if (!b->buf && !shm_buf_create(o->state, b, o->width, o->height))
        return;

    int dx = 0, dy = 0, dw = o->width, dh = o->height;
    if (animated)
    {
        /* The buffer may be a frame behind, so copy the whole canvas; only
         * the damage hint is limited to the changed rectangle. */
        memcpy(b->px, o->canvas, b->size);
        dx       = o->dirty.x0;
        dy       = o->dirty.y0;
        dw       = o->dirty.x1 - o->dirty.x0;
        dh       = o->dirty.y1 - o->dirty.y0;
        o->dirty = (struct rect){0};
    }
    else
        draw_image(b->px, o->width, o->height, o->state->img);

    b->busy = true;
    wl_surface_attach(o->surface, b->buf, 0, 0);
    wl_surface_damage_buffer(o->surface, dx, dy, dw, dh);
    wl_surface_commit(o->surface);
}

static void
surface_configure(void *data, struct ext_session_lock_surface_v1 *ls,
                  uint32_t serial, uint32_t w, uint32_t h)
{
    struct output *o = data;

    ext_session_lock_surface_v1_ack_configure(ls, serial);

    if (w != o->width || h != o->height)
    {
        shm_buf_destroy(&o->bufs[0]);
        shm_buf_destroy(&o->bufs[1]);
        o->width  = w;
        o->height = h;
        if (o->state->img && o->state->img->patches)
            anim_setup(o);
    }
    else if (o->canvas)
        o->dirty = (struct rect){0, 0, (int)w, (int)h};
    render_output(o);
}

static const struct ext_session_lock_surface_v1_listener surface_listener = {
    .configure = surface_configure,
};

/* ---------------- PAM ---------------- */

static int
pam_conv_cb(int n, const struct pam_message **msg, struct pam_response **resp,
            void *appdata)
{
    const char *pw         = appdata;
    struct pam_response *r = calloc(n, sizeof(*r));
    if (!r)
        return PAM_BUF_ERR;

    for (int i = 0; i < n; i++)
    {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF
            || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
            r[i].resp = strdup(pw); /* PAM frees this */
    }
    *resp = r;
    return PAM_SUCCESS;
}

/* PAM sleeps inside pam_authenticate after a failure (usually ~2s). Giving it
 * a delay function that does nothing turns that off, so the wait is ours to
 * control through behavior.fail_delay_s. */
static void
pam_no_delay(int status, unsigned usec, void *appdata)
{
}

static bool
check_password(const char *pw)
{
    struct passwd *pwd = getpwuid(getuid());
    if (!pwd)
        return false;

    struct pam_conv conv = {pam_conv_cb, (void *)pw};
    pam_handle_t *h      = NULL;
    /* PROJECT_NAME is already a quoted string from CMake. */
    if (pam_start(PROJECT_NAME, pwd->pw_name, &conv, &h) != PAM_SUCCESS)
        return false;

    pam_set_item(h, PAM_FAIL_DELAY, (const void *)pam_no_delay);

    int rc = pam_authenticate(h, 0);
    pam_end(h, rc);
    return rc == PAM_SUCCESS;
}

/* ---------------- keyboard ---------------- */

static void *
auth_thread(void *data)
{
    struct state *s = data;
    bool ok         = check_password(s->auth_pw);
    explicit_bzero(s->auth_pw, sizeof(s->auth_pw));

    /* Wrong password: wait before accepting another attempt. This runs on the
     * worker thread, so the animation keeps going; typing is ignored until
     * the result is reported below. */
    if (!ok && CONFIG.behavior.fail_delay_s > 0)
    {
        float d            = CONFIG.behavior.fail_delay_s;
        struct timespec ts = {(time_t)d, (long)((d - (time_t)d) * 1e9f)};
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
            ;
    }
    atomic_store(&s->auth_result, ok ? 1 : 2);
    uint64_t one = 1;
    if (write(s->auth_efd, &one, sizeof(one)) < 0)
        perror("eventfd");
    return NULL;
}

static void
start_auth(struct state *s)
{
    memcpy(s->auth_pw, s->password, s->pw_len);
    s->auth_pw[s->pw_len] = '\0';
    explicit_bzero(s->password, sizeof(s->password));
    s->pw_len = 0;

    atomic_store(&s->auth_result, 0);
    pthread_t t;
    if (pthread_create(&t, NULL, auth_thread, s) != 0)
    {
        perror("pthread_create");
        explicit_bzero(s->auth_pw, sizeof(s->auth_pw));
        return;
    }
    pthread_detach(t);
    s->auth_pending = true;
}

static void
kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int fd,
          uint32_t size)
{
    struct state *s = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
    {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;

    xkb_keymap_unref(s->xkb_keymap);
    xkb_state_unref(s->xkb_state);
    s->xkb_keymap
        = xkb_keymap_new_from_string(s->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                     XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    s->xkb_state = xkb_state_new(s->xkb_keymap);
}

static void
kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
         struct wl_surface *surface, struct wl_array *keys)
{
}

static void
kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
         struct wl_surface *surface)
{
}

static void
kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
       uint32_t key, uint32_t key_state)
{
    struct state *s = data;
    if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED || !s->xkb_state
        || s->auth_pending) /* ignore typing while a check is running */
        return;

    /* Wayland keycodes are offset by 8 from xkb keycodes. */
    xkb_keycode_t code = key + 8;
    xkb_keysym_t sym   = xkb_state_key_get_one_sym(s->xkb_state, code);

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)
    {
        if (s->pw_len || !CONFIG.behavior.ignore_empty_password)
            start_auth(s);
    }
    else if (sym == XKB_KEY_BackSpace)
    {
        /* Step back over a whole UTF-8 character. */
        while (s->pw_len > 0 && (s->password[--s->pw_len] & 0xC0) == 0x80)
        {
        }
    }
    else if (sym == XKB_KEY_Escape)
    {
        explicit_bzero(s->password, sizeof(s->password));
        s->pw_len = 0;
    }
    else
    {
        char buf[8];
        int n = xkb_state_key_get_utf8(s->xkb_state, code, buf, sizeof(buf));
        if (n > 0 && (unsigned char)buf[0] >= 0x20
            && s->pw_len + n < sizeof(s->password))
        {
            memcpy(s->password + s->pw_len, buf, n);
            s->pw_len += n;
        }
    }
}

static void
kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
             uint32_t depressed, uint32_t latched, uint32_t locked,
             uint32_t group)
{
    struct state *s = data;
    if (s->xkb_state)
        xkb_state_update_mask(s->xkb_state, depressed, latched, locked, 0, 0,
                              group);
}

static void
kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = kb_keymap,
    .enter       = kb_enter,
    .leave       = kb_leave,
    .key         = kb_key,
    .modifiers   = kb_modifiers,
    .repeat_info = kb_repeat_info,
};

/* ---------------- seat ---------------- */

/* A keyboard may only be requested once the seat advertises the capability
 * (asking earlier is a protocol error), and it can come and go. */
static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    struct state *s = data;

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !s->keyboard)
    {
        s->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(s->keyboard, &keyboard_listener, s);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && s->keyboard)
    {
        wl_keyboard_release(s->keyboard);
        s->keyboard = NULL;
    }
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

/* ---------------- registry ---------------- */

static void
handle_global(void *data, struct wl_registry *registry, uint32_t name,
              const char *interface, uint32_t version)
{
    struct state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
        state->compositor
            = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
    {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
        wl_seat_add_listener(state->seat, &seat_listener, state);
    }
    else if (strcmp(interface, wl_output_interface.name) == 0)
    {
        struct output *o = calloc(1, sizeof(*o));
        if (!o)
            return;
        o->name  = name;
        o->state = state;
        o->wl_output
            = wl_registry_bind(registry, name, &wl_output_interface, 2);
        o->next        = state->outputs;
        state->outputs = o;
    }
    else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0)
    {
        state->lock_manager = wl_registry_bind(
            registry, name, &ext_session_lock_manager_v1_interface, 1);
    }
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    /* Monitor unplug handling comes later. */
}

static const struct wl_registry_listener registry_listener = {
    .global        = handle_global,
    .global_remove = handle_global_remove,
};

static void
arm_timer(int fd, float ms)
{
    struct itimerspec its = {0};
    its.it_value.tv_sec   = (time_t)(ms / 1000);
    its.it_value.tv_nsec  = (long)(ms - its.it_value.tv_sec * 1000) * 1000000L;
    if (!its.it_value.tv_sec && !its.it_value.tv_nsec)
        its.it_value.tv_nsec = 1; /* all-zero would disarm the timer */
    timerfd_settime(fd, 0, &its, NULL);
}

static void
toml_error(const char *msg, const char *msg1)
{
    fprintf(stderr, "ERROR: %s%s\n", msg, msg1 ? msg1 : "");
    exit(1);
}

// ---------------- config ----------------
/* "#RRGGBB" (opaque) or "#RRGGBBAA" -> 0xAARRGGBB. With require_alpha only
 * the 8-digit form is accepted (used where the alpha is meaningful). */
static bool
parse_color(const char *s, bool require_alpha, uint32_t *out)
{
    if (*s == '#')
        s++;
    size_t len = strlen(s);
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || *end != '\0' || (len != 6 && len != 8)
        || (len == 6 && require_alpha))
        return false;

    *out = len == 8 ? (uint32_t)(v & 0xFF) << 24 | (uint32_t)(v >> 8)
                    : 0xFF000000u | (uint32_t)v;
    return true;
}

/* $XDG_CONFIG_HOME/minilock/config.toml or ~/.config/minilock/config.toml,
 * or NULL if there is no readable file there. */
static const char *
default_config_path(void)
{
    static char buf[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");

    if (xdg && *xdg)
        snprintf(buf, sizeof(buf), "%s/minilock/config.toml", xdg);
    else if (home && *home)
        snprintf(buf, sizeof(buf), "%s/.config/minilock/config.toml", home);
    else
        return NULL;
    return access(buf, R_OK) == 0 ? buf : NULL;
}

/* "~/x" -> "$HOME/x" (the shell does this for command-line paths, not TOML). */
static const char *
expand_home(const char *path)
{
    const char *home = getenv("HOME");
    if (!path || path[0] != '~' || path[1] != '/' || !home)
        return path;

    size_t n = strlen(home) + strlen(path);
    char *out = malloc(n);
    if (!out)
        return path;
    snprintf(out, n, "%s%s", home, path + 1);
    return out;
}

/* Fill CONFIG from the TOML file; keys that are absent keep their defaults. */
static bool
parse_config(void)
{
    if (!CONFIG.path)
        CONFIG.path = default_config_path();
    if (!CONFIG.path)
        return false; /* no config file is fine */

    toml_result_t result = toml_parse_file_ex(CONFIG.path);
    if (!result.ok)
    {
        toml_error(result.errmsg, 0);
        return false;
    }

    enum kind
    {
        K_STRING, /* const char *, copied */
        K_BOOL,
        K_INT,
        K_FLOAT, /* TOML float or integer -> float */
        K_COLOR, /* "#RRGGBB" or "#RRGGBBAA" -> uint32_t 0xAARRGGBB */
        K_TINT,  /* "#RRGGBBAA" only -> uint32_t 0xAARRGGBB */
    };
    struct
    {
        const char *key;
        enum kind kind;
        void *dest;
    } entries[] = {
        {"image.bgcolor", K_COLOR, &CONFIG.image.bgcolor},
        {"image.path", K_STRING, &CONFIG.image.path},
        {"image.smooth", K_BOOL, &CONFIG.image.smooth},
        {"image.tint", K_TINT, &CONFIG.image.tint_argb},

        {"behavior.ignore_empty_password", K_BOOL,
         &CONFIG.behavior.ignore_empty_password},
        {"behavior.fail_delay_s", K_FLOAT, &CONFIG.behavior.fail_delay_s},

        {"indicator.input.show", K_BOOL, &CONFIG.input_indicator.show},
        {"indicator.input.color", K_COLOR, &CONFIG.input_indicator.color},
        {"indicator.input.color_idle", K_COLOR,
         &CONFIG.input_indicator.color_idle},
        {"indicator.input.color_typing", K_COLOR,
         &CONFIG.input_indicator.color_typing},
        {"indicator.input.color_wrong", K_COLOR,
         &CONFIG.input_indicator.color_wrong},
        {"indicator.input.color_correct", K_COLOR,
         &CONFIG.input_indicator.color_correct},
        {"indicator.input.color_verifying", K_COLOR,
         &CONFIG.input_indicator.color_verifying},
        {"indicator.input.type", K_STRING, &CONFIG.input_indicator.type},
        {"indicator.input.radius", K_INT, &CONFIG.input_indicator.radius},
        {0, 0, 0}};

    for (int i = 0; entries[i].key; i++)
    {
        toml_datum_t datum = toml_seek(result.toptab, entries[i].key);
        if (datum.type == TOML_UNKNOWN)
            continue; /* key not present: keep the default */

        enum kind k = entries[i].kind;
        toml_type_t want = k == K_BOOL  ? TOML_BOOLEAN
                           : k == K_INT ? TOML_INT64
                                        : TOML_STRING;
        bool type_ok = k == K_FLOAT ? datum.type == TOML_FP64
                                          || datum.type == TOML_INT64
                                    : datum.type == want;
        if (!type_ok)
            toml_error("Invalid type for key: ", entries[i].key);

        switch (k)
        {
            case K_STRING:
                *(const char **)entries[i].dest = strdup(datum.u.s);
                if (!*(const char **)entries[i].dest)
                    toml_error("Out of memory reading key: ", entries[i].key);
                break;
            case K_BOOL:
                *(bool *)entries[i].dest = datum.u.boolean;
                break;
            case K_INT:
                *(int *)entries[i].dest = (int)datum.u.int64;
                break;
            case K_FLOAT:
                *(float *)entries[i].dest = datum.type == TOML_FP64
                                                ? (float)datum.u.fp64
                                                : (float)datum.u.int64;
                if (*(float *)entries[i].dest < 0)
                    toml_error("Must not be negative: ", entries[i].key);
                break;
            case K_COLOR:
            case K_TINT:
                if (!parse_color(datum.u.s, k == K_TINT,
                                 (uint32_t *)entries[i].dest))
                    toml_error(k == K_TINT
                                   ? "Invalid color (expected #RRGGBBAA) for key: "
                                   : "Invalid color (expected #RRGGBB or "
                                     "#RRGGBBAA) for key: ",
                               entries[i].key);
                break;
        }
    }
    toml_free(result); /* everything we keep was copied out */

    CONFIG.image.path = expand_home(CONFIG.image.path);
    return true;
}

static void
print_usage(FILE *out, const char *progname)
{
    fprintf(out,
            "Usage: %s [options]\n"
            "Options:\n"
            "  -h, --help       Show this help message and exit\n"
            "  -c, --config     Specify a custom config file (default: "
            "~/.config/minilock/config.toml)\n"
            "  -v, --version    Show version information and exit\n",
            progname);
}

static void
parse_args(int argc, char **argv, struct Config *cfg)
{
    static const struct option longopts[] = {
        {"config", required_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {0, 0, 0, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:hv", longopts, NULL)) != -1)
    {
        switch (c)
        {
            case 'c':
                cfg->path = optarg;
                break;
            case 'h':
                print_usage(stdout, argv[0]);
                exit(0);
            case 'v':
                printf("minilock " PROJECT_VERSION "\n");
                exit(0);
            default: /* '?' : getopt already printed the error */
                print_usage(stderr, argv[0]);
                exit(1);
        }
    }

    if (optind < argc)
    {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        print_usage(stderr, argv[0]);
        exit(1);
    }
}

int
minilock_init(int argc, char *argv[])
{
    parse_args(argc, argv, &CONFIG);
    parse_config();
    struct state state = {0};

    /* If there's no image, or it fails to load, the background color is used
     * (still locking beats refusing to start). */
    if (CONFIG.image.path && !load_image(CONFIG.image.path, &state))
        fprintf(stderr, "Failed to load image %s, using the background color\n",
                CONFIG.image.path);
    else if (state.img)
    {
        image_tint(state.img, CONFIG.image.tint_argb);
        if (state.img->patches && !anim_src_init(&state))
        {
            fprintf(stderr, "Out of memory, using the background color\n");
            free_image(&state);
        }
    }

    state.display = wl_display_connect(NULL);
    if (!state.display)
    {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

    /* Before the registry roundtrip: the keymap can arrive any time after it. */
    state.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);

    if (!state.compositor || !state.shm || !state.seat)
    {
        fprintf(stderr, "Missing required Wayland globals\n");
        return 1;
    }
    if (!state.lock_manager)
    {
        fprintf(stderr, "Compositor does not support ext-session-lock-v1\n");
        return 1;
    }
    if (!state.outputs)
    {
        fprintf(stderr, "No outputs found\n");
        return 1;
    }

    /* Lock. */
    state.lock = ext_session_lock_manager_v1_lock(state.lock_manager);
    ext_session_lock_v1_add_listener(state.lock, &lock_listener, &state);

    /* One surface + lock surface per output. */
    for (struct output *o = state.outputs; o; o = o->next)
    {
        o->surface      = wl_compositor_create_surface(state.compositor);
        o->lock_surface = ext_session_lock_v1_get_lock_surface(
            state.lock, o->surface, o->wl_output);
        ext_session_lock_surface_v1_add_listener(o->lock_surface,
                                                 &surface_listener, o);
    }

    while (!state.locked && !state.finished)
    {
        if (wl_display_dispatch(state.display) < 0)
        {
            fprintf(stderr, "Wayland connection lost\n");
            return 1;
        }
    }

    if (state.finished)
    {
        fprintf(stderr, "Lock denied (another locker running?)\n");
        ext_session_lock_v1_destroy(state.lock);
        wl_display_roundtrip(state.display);
        wl_display_disconnect(state.display);
        return 1;
    }

    wl_display_roundtrip(state.display);

    state.auth_efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (state.auth_efd < 0)
    {
        perror("eventfd");
        return 1;
    }

    /* Frame timer, only for animated images. */
    struct image *img = state.img;
    int tfd           = -1;
    if (img && img->n_frames > 1 && img->patches)
    {
        tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (tfd >= 0)
            arm_timer(tfd, img->patches[img->current_frame].delay_ms);
    }

    time_t end = time(NULL) + 10; /* temporary safety timeout */
    while (!state.authenticated && time(NULL) < end)
    {
        while (wl_display_prepare_read(state.display) != 0)
            wl_display_dispatch_pending(state.display);
        if (wl_display_flush(state.display) < 0 && errno != EAGAIN)
        {
            wl_display_cancel_read(state.display);
            break;
        }

        struct pollfd pfds[3] = {
            {wl_display_get_fd(state.display), POLLIN, 0},
            {tfd, POLLIN, 0}, /* a negative fd is ignored by poll */
            {state.auth_efd, POLLIN, 0},
        };
        if (poll(pfds, 3, 500) <= 0)
        {
            wl_display_cancel_read(state.display);
            continue;
        }

        if (pfds[0].revents & POLLIN)
        {
            if (wl_display_read_events(state.display) < 0)
                break;
        }
        else
            wl_display_cancel_read(state.display);
        if (wl_display_dispatch_pending(state.display) < 0)
            break;

        if (pfds[2].revents & POLLIN)
        {
            uint64_t n;
            if (read(state.auth_efd, &n, sizeof(n)) > 0)
            {
                state.auth_pending = false;
                if (atomic_load(&state.auth_result) == 1)
                    state.authenticated = true;
                else
                    fprintf(stderr, "Wrong password\n");
            }
        }

        if (tfd >= 0 && (pfds[1].revents & POLLIN))
        {
            uint64_t expirations;
            if (read(tfd, &expirations, sizeof(expirations)) > 0)
            {
                img->current_frame = (img->current_frame + 1) % img->n_frames;
                arm_timer(tfd, img->patches[img->current_frame].delay_ms);
                src_step(&state, img->current_frame);
                for (struct output *o = state.outputs; o; o = o->next)
                {
                    if (o->canvas)
                        anim_update(o, &state.src_dirty);
                    render_output(o);
                }
            }
        }
    }
    if (tfd >= 0)
        close(tfd);
    close(state.auth_efd);
    free(state.src_canvas);
    free(state.src_prev);

    ext_session_lock_v1_unlock_and_destroy(state.lock);
    wl_display_roundtrip(state.display); /* flush the unlock request */
    printf("Unlocked.\n");

    wl_display_disconnect(state.display);

    free_image(&state);

    return 0;
}
