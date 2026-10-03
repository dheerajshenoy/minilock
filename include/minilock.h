#pragma once

#define _GNU_SOURCE /* memfd_create, explicit_bzero: must come first */
#include "ext-session-lock-v1-client-protocol.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-client.h>

struct state; /* forward declaration */

/* One persistently-mapped shm buffer. */
struct shm_buf
{
    struct wl_buffer *buf;
    uint32_t *px;
    size_t size;
    bool busy; /* attached and not yet released by the compositor */
};

struct output
{
    struct wl_output *wl_output;
    uint32_t name;
    struct output *next;
    struct state *state;
    struct wl_surface *surface;
    struct ext_session_lock_surface_v1 *lock_surface;
    uint32_t width, height;
    struct shm_buf bufs[2]; /* double buffered so animation can reuse them */

    /* Animation state, at this output's resolution (NULL for still images). */
    uint32_t *canvas;       /* the picture after drawing frame `shown` */
    uint32_t *prev;         /* saved canvas for FRAME_RESTORE */
    uint32_t *xmap, *ymap;  /* output column/row -> image column/row */
    int shown;              /* frame currently composited, -1 for none */
    int dirty_x0, dirty_y0, dirty_x1, dirty_y1; /* not yet presented */
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

    /* PAM runs on a worker thread so a failed attempt's delay (pam_faildelay,
     * usually ~2s) doesn't freeze the event loop and the animation. */
    bool auth_pending;
    char auth_pw[256];
    atomic_int auth_result; /* 0 running, 1 ok, 2 wrong */
    int auth_efd;           /* eventfd signalled when the worker finishes */

    struct image *img;
};

int
minilock_init(int argc, char *argv[]);
