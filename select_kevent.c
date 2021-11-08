#ifdef __APPLE__

#include "select.h"
#include <err.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <unistd.h>

struct kevent_select {
  int kq;
  int count;
  struct kevent kev[MAX_SELECT];
};

void *select_init(struct wait_list *wl, UINT fdscount) {
  if (fdscount > MAX_SELECT)
    err(1, "Cannot select more than %d!", MAX_SELECT);

  int kq = kqueue();
  if (kq < 0) {
    warn("kqueue error");
    return NULL;
  }

  struct kevent_select *ret = malloc(sizeof(struct kevent_select));
  ret->kq = kq;
  ret->count = fdscount;
  for (int i = 0; i < fdscount; ++i)
    select_wl_change(ret, i, wl + i);
  return (void *)ret;
}

bool select_resize(void *inst, UINT newsize) {
  struct kevent_select *ks = inst;
  if (newsize > MAX_SELECT) {
    warnx("Cannot select more than %d!", MAX_SELECT);
    return false;
  }
  ks->count = newsize;
  return true;
}

bool select_wl_change(void *inst, UINT idx, struct wait_list *wl) {
  if (idx >= MAX_SELECT) {
    warnx("Invalid index");
    return false;
  }
  struct kevent_select *ks = inst;
  int evfilt = (wl->wm & WM_READ) ? EVFILT_READ : 0;
  evfilt |= (wl->wm & WM_WRITE) ? EVFILT_WRITE : 0;
  EV_SET(ks->kev + idx, wl->fd, evfilt, EV_ADD | EV_ENABLE, 0, 0, 0);
  return true;
}

int select_wait(void *inst, int *fds) {
  struct kevent_select *ks = inst;
  struct kevent *kevres = alloca(sizeof(struct kevent) * ks->count);
  int evs = kevent(ks->kq, ks->kev, ks->count, kevres, ks->count, NULL);

  for (int i = 0; i < evs; ++i)
    fds[i] = kevres[i].ident;

  return evs;
}

void select_destroy(void *inst) {
  struct kevent_select *ks = inst;
  close(ks->kq);
  free(inst);
}

#endif
