#include "utils.h"
#include "select.h"
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

int set_fd_flags(int fd, bool set, int flags) {
  int fdflags = fcntl(fd, F_GETFL, 0);
  if (set)
    fdflags |= flags;
  else
    fdflags &= ~flags;
  return fcntl(fd, F_SETFL, fdflags);
}

bool write_all(int fd, const void *buff, UINT len) {
  static void *sinst = NULL;
  static int prevfd;
  struct wait_list wl = {.fd = fd, .wm = WM_WRITE};
  if (!sinst) {
    sinst = select_init(&wl, 1);
    if (!sinst)
      errx(1, "write_all() init error");
  } else {
    if (prevfd != fd) {
      select_wl_change(sinst, 0, &wl);
      prevfd = fd;
    }
  }

  int written = 0;
  while (written < len) {
    int wfd;
    select_wait(sinst, &wfd);
    int wr = write(wfd, (void *)((uintptr_t)buff + written), len - written);
    if (wr < 0) {
      if (errno == EAGAIN)
        continue;
      return false;
    }
    if (wr == 0) {
      // EOF reached (?) but we haven't written everything
      errno = EIO;
      return false;
    }
    written += wr;
  }

  return true;
}

void wait_debugger() {
  printf("Please attach debugger to PID %d\n", getpid());
  bool stop = false;
  while (!stop)
    sleep(1);
}