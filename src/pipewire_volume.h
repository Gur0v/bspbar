#ifndef BSPBAR_PIPEWIRE_VOLUME_H
#define BSPBAR_PIPEWIRE_VOLUME_H

#include "app.h"

#include <stdbool.h>

bool pipewire_volume_start(App *app);
void pipewire_volume_stop(App *app);

#endif
