#ifdef __linux__

#include "select.h"
#include <err.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

struct epoll_select {
  int epfd;
  int count;
  int fds[MAX_SELECT];
};

void *select_init(struct wait_list *wl, UINT fdscount) {
  int fd = epoll_create(1);
  if (fd < 0) {
    warn("epoll_create error");
    return NULL;
  }

  struct epoll_select *ret = malloc(sizeof(struct epoll_select));
  if (!ret) {
    warnx("malloc error");
    close(fd);
    return NULL;
  }

  ret->epfd = fd;
  ret->count = fdscount;
  memset(ret->fds, -1, sizeof(ret->fds));

  for (int i = 0; i < fdscount; ++i)
    select_wl_change(ret, i, wl + i);

  return (void *)ret;
}

bool select_resize(void *inst, UINT newsize) {
  struct epoll_select *ret = inst;
  if (newsize > MAX_SELECT) {
    warnx("Cannot select more than %d!", MAX_SELECT);
    return false;
  }
  for (int i = newsize; i < ret->count; ++i) {
    if (ret->fds[i] >= 0) {
      if (epoll_ctl(ret->epfd, EPOLL_CTL_DEL, ret->fds[i], NULL) < 0)
        warn("Error epoll_ctl_del");
      ret->fds[i] = -1;
    }
  }
  ret->count = newsize;
  return true;
}

bool select_wl_change(void *inst, UINT idx, struct wait_list *wl) {
  struct epoll_select *ret = inst;
  if (idx >= ret->count) {
    warnx("Invalid index");
    return false;
  }

  struct epoll_event st;
  st.data.fd = wl->fd;
  st.events =
      EPOLLERR | EPOLLRDHUP | EPOLLHUP | ((wl->wm & WM_READ) ? EPOLLIN : 0) | ((wl->wm & WM_WRITE) ? EPOLLOUT : 0);
  int stat = 0;
  int *prevfds = ret->fds + idx;
  if (*prevfds == wl->fd)
    stat = epoll_ctl(ret->epfd, EPOLL_CTL_MOD, wl->fd, &st);
  else if (*prevfds >= 0) {
    epoll_ctl(ret->epfd, EPOLL_CTL_DEL, *prevfds, NULL);
    *prevfds = -1;
  }
  if (*prevfds < 0)
    stat = epoll_ctl(ret->epfd, EPOLL_CTL_ADD, wl->fd, &st);
  *prevfds = wl->fd;

  if (stat < 0) {
    warn("epoll_ctl error");
    return false;
  }

  return true;
}

int select_wait(void *inst, int *fds) {
  struct epoll_select *ret = inst;
  struct epoll_event epev[MAX_SELECT];
  int count = epoll_wait(ret->epfd, epev, MAX_SELECT, -1);
  for (int i = 0; i < count; ++i)
    fds[i] = epev[i].data.fd;
  return count;
}

void select_destroy(void *inst) {
  struct epoll_select *ret = inst;
  close(ret->epfd);
  free(inst);
}

#endif
