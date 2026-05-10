#ifndef BSPBAR_APP_H
#define BSPBAR_APP_H

#include "../config.h"

#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include <pango/pango.h>
#include <xkbcommon/xkbcommon.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char name[MAX_NAME];
    bool focused;
    bool occupied;
    bool urgent;
    int x0;
    int x1;
} Desktop;

typedef struct {
    Desktop desktops[MAX_DESKTOPS];
    size_t desktop_count;
} DesktopSnapshot;

typedef enum {
    WORKER_MSG_VOLUME = 1,
    WORKER_MSG_DESKTOPS = 2,
} WorkerMessageType;

typedef struct {
    WorkerMessageType type;
    union {
        char volume[16];
        DesktopSnapshot desktops;
    } payload;
} WorkerMessage;

typedef struct {
    Desktop desktops[MAX_DESKTOPS];
    size_t desktop_count;
    char monitor[MAX_NAME];
    int mon_x;
    int mon_y;
    int mon_w;
    int mon_h;
    int padding_before;
    bool padding_changed;
    atomic_bool running;

    char volume[16];
    char kbd_layout[16];
    char clock_text[48];
    char status[MAX_STATUS];

    Display *dpy;
    xcb_connection_t *xcb;
    int screen;
    Window root;
    Window win;
    cairo_surface_t *surface;
    cairo_t *cr;
    PangoLayout *layout;
    PangoFontDescription *font;
    Atom wm_delete;
    int xkb_event_base;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    int32_t xkb_device_id;

    int worker_fd;
    int worker_write_fd;
    pthread_t desktop_thread;
    bool desktop_thread_started;
    void *pipewire_volume;
    bool pipewire_started;
} App;

#endif
