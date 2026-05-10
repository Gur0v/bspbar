#define _POSIX_C_SOURCE 200809L

#include "bspwm.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static bool desktop_name_in_output(const char *name, const char *output) {
    size_t name_len = strlen(name);
    const char *p = output;

    while (*p) {
        while (*p == '\n') {
            p++;
        }

        if (strncmp(p, name, name_len) == 0 && (p[name_len] == '\n' || p[name_len] == '\0')) {
            return true;
        }

        const char *next = strchr(p, '\n');
        if (!next) {
            break;
        }
        p = next + 1;
    }

    return false;
}

bool set_monitor_name(char monitor[MAX_NAME], const char *requested) {
    if (requested && requested[0]) {
        snprintf(monitor, MAX_NAME, "%s", requested);
        return true;
    }

    const char *const argv[] = {"bspc", "query", "-M", "-m", "focused", "--names", NULL};
    char out[MAX_CMD_OUT] = {0};
    if (!run_capture(out, sizeof(out), argv)) {
        return false;
    }

    char *line = out;
    trim_leading(&line);
    if (*line == '\0') {
        return false;
    }

    char *newline = strchr(line, '\n');
    if (newline) {
        *newline = '\0';
    }

    snprintf(monitor, MAX_NAME, "%s", line);
    return true;
}

bool load_monitor_geometry(const char *monitor, int *x, int *y, int *w, int *h) {
    const char *const argv[] = {"bspc", "query", "-T", "-m", monitor, NULL};
    char out[MAX_CMD_OUT] = {0};
    const char *needle = "\"rectangle\"";
    const char *p = NULL;

    if (!run_capture(out, sizeof(out), argv)) {
        return false;
    }

    p = strstr(out, needle);
    if (!p) {
        return false;
    }

    return sscanf(
               p,
               "\"rectangle\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d",
               x,
               y,
               w,
               h) == 4 &&
           *w > 0 &&
           *h > 0;
}

bool load_desktops_snapshot(const char *monitor, DesktopSnapshot *snapshot) {
    const char *const names_argv[] = {"bspc", "query", "-D", "-m", monitor, "--names", NULL};
    const char *const focused_argv[] = {"bspc", "query", "-D", "-m", monitor, "-d", "focused", "--names", NULL};
    const char *const occupied_argv[] = {"bspc", "query", "-D", "-m", monitor, "-d", ".occupied", "--names", NULL};
    const char *const urgent_argv[] = {"bspc", "query", "-D", "-m", monitor, "-d", ".urgent", "--names", NULL};

    char out[MAX_CMD_OUT] = {0};
    char focused[MAX_CMD_OUT] = {0};
    char occupied[MAX_CMD_OUT] = {0};
    char urgent[MAX_CMD_OUT] = {0};

    if (!run_capture(out, sizeof(out), names_argv)) {
        return false;
    }

    snapshot->desktop_count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(out, "\n", &save);
         tok && snapshot->desktop_count < MAX_DESKTOPS;
         tok = strtok_r(NULL, "\n", &save)) {
        trim_trailing(tok);
        if (*tok == '\0') {
            continue;
        }

        Desktop *d = &snapshot->desktops[snapshot->desktop_count++];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "%s", tok);
        d->x0 = -1;
        d->x1 = -1;
    }

    if (snapshot->desktop_count == 0) {
        return false;
    }

    run_capture(focused, sizeof(focused), focused_argv);
    run_capture(occupied, sizeof(occupied), occupied_argv);
    run_capture(urgent, sizeof(urgent), urgent_argv);

    for (size_t i = 0; i < snapshot->desktop_count; i++) {
        Desktop *d = &snapshot->desktops[i];
        d->focused = desktop_name_in_output(d->name, focused);
        d->occupied = desktop_name_in_output(d->name, occupied);
        d->urgent = desktop_name_in_output(d->name, urgent);
    }

    return true;
}

bool bspwm_get_bottom_padding(const char *monitor, int *padding) {
    const char *const argv[] = {"bspc", "config", "-m", monitor, "bottom_padding", NULL};
    char out[MAX_CMD_OUT] = {0};
    if (!run_capture(out, sizeof(out), argv)) {
        return false;
    }

    *padding = atoi(out);
    return true;
}

bool bspwm_set_bottom_padding(const char *monitor, int value) {
    char value_buf[32];
    snprintf(value_buf, sizeof(value_buf), "%d", value);

    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        execlp("bspc", "bspc", "config", "-m", monitor, "bottom_padding", value_buf, (char *)NULL);
        _exit(127);
    }

    return waitpid(pid, NULL, 0) > 0;
}

void focus_desktop(const char *name) {
    if (!name || !*name) {
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        execlp("bspc", "bspc", "desktop", "-f", name, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

bool spawn_subscriber_pipe(int *fd, pid_t *pid) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return false;
    }

    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execlp("bspc", "bspc", "subscribe", "report", (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    *fd = pipefd[0];
    *pid = child;
    return true;
}
