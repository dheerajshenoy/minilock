#pragma once

#define _GNU_SOURCE /* memfd_create, explicit_bzero: must come first */
#include "ext-session-lock-v1-client-protocol.h"
#include "keypress_indicator.h"

#include <cairo.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-client.h>

struct state; /* forward declaration */

struct output;

/* One persistently-mapped shm buffer. */
struct shm_buf
{
    struct output *out; /* whose buffer this is */
    struct wl_buffer *buf;
    uint32_t *px;
    size_t size;
    bool busy; /* attached and not yet released by the compositor */
    cairo_surface_t *cairo_surface;
};

struct rect
{
    int x0, y0, x1, y1; /* empty when x1 <= x0 */
};

/* For one output column/row: the two image columns/rows to sample and the
 * weight (0-255) of the second. Nearest-neighbour uses i0 == i1, f == 0. */
struct axis
{
    uint32_t i0, i1;
    uint8_t f;
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
    bool redraw;            /* wanted a redraw but both buffers were in use */
    bool indicator_dirty;   /* the indicator changed since the last present */

    /* Animation: the image scaled to this output (NULL for still images). */
    uint32_t *canvas;
    struct axis *xm, *ym; /* output column/row -> image sample positions */
    struct rect dirty;    /* changed since the last present */
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
    bool caps_lock; /* the Caps Lock modifier is locked on */
    bool auth_pending;
    char auth_pw[256];
    atomic_int auth_result; /* 0 running, 1 ok, 2 wrong */
    int auth_efd;           /* eventfd signalled when the worker finishes */

    struct image *img;

    /* Animation source, shared by all outputs: the composited frame at the
     * image's own resolution. Palette (GIF) frames are drawn onto src_canvas;
     * whole-frame animations just point src_cur at the current frame. */
    uint32_t *src_canvas, *src_prev;
    const uint32_t *src_cur;
    int src_shown;         /* frame composited so far, -1 for none */
    struct rect src_dirty; /* region the last step changed */
    enum KeypressIndicatorState keypress_indicator_state;
    int indicator_tfd; /* one-shot timer: typing/failed -> idle, or -1 */
};

int
minilock_init(int argc, char *argv[]);
