#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

void trim_trailing(char *s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

void trim_leading(char **s) {
    while (**s && isspace((unsigned char)**s)) {
        (*s)++;
    }
}

bool run_capture(char *buf, size_t len, const char *const argv[]) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(pipefd[1]);
    ssize_t total = 0;
    while ((size_t)total + 1 < len) {
        ssize_t n = read(pipefd[0], buf + total, len - (size_t)total - 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    buf[total > 0 ? total : 0] = '\0';
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return false;
    }

    trim_trailing(buf);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool write_full(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t written = 0;

    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += (size_t)n;
    }

    return true;
}

struct timespec now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

struct timespec add_ms(struct timespec ts, long ms) {
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

long ms_until(struct timespec deadline, struct timespec now) {
    long sec = (long)(deadline.tv_sec - now.tv_sec);
    long nsec = deadline.tv_nsec - now.tv_nsec;
    long ms = sec * 1000L + nsec / 1000000L;
    return ms < 0 ? 0 : ms;
}
