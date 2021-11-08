#include "common.h"
#include "select.h"
#include "socks.h"
#include "utils.h"
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LISTEN_BACKLOG 8
#define BUFF_SIZE 4096

void server_worker_loop(int commfd, char *launchreq);

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
        server_worker_loop(commfd, launchreq_c);
      }
    }
  }

  select_destroy(sinst);
  return 0;
}

void server_worker_loop(int commfd, char *launchreq) {
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
    err(1, "Error getting name for PTS");
  strcpy(pts_name, ptsnameres);
  int ptys = open(pts_name, O_RDWR);
  if (ptys < 0)
    err(1, "Error opening PTS");

  pid_t pid = fork();
  if (pid < 0)
    err(1, "Error spawning process");
  if (!pid) {
    // child.
    // we're done with m PTY
    close(ptym);
    // make s PTY our stdio!
    for (int i = 0; i <= 2; ++i) {
      if (dup2(ptys, i) != i)
        err(1, "Error dup2 PTS to stdio");
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
  for (;;) {
    int readyfdscount = select_wait(rsinst, readyfds);
    if (readyfdscount < 0) {
      if (errno == EINTR) {
        // TODO: handle signal?
        continue;
      }
      err(1, "Wait error");
    }
    for (int i = 0; i < readyfdscount; ++i) {
      int destfd = readyfds[i] == ptym ? commfd : ptym;
      int rd = read(readyfds[i], rbuff, BUFF_SIZE);
      if (rd < 0)
        err(1, "Read error");
      // write entire buffer no matter what
      if (!write_all(destfd, rbuff, rd))
        err(1, "Write error");
    }
  }
  select_destroy(rsinst);
}
