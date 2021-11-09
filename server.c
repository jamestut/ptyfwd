#include "common.h"
#include "protocol.h"
#include "select.h"
#include "socks.h"
#include "utils.h"
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#define LISTEN_BACKLOG 8

static void server_worker_loop(int commfd, char *launchreq);

static void set_winsize(int fd, const struct winch_data *data);

static char rbuff[BUFF_SIZE];

int start_server(struct fd_list *fds, const char *launchreq) {
  struct wait_list *wl = alloca(sizeof(struct wait_list) * fds->count);
  for (int i = 0; i < fds->count; ++i) {
    wl[i].fd = fds->fds[i];
    wl[i].wm = WM_READ;
    if (listen(wl[i].fd, LISTEN_BACKLOG) < 0) {
      warn("Listen error");
      return 1;
    }
  }

  void *sinst = select_init(wl, fds->count);
  if (!sinst)
    return 1;

  int *readyfds = alloca(sizeof(int) * fds->count);
  int readyfdscount;
  for (;;) {
    readyfdscount = select_wait(sinst, readyfds);
    if (readyfdscount < 0) {
      if (errno == EINTR) {
        // TODO: handle signal and exit?
        continue;
      }
      warn("Wait error");
      continue;
    }
    for (int i = 0; i < readyfdscount; ++i) {
      int commfd = accept(readyfds[i], NULL, NULL);
      if (commfd < 0) {
        warn("Error accepting connection");
        continue;
      }
      set_fd_flags(commfd, true, O_NONBLOCK);
      pid_t pid = fork();
      if (pid < 0) {
        warn("Fork failed");
        continue;
      } else if (pid) {
        // parent. we don't need the commfd here.
        close(commfd);
        // worker is @ grandchild. child will be killed immediately.
        waitpid(pid, NULL, 0);
      } else {
        // child. fork again! we don't want to deal with zombies.
        pid_t pid2 = fork();
        if (pid2 < 0)
          err(1, "Double fork error");
        if (pid2)
          exit(0);
        // from this point on is the grandchild, which will do all the job.
        // no return
        char *launchreq_c = alloca(strlen(launchreq) + 1);
        strcpy(launchreq_c, launchreq);
        // never return
        server_worker_loop(commfd, launchreq_c);
      }
    }
  }

  select_destroy(sinst);
  return 0;
}

static void server_worker_loop(int commfd, char *launchreq) {
  // controlling (m) pty
  int ptym = open("/dev/ptmx", O_RDWR);
  if (ptym < 0)
    err(1, "Error opening ptmx");
  set_fd_flags(ptym, true, O_NONBLOCK);

  if (grantpt(ptym) < 0)
    err(1, "grantpt error");
  if (unlockpt(ptym) < 0)
    err(1, "unlockpt error");

  // controlled (s) pty
  char pts_name[32];
  char *ptsnameres = ptsname(ptym);
  if (!ptsnameres)
    err(1, "Error getting name for sPTY");
  strcpy(pts_name, ptsnameres);

  int ptys = open(pts_name, O_RDWR);
  if (ptys < 0)
    err(1, "Error opening sPTY");

  pid_t pid = fork();
  if (pid < 0)
    err(1, "Error spawning process");
  if (!pid) {
    // child.

    // we're done with mPTY
    close(ptym);
    // child also no need to deal with commsock
    close(commfd);

    // this must be done in this exact order to make this process
    // as both session leader and controlling terminal
    if (setsid() < 0)
      err(1, "Error setting session leader");
    if (ioctl(ptys, TIOCSCTTY, 0) < 0)
      err(1, "Error setting controlling terminal");
    if (tcsetpgrp(ptys, getpid()) < 0)
      err(1, "Error setting foreground process group");

    // make sPTY our stdio!
    for (int i = 0; i <= 2; ++i) {
      if (dup2(ptys, i) != i)
        err(1, "Error dup2 sPTY to stdio");
    }

    char *args[2] = {launchreq, NULL};
    if (execvp(launchreq, args) < 0)
      err(1, "exec error");
  }

  // parent
  // this loop basically:
  // - read remote, write to PTM
  // - read PTM, write to remote

  // wait list for reading
  struct wait_list rwl[2];
  rwl[0].fd = commfd;
  rwl[1].fd = ptym;
  rwl[0].wm = rwl[1].wm = WM_READ;
  void *rsinst = select_init(rwl, 2);

  int readyfds[2];
  const char *errmsg = NULL;
  bool stop = false;
  while (!(errmsg || stop)) {
    int readyfdscount = select_wait(rsinst, readyfds);
    if (readyfdscount < 0) {
      if (errno == EINTR) {
        // TODO: handle signal?
        continue;
      }
      errmsg = "Wait error";
    }

    for (int i = 0; i < readyfdscount; ++i) {
      int srcfd = readyfds[i];
      if (srcfd == commfd) {
        uint16_t rdlen;
        enum data_type pdatatype;
        if (!proto_read(commfd, &rdlen, &pdatatype, rbuff)) {
          errmsg = "Socket read error";
          break;
        }

        switch (pdatatype) {
        case DT_WINCH:
          set_winsize(ptys, (struct winch_data *)rbuff);
          break;
        case DT_REGULAR:
          if (!write_all(ptym, rbuff, rdlen))
            errmsg = "mPTY write error";
          break;
        case DT_CLOSE:
          stop = true;
          break;
        case DT_NONE:
          break;
        default:
          warnx("Unrecognized data type %d", pdatatype);
          continue;
        }
      } else if (srcfd == ptym) {
        int rd = read(ptym, rbuff, BUFF_SIZE);
        if (rd <= 0) {
          if (rd < 0)
            errmsg = "mPTY read error";
          stop = true;
          break;
        }

        if (!proto_write(commfd, rd, DT_REGULAR, rbuff)) {
          errmsg = "Socket write error";
          break;
        }
      } else {
        errno = EINVAL;
        errmsg = "unknown src fd";
      }
    }
  }

  if (errmsg)
    warn("%s", errmsg);

  // don't forget to let client know if we're stopping
  proto_write(commfd, 0, DT_CLOSE, NULL);

  close(commfd);
  close(ptym);
  select_destroy(rsinst);

  exit(errmsg ? 1 : 0);
}

static void set_winsize(int fd, const struct winch_data *data) {
  struct winsize ws = {.ws_row = data->rows, .ws_col = data->cols};
  if (ioctl(fd, TIOCSWINSZ, &ws) < 0)
    warn("Set window size error");
}
