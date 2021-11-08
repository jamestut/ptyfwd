#pragma once

#include "common.h"
#include <stdbool.h>

#define MAX_SELECT 8

enum wait_mode { WM_READ = 1, WM_WRITE = 2 };

struct wait_list {
  int fd;
  enum wait_mode wm;
};

void *select_init(struct wait_list *wl, UINT fdscount);

bool select_resize(void *inst, UINT newsize);

bool select_wl_change(void *inst, UINT idx, struct wait_list *wl);

int select_wait(void *inst, int *fds);

void select_destroy(void *inst);
