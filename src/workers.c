#define _POSIX_C_SOURCE 200809L

#include "workers.h"
#include "bspwm.h"
#include "pipewire_volume.h"
#include "util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void *desktop_thread_main(void *arg) {
    App *app = (App *)arg;
    DesktopSnapshot last = {0};
    bool have_last = false;

    while (atomic_load(&app->running)) {
        DesktopSnapshot snapshot = {0};
        if (load_desktops_snapshot(app->monitor, &snapshot) &&
            (!have_last || memcmp(&snapshot, &last, sizeof(snapshot)) != 0)) {
            WorkerMessage msg = {.type = WORKER_MSG_DESKTOPS};
            msg.payload.desktops = snapshot;
            if (!write_full(app->worker_write_fd, &msg, sizeof(msg))) {
                break;
            }
            last = snapshot;
            have_last = true;
        }

        int sub_fd = -1;
        pid_t sub_pid = -1;
        if (!spawn_subscriber_pipe(&sub_fd, &sub_pid)) {
            struct timespec retry = {
                .tv_sec = SUBSCRIBE_RETRY_MS / 1000,
                .tv_nsec = (SUBSCRIBE_RETRY_MS % 1000) * 1000000L,
            };
            nanosleep(&retry, NULL);
            continue;
        }

        while (atomic_load(&app->running)) {
            struct pollfd pfd = {.fd = sub_fd, .events = POLLIN | POLLHUP};
            int pr = poll(&pfd, 1, 500);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (pr == 0) {
                continue;
            }

            if (pfd.revents & (POLLIN | POLLHUP)) {
                char buf[256];
                ssize_t n = read(sub_fd, buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }

                DesktopSnapshot next = {0};
                if (load_desktops_snapshot(app->monitor, &next) &&
                    (!have_last || memcmp(&next, &last, sizeof(next)) != 0)) {
                    WorkerMessage msg = {.type = WORKER_MSG_DESKTOPS};
                    msg.payload.desktops = next;
                    if (!write_full(app->worker_write_fd, &msg, sizeof(msg))) {
                        close(sub_fd);
                        kill(sub_pid, SIGTERM);
                        waitpid(sub_pid, NULL, 0);
                        return NULL;
                    }
                    last = next;
                    have_last = true;
                }
            }
        }

        close(sub_fd);
        kill(sub_pid, SIGTERM);
        waitpid(sub_pid, NULL, 0);
    }

    return NULL;
}

bool start_workers(App *app) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return false;
    }

    if (!set_nonblocking(pipefd[0])) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    app->worker_fd = pipefd[0];
    app->worker_write_fd = pipefd[1];

    if (!pipewire_volume_start(app)) {
        close(app->worker_fd);
        close(app->worker_write_fd);
        app->worker_fd = -1;
        app->worker_write_fd = -1;
        return false;
    }

    if (pthread_create(&app->desktop_thread, NULL, desktop_thread_main, app) != 0) {
        stop_workers(app);
        return false;
    }
    app->desktop_thread_started = true;

    return true;
}

void stop_workers(App *app) {
    atomic_store(&app->running, false);

    if (app->pipewire_started) {
        pipewire_volume_stop(app);
    }
    if (app->desktop_thread_started) {
        pthread_join(app->desktop_thread, NULL);
        app->desktop_thread_started = false;
    }

    if (app->worker_fd >= 0) {
        close(app->worker_fd);
        app->worker_fd = -1;
    }
    if (app->worker_write_fd >= 0) {
        close(app->worker_write_fd);
        app->worker_write_fd = -1;
    }
}

bool workers_drain(App *app) {
    WorkerMessage msg;
    bool changed = false;

    while (read(app->worker_fd, &msg, sizeof(msg)) == (ssize_t)sizeof(msg)) {
        if (msg.type == WORKER_MSG_VOLUME) {
            if (strcmp(app->volume, msg.payload.volume) != 0) {
                snprintf(app->volume, sizeof(app->volume), "%s", msg.payload.volume);
                changed = true;
            }
        } else if (msg.type == WORKER_MSG_DESKTOPS) {
            bool snapshot_changed = app->desktop_count != msg.payload.desktops.desktop_count ||
                                    memcmp(app->desktops, msg.payload.desktops.desktops, sizeof(app->desktops)) != 0;
            if (snapshot_changed) {
                app->desktop_count = msg.payload.desktops.desktop_count;
                memcpy(app->desktops, msg.payload.desktops.desktops, sizeof(app->desktops));
                changed = true;
            }
        }
    }

    return changed;
}
