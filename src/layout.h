#ifndef BSPBAR_LAYOUT_H
#define BSPBAR_LAYOUT_H

#include "app.h"

#include <stdbool.h>

bool layout_init(App *app);
void layout_destroy(App *app);
bool layout_update(App *app);

#endif
