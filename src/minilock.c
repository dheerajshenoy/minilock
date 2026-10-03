#include "minilock.h"
#include "config.h"

#include "decoder.h"

#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static void
free_image(struct state *state);

bool
load_image(const char *path, struct state *state)
{
    state->img = calloc(1, sizeof(*state->img));
    if (!state->img)
        return false;

    unsigned char magic[12] = {0};
    FILE *f                = fopen(path, "rb");
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
    int stride = w * 4;
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

/* Scale-to-cover mapping: for each output column/row, the image column/row
 * it shows. The image is centered and cropped, keeping its aspect ratio. */
static void
build_maps(uint32_t w, uint32_t h, const struct image *img, uint32_t *xmap,
           uint32_t *ymap)
{
    uint64_t sw = w, sh = h;
    bool wide   = sw * img->h > sh * img->w;
    uint64_t dw = wide ? sw : (sh * img->w + img->h - 1) / img->h;
    uint64_t dh = wide ? (sw * img->h + img->w - 1) / img->w : sh;
    int64_t ox  = ((int64_t)dw - (int64_t)w) / 2;
    int64_t oy  = ((int64_t)dh - (int64_t)h) / 2;

    for (uint32_t x = 0; x < w; x++)
    {
        uint64_t sx = (uint64_t)(x + ox) * img->w / dw;
        xmap[x]     = sx >= img->w ? img->w - 1 : sx;
    }
    for (uint32_t y = 0; y < h; y++)
    {
        uint64_t sy = (uint64_t)(y + oy) * img->h / dh;
        ymap[y]     = sy >= img->h ? img->h - 1 : sy;
    }
}

/* Still image (or solid color): draw it into px. */
static void
draw_image(uint32_t *px, uint32_t w, uint32_t h, const struct image *img)
{
    uint32_t *maps = img && img->data && img->w && img->h
                         ? malloc(((size_t)w + h) * sizeof(*maps))
                         : NULL;
    if (!maps)
    {
        for (size_t i = 0; i < (size_t)w * h; i++)
            px[i] = IMAGE_BG;
        return;
    }
    uint32_t *xmap = maps, *ymap = maps + w;
    build_maps(w, h, img, xmap, ymap);

    for (uint32_t y = 0; y < h; y++)
    {
        const uint32_t *src
            = (const uint32_t *)((const char *)img->data
                                 + (size_t)ymap[y] * img->stride);
        uint32_t *dst = px + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++)
            dst[x] = src[xmap[x]];
    }
    free(maps);
}

/* ---- animation: patches composited onto an output-sized canvas ---- */

static void
dirty_add(struct output *o, int x0, int y0, int x1, int y1)
{
    if (o->dirty_x1 <= o->dirty_x0) /* currently empty */
    {
        o->dirty_x0 = x0;
        o->dirty_y0 = y0;
        o->dirty_x1 = x1;
        o->dirty_y1 = y1;
        return;
    }
    if (x0 < o->dirty_x0) o->dirty_x0 = x0;
    if (y0 < o->dirty_y0) o->dirty_y0 = y0;
    if (x1 > o->dirty_x1) o->dirty_x1 = x1;
    if (y1 > o->dirty_y1) o->dirty_y1 = y1;
}

/* Output-space rectangle showing patch p. maps are monotonic, so the output
 * columns/rows that map into [left, left+w) form one contiguous range. */
static bool
patch_bbox(const struct output *o, const struct frame_patch *p, int *bx0,
           int *by0, int *bx1, int *by1)
{
    if (!p->w || !p->h)
        return false;
    int x0 = 0, y0 = 0;
    while (x0 < (int)o->width && (int)o->xmap[x0] < p->left)
        x0++;
    int x1 = x0;
    while (x1 < (int)o->width && (int)o->xmap[x1] < p->left + p->w)
        x1++;
    while (y0 < (int)o->height && (int)o->ymap[y0] < p->top)
        y0++;
    int y1 = y0;
    while (y1 < (int)o->height && (int)o->ymap[y1] < p->top + p->h)
        y1++;
    *bx0 = x0; *by0 = y0; *bx1 = x1; *by1 = y1;
    return x1 > x0 && y1 > y0;
}

/* Draw patch p onto the canvas, or clear its rectangle to the background. */
static void
patch_paint(struct output *o, const struct frame_patch *p, bool clear)
{
    int x0, y0, x1, y1;
    if (!patch_bbox(o, p, &x0, &y0, &x1, &y1))
        return;

    for (int y = y0; y < y1; y++)
    {
        const uint8_t *row = p->idx + (size_t)(o->ymap[y] - p->top) * p->w;
        uint32_t *dst      = o->canvas + (size_t)y * o->width;
        for (int x = x0; x < x1; x++)
        {
            uint8_t c = row[o->xmap[x] - p->left];
            if (clear)
                dst[x] = IMAGE_BG;
            else if (c != p->transparent)
                dst[x] = p->pal[c];
        }
    }
    dirty_add(o, x0, y0, x1, y1);
}

/* Advance the canvas from the frame shown so far to frame k. */
static void
anim_step(struct output *o, int k)
{
    const struct image *img = o->state->img;
    size_t px               = (size_t)o->width * o->height;

    if (o->shown >= 0)
    {
        const struct frame_patch *old = &img->patches[o->shown];
        if (old->disposal == FRAME_CLEAR)
            patch_paint(o, old, true);
        else if (old->disposal == FRAME_RESTORE)
        {
            int x0, y0, x1, y1;
            memcpy(o->canvas, o->prev, px * sizeof(*o->canvas));
            if (patch_bbox(o, old, &x0, &y0, &x1, &y1))
                dirty_add(o, x0, y0, x1, y1);
        }
    }

    if (k == 0) /* (re)starting the loop */
    {
        for (size_t i = 0; i < px; i++)
            o->canvas[i] = IMAGE_BG;
        dirty_add(o, 0, 0, o->width, o->height);
    }

    const struct frame_patch *p = &img->patches[k];
    if (p->disposal == FRAME_RESTORE)
        memcpy(o->prev, o->canvas, px * sizeof(*o->canvas));
    patch_paint(o, p, false);
    o->shown = k;
}

static void
anim_teardown(struct output *o)
{
    free(o->canvas);
    free(o->prev);
    free(o->xmap);
    free(o->ymap);
    o->canvas = o->prev = o->xmap = o->ymap = NULL;
    o->shown                                = -1;
}

/* Allocate this output's canvas and catch it up to the current frame. */
static void
anim_setup(struct output *o)
{
    const struct image *img = o->state->img;
    anim_teardown(o);

    size_t px = (size_t)o->width * o->height;
    o->canvas = malloc(px * sizeof(*o->canvas));
    o->prev   = malloc(px * sizeof(*o->prev));
    o->xmap   = malloc(o->width * sizeof(*o->xmap));
    o->ymap   = malloc(o->height * sizeof(*o->ymap));
    if (!o->canvas || !o->prev || !o->xmap || !o->ymap)
    {
        anim_teardown(o);
        return;
    }

    build_maps(o->width, o->height, img, o->xmap, o->ymap);
    for (size_t i = 0; i < px; i++)
        o->canvas[i] = IMAGE_BG;
    for (int k = 0; k <= img->current_frame; k++)
        anim_step(o, k);
    o->dirty_x0 = o->dirty_y0 = 0;
    o->dirty_x1 = o->width;
    o->dirty_y1 = o->height;
}

/* Render into a free buffer and commit it, damaging only what changed. */
static void
render_output(struct output *o)
{
    if (!o->width || !o->height)
        return;

    bool animated = o->canvas != NULL;
    if (animated && o->dirty_x1 <= o->dirty_x0)
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
        dx = o->dirty_x0;
        dy = o->dirty_y0;
        dw = o->dirty_x1 - o->dirty_x0;
        dh = o->dirty_y1 - o->dirty_y0;
        o->dirty_x0 = o->dirty_y0 = o->dirty_x1 = o->dirty_y1 = 0;
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
    {
        o->dirty_x0 = o->dirty_y0 = 0;
        o->dirty_x1 = w;
        o->dirty_y1 = h;
    }
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

    int rc = pam_authenticate(h, 0);
    pam_end(h, rc);
    return rc == PAM_SUCCESS;
}

/* ---------------- keyboard ---------------- */

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
    if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED || !s->xkb_state)
        return;

    /* Wayland keycodes are offset by 8 from xkb keycodes. */
    xkb_keycode_t code = key + 8;
    xkb_keysym_t sym   = xkb_state_key_get_one_sym(s->xkb_state, code);

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)
    {
        s->password[s->pw_len] = '\0';
        if (check_password(s->password))
            s->authenticated = true;
        else
            fprintf(stderr, "Wrong password\n");
        explicit_bzero(s->password, sizeof(s->password));
        s->pw_len = 0;
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

int
minilock_init(int argc, char *argv[])
{
    struct state state = {0};

    if (argc > 1)
    {
        if (!load_image(argv[1], &state))
        {
            fprintf(stderr, "Failed to load image: %s\n", argv[1]);
            return 1;
        } else {
            printf("Loaded image: %s\n", argv[1]);
        }
    }

    state.display = wl_display_connect(NULL);
    if (!state.display)
    {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }

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

    /* Keyboard + xkb. */
    state.xkb_ctx  = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    state.keyboard = wl_seat_get_keyboard(state.seat);
    wl_keyboard_add_listener(state.keyboard, &keyboard_listener, &state);

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
    printf("Locked. Enter your password.\n");

    /* Frame timer, only for animated images. */
    struct image *img = state.img;
    int tfd           = -1;
    if (img && img->n_frames > 1 && img->patches)
    {
        tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (tfd >= 0)
            arm_timer(tfd, img->patches[img->current_frame].delay_ms);
    }

    time_t end = time(NULL) + 60; /* temporary safety timeout */
    while (!state.authenticated && time(NULL) < end)
    {
        while (wl_display_prepare_read(state.display) != 0)
            wl_display_dispatch_pending(state.display);
        if (wl_display_flush(state.display) < 0 && errno != EAGAIN)
        {
            wl_display_cancel_read(state.display);
            break;
        }

        struct pollfd pfds[2] = {
            {wl_display_get_fd(state.display), POLLIN, 0},
            {tfd, POLLIN, 0}, /* a negative fd is ignored by poll */
        };
        if (poll(pfds, 2, 500) <= 0)
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

        if (tfd >= 0 && (pfds[1].revents & POLLIN))
        {
            uint64_t expirations;
            if (read(tfd, &expirations, sizeof(expirations)) > 0)
            {
                img->current_frame = (img->current_frame + 1) % img->n_frames;
                arm_timer(tfd, img->patches[img->current_frame].delay_ms);
                for (struct output *o = state.outputs; o; o = o->next)
                {
                    if (o->canvas)
                        anim_step(o, img->current_frame);
                    render_output(o);
                }
            }
        }
    }
    if (tfd >= 0)
        close(tfd);

    ext_session_lock_v1_unlock_and_destroy(state.lock);
    wl_display_roundtrip(state.display); /* flush the unlock request */
    printf("Unlocked.\n");

    wl_display_disconnect(state.display);

    free_image(&state);

    return 0;
}
