#include "common.h"
#include "global.h"
#include "protocol.h"
#include "socks.h"
#include "utils.h"
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <openssl/sha.h>

#define LISTEN_BACKLOG 8

static void server_worker_loop(int commfd, char *launchreq);

static void set_winsize(int fd, const struct winch_data *data);

static char rbuff[BUFF_SIZE];

static bool negotiate(int fd);

static bool authenticate(int fd);

int start_server(int svrfd, const char *launchreq) {
  if (listen(svrfd, LISTEN_BACKLOG) < 0) {
    warn("Listen error");
    return 1;
  }

  for (;;) {
    int commfd = accept(svrfd, NULL, NULL);
    if (commfd < 0) {
      if (errno != EINTR)
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
      if (!negotiate(commfd)) {
        errx(1, "Client negotiation failed.");
      }
      warnx("New client successfully connected.");
      server_worker_loop(commfd, launchreq_c);
    }
  }

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

  pid_t childpid = fork();
  if (childpid < 0)
    err(1, "Error spawning process");
  if (!childpid) {
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

  close(ptys);

  // parent
  // this loop basically:
  // - read remote, write to PTM
  // - read PTM, write to remote

  struct pollfd pfds[2];
  pfds[0].fd = commfd;
  pfds[1].fd = ptym;
  pfds[0].events = pfds[1].events = POLLIN;
  const char *errmsg = NULL;
  bool stop = false;
  while (!(errmsg || stop)) {
    if (poll(pfds, 2, -1) < 0) {
      if (errno == EINTR)
        continue;
    }

    for (int i = 0; i < 2; ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP)))
        continue;
      int srcfd = pfds[i].fd;
      if (srcfd == commfd) {
        uint16_t rdlen;
        enum data_type pdatatype;
        if (!proto_read(commfd, &rdlen, &pdatatype, rbuff)) {
          errmsg = "Socket read error";
          break;
        }

        switch (pdatatype) {
        case DT_WINCH:
          set_winsize(ptym, (struct winch_data *)rbuff);
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

  warnx("Client disconnected.");
  exit(errmsg ? 1 : 0);
}

static void set_winsize(int fd, const struct winch_data *data) {
  struct winsize ws = {.ws_row = data->rows, .ws_col = data->cols};
  if (ioctl(fd, TIOCSWINSZ, &ws) < 0)
    warn("Set window size error");
}

static bool negotiate(int fd) {
  // preamble: server send a 8 byte preamble data
  // client should either disconnect the connection if it doesn't agree,
  // or reply with the same preamble string.
  enum data_type recv_type;
  uint16_t recv_len;

  proto_write(fd, sizeof(preamble), DT_PREAMBLE, preamble);

  if (!proto_read(fd, &recv_len, &recv_type, rbuff)) {
    warn("%s", "Error receiving preamble message back");
    return false;
  }
  if (recv_len != sizeof(preamble) && recv_type != DT_PREAMBLE) {
    warn("Got unknown response from client");
    return false;
  }

  if (memcmp(rbuff, preamble, sizeof(preamble))) {
    warnx("Reply back preamble mismatch!");
    return false;
  }

  if (cookie.size) {
    return authenticate(fd);
  } else {
    return proto_write(fd, 0, DT_NONE, NULL);
  }
}

static bool authenticate(int fd) {
  uint8_t nonce[NONCE_SIZE];
  arc4random_buf(nonce, sizeof(nonce));

  // we generate the correct answer ourselves first
  // answer is SHA1(nonce + cookie)
  uint8_t refanswer[ANSWER_SIZE];
  SHA_CTX shactx;
  int sharesult = 1;
  sharesult &= SHA1_Init(&shactx);
  sharesult &= SHA1_Update(&shactx, nonce, NONCE_SIZE);
  sharesult &= SHA1_Update(&shactx, cookie.data, cookie.size);
  sharesult &= SHA1_Final(refanswer, &shactx);
  if (!sharesult) {
    errx(1, "BUG! Failed to compute reference answer!");
  }

  // send nonce only
  proto_write(fd, NONCE_SIZE, DT_AUTH, nonce);
  // expect SHA1 answer from client
  uint16_t recv_len;
  enum data_type recv_type;
  if (!proto_read(fd, &recv_len, &recv_type, rbuff)) {
    warn("Error reading authentication response.");
    return false;
  }
  if (recv_len != ANSWER_SIZE && recv_type != DT_AUTH) {
    warn("Got unknown authentication from client");
    return false;
  }

  if (memcmp(refanswer, rbuff, ANSWER_SIZE)) {
    warnx("Client authentication request rejected!");
    // send CLOSE message to let client know we reject this request
    proto_write(fd, 0, DT_CLOSE, NULL);
    return false;
  }

  // send a NONE to let client know auth was successful
  proto_write(fd, 0, DT_NONE, NULL);
  return true;
}