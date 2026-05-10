#define _POSIX_C_SOURCE 200809L

#include "app.h"
#include "bspwm.h"
#include "layout.h"
#include "render.h"
#include "util.h"
#include "workers.h"

#include <X11/Xlib.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static App *g_app;

static void cleanup(void) {
    if (!g_app) {
        return;
    }

    stop_workers(g_app);
    layout_destroy(g_app);
    render_destroy(g_app);

    if (g_app->padding_changed) {
        bspwm_set_bottom_padding(g_app->monitor, g_app->padding_before);
        g_app->padding_changed = false;
    }
}

static void on_signal(int sig) {
    (void)sig;
    if (g_app) {
        atomic_store(&g_app->running, false);
    }
}

static bool install_signal_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGINT, &sa, NULL) == 0 &&
           sigaction(SIGTERM, &sa, NULL) == 0;
}

static void usage(FILE *out) {
    fprintf(out, "Usage: bspbar [--monitor <name>]\n");
}

static void focus_relative_visible(App *app, int direction) {
    ssize_t visible_index = -1;
    ssize_t focused_index = -1;

    for (size_t i = 0; i < app->desktop_count; i++) {
        if (!(app->desktops[i].focused || app->desktops[i].occupied || app->desktops[i].urgent)) {
            continue;
        }
        visible_index++;
        if (app->desktops[i].focused) {
            focused_index = visible_index;
            break;
        }
    }

    if (focused_index < 0) {
        return;
    }

    visible_index = -1;
    for (size_t i = 0; i < app->desktop_count; i++) {
        Desktop *d = &app->desktops[i];
        if (!(d->focused || d->occupied || d->urgent)) {
            continue;
        }
        visible_index++;
        if (visible_index == focused_index + direction) {
            focus_desktop(d->name);
            return;
        }
    }
}

static void handle_button(App *app, XButtonEvent *event) {
    if (event->button == Button4) {
        focus_relative_visible(app, -1);
        return;
    }
    if (event->button == Button5) {
        focus_relative_visible(app, 1);
        return;
    }
    if (event->button != Button1) {
        return;
    }

    for (size_t i = 0; i < app->desktop_count; i++) {
        Desktop *d = &app->desktops[i];
        if (d->x0 >= 0 && event->x >= d->x0 && event->x <= d->x1) {
            focus_desktop(d->name);
            return;
        }
    }
}

int main(int argc, char **argv) {
    int exit_code = 0;
    App *app = calloc(1, sizeof(*app));
    if (!app) {
        fprintf(stderr, "bspbar: out of memory\n");
        return 1;
    }
    app->worker_fd = -1;
    app->worker_write_fd = -1;
    atomic_init(&app->running, true);
    g_app = app;

    const char *monitor_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--monitor") == 0) && i + 1 < argc) {
            monitor_arg = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            goto done;
        } else {
            usage(stderr);
            exit_code = 1;
            goto done;
        }
    }

    if (!install_signal_handlers()) {
        fprintf(stderr, "bspbar: failed to install signal handlers\n");
        exit_code = 1;
        goto done;
    }

    if (!set_monitor_name(app->monitor, monitor_arg)) {
        fprintf(stderr, "bspbar: failed to determine monitor\n");
        exit_code = 1;
        goto done;
    }

    if (!load_monitor_geometry(app->monitor, &app->mon_x, &app->mon_y, &app->mon_w, &app->mon_h)) {
        fprintf(stderr, "bspbar: failed to read geometry for monitor `%s`\n", app->monitor);
        exit_code = 1;
        goto done;
    }

    if (!render_init(app)) {
        exit_code = 1;
        goto done;
    }

    if (!layout_init(app)) {
        fprintf(stderr, "bspbar: failed to initialize XKB state\n");
        exit_code = 1;
        goto done;
    }
    layout_update(app);
    snprintf(app->volume, sizeof(app->volume), "??%%");
    render_update_clock(app);
    render_update_status(app);

    if (bspwm_get_bottom_padding(app->monitor, &app->padding_before) &&
        bspwm_set_bottom_padding(app->monitor, BAR_HEIGHT)) {
        app->padding_changed = true;
    }

    render_bar(app);

    if (!start_workers(app)) {
        fprintf(stderr, "bspbar: failed to start background workers\n");
        exit_code = 1;
        goto done;
    }

    struct timespec next_clock = add_ms(now_monotonic(), CLOCK_INTERVAL_MS);
    while (atomic_load(&app->running)) {
        struct pollfd fds[2];
        nfds_t nfds = 0;

        fds[nfds].fd = ConnectionNumber(app->dpy);
        fds[nfds].events = POLLIN;
        nfds++;

        if (app->worker_fd >= 0) {
            fds[nfds].fd = app->worker_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        long timeout_ms = ms_until(next_clock, now_monotonic());
        int poll_result = poll(fds, nfds, (int)timeout_ms);
        if (poll_result < 0 && errno != EINTR) {
            break;
        }

        while (XPending(app->dpy)) {
            XEvent ev;
            XNextEvent(app->dpy, &ev);

            if (ev.type == Expose) {
                render_bar(app);
            } else if (ev.type == ButtonPress) {
                handle_button(app, &ev.xbutton);
            } else if (ev.type == ClientMessage &&
                       (Atom)ev.xclient.data.l[0] == app->wm_delete) {
                atomic_store(&app->running, false);
            } else if (app->xkb_event_base > 0 && ev.type == app->xkb_event_base) {
                layout_update(app);
                render_update_status(app);
                render_bar(app);
            }
        }

        if (app->worker_fd >= 0 && nfds > 1 && (fds[1].revents & POLLIN)) {
            if (workers_drain(app)) {
                render_update_status(app);
                render_bar(app);
            }
        }

        struct timespec now = now_monotonic();
        if (ms_until(next_clock, now) == 0) {
            render_update_clock(app);
            render_update_status(app);
            render_bar(app);
            next_clock = add_ms(now, CLOCK_INTERVAL_MS);
        }
    }

done:
    cleanup();
    g_app = NULL;
    free(app);
    return exit_code;
}
