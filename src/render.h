#ifndef BSPBAR_RENDER_H
#define BSPBAR_RENDER_H

#include "app.h"

#include <stdbool.h>

bool render_init(App *app);
void render_destroy(App *app);
void render_update_clock(App *app);
void render_update_status(App *app);
void render_bar(App *app);

#endif
