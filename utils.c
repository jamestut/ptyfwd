#include "utils.h"
#include "select.h"
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

bool rw_all(bool iswrite, int fd, const void *buff, UINT len);

int set_fd_flags(int fd, bool set, int flags) {
  int fdflags = fcntl(fd, F_GETFL, 0);
  if (set)
    fdflags |= flags;
  else
    fdflags &= ~flags;
  return fcntl(fd, F_SETFL, fdflags);
}

bool write_all(int fd, const void *buff, UINT len) { return rw_all(true, fd, buff, len); }

bool read_all(int fd, const void *buff, UINT len) { return rw_all(false, fd, buff, len); }

bool rw_all(bool iswrite, int fd, const void *buff, UINT len) {
  static void *sinst = NULL;
  struct wait_list wl = {.fd = fd, .wm = iswrite ? WM_WRITE : WM_READ};
  if (!sinst) {
    sinst = select_init(&wl, 1);
    if (!sinst)
      errx(1, "rw_all() select init error");
  } else {
    select_wl_change(sinst, 0, &wl);
  }

  int done = 0;
  while (done < len) {
    int wfd;
    select_wait(sinst, &wfd);
    int currdone = iswrite ? write(fd, (void *)((uintptr_t)buff + done), len - done)
                           : read(fd, (void *)((uintptr_t)buff + done), len - done);
    if (currdone < 0) {
      if (errno == EAGAIN)
        continue;
      return false;
    }
    if (currdone == 0) {
      // EOF reached (?) but we haven't written everything
      errno = EIO;
      return false;
    }
    done += currdone;
  }

  return true;
}

void wait_debugger() {
  printf("Please attach debugger to PID %d\n", getpid());
  bool stop = false;
  while (!stop)
    sleep(1);
}