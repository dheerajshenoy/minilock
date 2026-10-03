#pragma once

#define _GNU_SOURCE
#include "decoder.h"
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

    uint32_t *img_px;
    int img_w, img_h;
};

static bool
load_image(struct state *s, const char *path)
{
#ifdef HAVE_PNG
    if (load_png(path, &s->img_px, &s->img_w, &s->img_h))
        return true;
#endif
#ifdef HAVE_JPEG
    if (load_jpeg(path, &s->img_px, &s->img_w, &s->img_h))
        return true;
#endif
    fprintf(stderr, "Cannot load image (unsupported or invalid): %s\n", path);
    return false;
}
