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
#include <sys/poll.h>
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
    if (!state->img)
        return;
    free(state->img->data);
    free(state->img);
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
    wl_buffer_destroy(buf);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static struct wl_buffer *
create_buffer(struct state *s, uint32_t w, uint32_t h)
{
    int stride = w * 4;
    int size   = stride * h;

    int fd = memfd_create(PROJECT_NAME "-buf", 0);
    if (fd < 0 || ftruncate(fd, size) < 0)
    {
        perror("shm");
        exit(1);
    }

    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }

    const struct image *img = s->img;
    if (img && img->data && img->w && img->h)
    {
        /* Scale to cover the output, centered, nearest-neighbour. */
        uint64_t sw = w, sh = h;
        bool wide   = sw * img->h > sh * img->w;
        uint64_t dw = wide ? sw : (sh * img->w + img->h - 1) / img->h;
        uint64_t dh = wide ? (sw * img->h + img->w - 1) / img->w : sh;
        int64_t ox  = ((int64_t)dw - (int64_t)w) / 2;
        int64_t oy  = ((int64_t)dh - (int64_t)h) / 2;

        for (uint32_t y = 0; y < h; y++)
        {
            uint64_t sy = (uint64_t)(y + oy) * img->h / dh;
            if (sy >= img->h)
                sy = img->h - 1;
            const uint32_t *src
                = (const uint32_t *)((const char *)img->data + sy * img->stride);
            for (uint32_t x = 0; x < w; x++)
            {
                uint64_t sx = (uint64_t)(x + ox) * img->w / dw;
                if (sx >= img->w)
                    sx = img->w - 1;
                px[(size_t)y * w + x] = src[sx];
            }
        }
    }
    else
    {
        for (uint32_t i = 0; i < w * h; i++)
            px[i] = 0xFF1E1E2E; /* ARGB: opaque dark blue-gray */
    }

    munmap(px, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(s->shm, fd, size);
    struct wl_buffer *buf    = wl_shm_pool_create_buffer(
        pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    wl_buffer_add_listener(buf, &buffer_listener, NULL);
    return buf;
}

static void
surface_configure(void *data, struct ext_session_lock_surface_v1 *ls,
                  uint32_t serial, uint32_t w, uint32_t h)
{
    struct output *o = data;

    ext_session_lock_surface_v1_ack_configure(ls, serial);

    struct wl_buffer *buf = create_buffer(o->state, w, h);
    wl_surface_attach(o->surface, buf, 0, 0);
    wl_surface_damage_buffer(o->surface, 0, 0, w, h);
    wl_surface_commit(o->surface);
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

    time_t end = time(NULL) + 60; /* temporary safety timeout */
    while (!state.authenticated && time(NULL) < end)
    {
        wl_display_flush(state.display);
        struct pollfd pfd = {wl_display_get_fd(state.display), POLLIN, 0};
        if (poll(&pfd, 1, 500) > 0)
        {
            if (wl_display_dispatch(state.display) < 0)
                break;
        }
    }

    ext_session_lock_v1_unlock_and_destroy(state.lock);
    wl_display_roundtrip(state.display); /* flush the unlock request */
    printf("Unlocked.\n");

    wl_display_disconnect(state.display);

    free_image(&state);

    return 0;
}
