#ifndef BSPBAR_UTIL_H
#define BSPBAR_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

void trim_trailing(char *s);
void trim_leading(char **s);
bool run_capture(char *buf, size_t len, const char *const argv[]);
bool set_nonblocking(int fd);
bool write_full(int fd, const void *buf, size_t len);
struct timespec now_monotonic(void);
struct timespec add_ms(struct timespec ts, long ms);
long ms_until(struct timespec deadline, struct timespec now);

#endif
