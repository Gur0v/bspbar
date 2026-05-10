#ifndef BSPBAR_BSPWM_H
#define BSPBAR_BSPWM_H

#include "app.h"

#include <stdbool.h>
#include <sys/types.h>

bool set_monitor_name(char monitor[MAX_NAME], const char *requested);
bool load_monitor_geometry(const char *monitor, int *x, int *y, int *w, int *h);
bool load_desktops_snapshot(const char *monitor, DesktopSnapshot *snapshot);
bool bspwm_get_bottom_padding(const char *monitor, int *padding);
bool bspwm_set_bottom_padding(const char *monitor, int value);
void focus_desktop(const char *name);
bool spawn_subscriber_pipe(int *fd, pid_t *pid);

#endif
