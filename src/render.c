#include "render.h"

#include <X11/Xatom.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

static void set_source_hex(cairo_t *cr, const char *hex) {
    unsigned int r = 0;
    unsigned int g = 0;
    unsigned int b = 0;

    if (hex && hex[0] == '#' && strlen(hex) == 7 &&
        sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
        cairo_set_source_rgb(cr, r / 255.0, g / 255.0, b / 255.0);
        return;
    }

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
}

static int text_width(App *app, const char *text) {
    if (!text || !*text) {
        return 0;
    }

    int width = 0;
    pango_layout_set_text(app->layout, text, -1);
    pango_layout_get_pixel_size(app->layout, &width, NULL);
    return width;
}

static void draw_text(App *app, int x, const char *hex, const char *text) {
    if (!text || !*text) {
        return;
    }

    int height = 0;
    pango_layout_set_text(app->layout, text, -1);
    pango_layout_get_pixel_size(app->layout, NULL, &height);

    set_source_hex(app->cr, hex);
    cairo_move_to(app->cr, x, (BAR_HEIGHT - height) / 2);
    pango_cairo_show_layout(app->cr, app->layout);
}

static bool desktop_visible(const Desktop *d) {
    return d->focused || d->occupied || d->urgent;
}

bool render_init(App *app) {
    app->dpy = XOpenDisplay(NULL);
    if (!app->dpy) {
        fprintf(stderr, "bspbar: failed to open X display\n");
        return false;
    }

    app->screen = DefaultScreen(app->dpy);
    app->root = RootWindow(app->dpy, app->screen);

    app->font = pango_font_description_from_string(FONT_NAME);
    if (!app->font) {
        fprintf(stderr, "bspbar: failed to load font `%s`\n", FONT_NAME);
        return false;
    }

    XSetWindowAttributes attrs = {0};
    attrs.override_redirect = False;
    attrs.background_pixel = BlackPixel(app->dpy, app->screen);
    attrs.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask;

    app->win = XCreateWindow(
        app->dpy,
        app->root,
        app->mon_x,
        app->mon_y + app->mon_h - BAR_HEIGHT,
        (unsigned int)app->mon_w,
        BAR_HEIGHT,
        0,
        CopyFromParent,
        InputOutput,
        CopyFromParent,
        CWBackPixel | CWEventMask | CWOverrideRedirect,
        &attrs);

    app->surface = cairo_xlib_surface_create(
        app->dpy,
        app->win,
        DefaultVisual(app->dpy, app->screen),
        app->mon_w,
        BAR_HEIGHT);
    app->cr = cairo_create(app->surface);
    app->layout = pango_cairo_create_layout(app->cr);
    if (!app->surface || !app->cr || !app->layout) {
        fprintf(stderr, "bspbar: failed to create drawing context\n");
        return false;
    }

    cairo_font_options_t *font_options = cairo_font_options_create();
    cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_SUBPIXEL);
    cairo_font_options_set_hint_style(font_options, CAIRO_HINT_STYLE_FULL);
    cairo_font_options_set_hint_metrics(font_options, CAIRO_HINT_METRICS_ON);
    cairo_set_font_options(app->cr, font_options);
    pango_cairo_context_set_font_options(pango_layout_get_context(app->layout), font_options);
    cairo_font_options_destroy(font_options);

    pango_layout_set_font_description(app->layout, app->font);
    pango_layout_set_single_paragraph_mode(app->layout, TRUE);

    Atom type_atom = XInternAtom(app->dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom dock_atom = XInternAtom(app->dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(app->dpy, app->win, type_atom, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dock_atom, 1);

    Atom state_atom = XInternAtom(app->dpy, "_NET_WM_STATE", False);
    Atom sticky_atom = XInternAtom(app->dpy, "_NET_WM_STATE_STICKY", False);
    Atom skip_taskbar_atom = XInternAtom(app->dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom skip_pager_atom = XInternAtom(app->dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    Atom states[] = {sticky_atom, skip_taskbar_atom, skip_pager_atom};
    XChangeProperty(app->dpy, app->win, state_atom, XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 3);

    Atom desktop_atom = XInternAtom(app->dpy, "_NET_WM_DESKTOP", False);
    unsigned long all_desktops = 0xFFFFFFFFUL;
    XChangeProperty(app->dpy, app->win, desktop_atom, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&all_desktops, 1);

    app->wm_delete = XInternAtom(app->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app->dpy, app->win, &app->wm_delete, 1);
    XStoreName(app->dpy, app->win, "bspbar");
    XMapRaised(app->dpy, app->win);
    XFlush(app->dpy);
    return true;
}

void render_destroy(App *app) {
    if (app->layout) {
        g_object_unref(app->layout);
        app->layout = NULL;
    }
    if (app->font) {
        pango_font_description_free(app->font);
        app->font = NULL;
    }
    if (app->cr) {
        cairo_destroy(app->cr);
        app->cr = NULL;
    }
    if (app->surface) {
        cairo_surface_destroy(app->surface);
        app->surface = NULL;
    }
    if (app->win) {
        XDestroyWindow(app->dpy, app->win);
        app->win = 0;
    }
    if (app->dpy) {
        XCloseDisplay(app->dpy);
        app->dpy = NULL;
    }
}

void render_update_clock(App *app) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(app->clock_text, sizeof(app->clock_text), "%Y-%m-%d %I:%M:%S %p", &tm);
}

void render_update_status(App *app) {
    snprintf(
        app->status,
        sizeof(app->status),
        "%s %s %s",
        app->volume[0] ? app->volume : "??%",
        app->kbd_layout[0] ? app->kbd_layout : "??",
        app->clock_text[0] ? app->clock_text : "");
}

void render_bar(App *app) {
    set_source_hex(app->cr, BACKGROUND);
    cairo_rectangle(app->cr, 0, 0, app->mon_w, BAR_HEIGHT);
    cairo_fill(app->cr);

    int status_width = text_width(app, app->status);
    int status_x = app->mon_w - RIGHT_PADDING - status_width;
    draw_text(app, status_x, FOREGROUND, app->status);

    int x = LEFT_PADDING;
    for (size_t i = 0; i < app->desktop_count; i++) {
        Desktop *d = &app->desktops[i];
        d->x0 = d->x1 = -1;
        if (!desktop_visible(d)) {
            continue;
        }

        const char *color = DIM_FOREGROUND;
        if (d->focused) {
            color = FOREGROUND;
        } else if (d->urgent) {
            color = URGENT_FOREGROUND;
        }

        int w = text_width(app, d->name);
        if (x + w > status_x - STATUS_GAP) {
            break;
        }

        d->x0 = x;
        d->x1 = x + w;
        draw_text(app, x, color, d->name);
        x += w + TAG_SPACING;
    }

    cairo_surface_flush(app->surface);
    XFlush(app->dpy);
}
