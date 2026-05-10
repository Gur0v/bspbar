#ifndef BSPBAR_WORKERS_H
#define BSPBAR_WORKERS_H

#include "app.h"

#include <stdbool.h>

bool start_workers(App *app);
void stop_workers(App *app);
bool workers_drain(App *app);

#endif
