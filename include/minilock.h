#pragma once

#define _GNU_SOURCE /* memfd_create, explicit_bzero: must come first */
#include "ext-session-lock-v1-client-protocol.h"

#include <stdbool.h>
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

    struct image *img;
};

int
minilock_init(int argc, char *argv[]);
