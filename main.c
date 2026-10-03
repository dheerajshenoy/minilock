#define _GNU_SOURCE
#include "ext-session-lock-v1-client-protocol.h"

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#define STR_(x) #x
#define STR(x) STR_(x)

struct state; /* forward declaration */

struct output
{
    struct wl_output *wl_output;
    uint32_t name;
    struct output *next;
    struct state *state;
    struct wl_surface *surface;
    struct ext_session_lock_surface_v1 *lock_surface;
};

struct state
{
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct ext_session_lock_manager_v1 *lock_manager;
    struct output *outputs;
    struct ext_session_lock_v1 *lock;
    bool locked, finished;
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    char password[256];
    size_t pw_len;
    bool authenticated;
};

// Registry listener callbacks when a global object is added or removed
static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
    struct state *s = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
        s->compositor
            = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
    {
        s->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
        s->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
    }
    else if (strcmp(interface, wl_output_interface.name) == 0)
    {
        struct output *o = calloc(1, sizeof(*o));
        o->name          = name;
        o->state         = s;
        o->wl_output
            = wl_registry_bind(registry, name, &wl_output_interface, 2);
        o->next    = s->outputs;
        s->outputs = o;
    }
    else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0)
    {
        s->lock_manager = wl_registry_bind(
            registry, name, &ext_session_lock_manager_v1_interface, 1);
    }
}

/* Monitor unplug handling: we'll deal with this later. */
static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

static void
lock_locked(void *data, struct ext_session_lock_v1 *lock)
{
    struct state *s = data;
    s->locked       = true;
}

static void
lock_finished(void *data, struct ext_session_lock_v1 *lock)
{
    struct state *s = data;
    s->finished     = true;
}

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked   = lock_locked,
    .finished = lock_finished,
};

static struct wl_buffer *
create_buffer(struct state *s, uint32_t width, uint32_t height)
{
    int stride = width * 4;
    int size   = stride * height;

    int fd = memfd_create(STR(PROJECT_NAME) "-buf", 0);
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
    for (uint32_t i = 0; i < width * height; i++)
        px[i] = 0xFF1E1E2E; /* ARGB: opaque dark blue-gray */
    munmap(px, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(s->shm, fd, size);
    struct wl_buffer *buf    = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

static void
surface_configure(void *data, struct ext_session_lock_surface_v1 *ls,
                  uint32_t serial, uint32_t width, uint32_t height)
{
    struct output *o = data;

    ext_session_lock_surface_v1_ack_configure(ls, serial);

    struct wl_buffer *buf = create_buffer(o->state, width, height);
    wl_surface_attach(o->surface, buf, 0, 0);
    wl_surface_commit(o->surface);
}

static const struct ext_session_lock_surface_v1_listener surface_listener = {
    .configure = surface_configure,
};

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
        {
            r[i].resp = strdup(pw); /* PAM frees this itself */
        }
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
    if (pam_start(PROJECT_NAME, pwd->pw_name, &conv, &h) != PAM_SUCCESS)
        return false;

    int rc = pam_authenticate(h, 0);
    if (rc != PAM_SUCCESS)
        fprintf(stderr, "PAM: %s (service: %s)\n", pam_strerror(h, rc),
                STR(PROJECT_NAME));
    pam_end(h, rc);
    return rc == PAM_SUCCESS;
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

    fprintf(stderr, "keymap received\n");
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

    // Handle password input and authentication
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)
    {
        s->password[s->pw_len] = '\0';
        fprintf(stderr, "bytes:");
        for (size_t i = 0; i < s->pw_len; i++)
            fprintf(stderr, " %02x", (unsigned char)s->password[i]);
        fprintf(stderr, "\n");
        if (check_password(s->password))
            s->authenticated = true;
        else
            printf("Wrong password\n");
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

int
main()
{
    struct state state = {0};

    state.display = wl_display_connect(NULL);
    if (!state.display)
    {
        fprintf(stderr, "Cannot connect to Wayland display\n");
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);

    /* Block until the compositor has sent all globals. */
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

    int n = 0;
    for (struct output *o = state.outputs; o; o = o->next)
        n++;

    state.lock     = ext_session_lock_manager_v1_lock(state.lock_manager);
    state.xkb_ctx  = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    state.keyboard = wl_seat_get_keyboard(state.seat);

    wl_keyboard_add_listener(state.keyboard, &keyboard_listener, &state);
    ext_session_lock_v1_add_listener(state.lock, &lock_listener, &state);

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
        return 1;
    }

    printf("Session locked!\n");
    wl_display_roundtrip(state.display);

    printf("Locked. Enter your password.\n");
    time_t end = time(NULL) + 10; /* temporary safety timeout */
    while (!state.authenticated && time(NULL) < end)
    {
        wl_display_flush(state.display);
        struct pollfd pfd = {wl_display_get_fd(state.display), POLLIN, 0};
        if (poll(&pfd, 1, 500) > 0)
            wl_display_dispatch(state.display);
    }

    ext_session_lock_v1_unlock_and_destroy(state.lock);
    wl_display_roundtrip(state.display); /* make sure the request is sent */
    printf("Unlocked.\n");

    wl_display_disconnect(state.display);

    return 0;
}
