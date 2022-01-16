#include "common.h"
#include "global.h"
#include "protocol.h"
#include "socks.h"
#include "utils.h"
#include "serverclient.h"
#include "session.h"
#include "simplemap.h"
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

static void sigchld_handler(int sig);

static void server_worker_init(char *launchreq);

static void server_worker_shell_exec(char *launchreq);

static void server_worker_main_loop();

static void handle_pollin_commfd();

static void handle_pollin_ptym();

static void handle_pollin_stfd();

static void set_commfd_error();

static void redirect_persistent_session(int commfd, struct session *sess);

static bool forward_pty_to_client();

static void set_winsize(int fd, const struct winch_data *data);

static char rbuff[BUFF_SIZE];

struct {
  uint8_t *data;
  size_t size;
  size_t capacity;
} ptym_buff;

struct {
  int commfd;
  int pendcommfd;
  int stfd;
  int ptym;
  int ptys;
  char pts_name[32];
  pid_t shellpid;
  bool stop_loop;
  const char *errmsg;
} worker = {};

static bool negotiate(int fd);

static bool authenticate(int fd);

void *map_pid;

int start_server(int svrfd, struct server_options *opt) {
  if (listen(svrfd, LISTEN_BACKLOG) < 0) {
    warn("Listen error");
    return 1;
  }

  ptym_buff.capacity = opt->ptybufsz;
  ptym_buff.data = malloc(opt->ptybufsz);
  if (!ptym_buff.data) {
    err(1, "Error allocating PTYm buffer");
  }

  if (opt->sessionsave) {
    // install signal handler to detect if child server exits.
    // this way we know on when to remove session data.
    struct sigaction act = {0};
    sigfillset(&act.sa_mask);
    act.sa_handler = sigchld_handler;
    if (sigaction(SIGCHLD, &act, NULL) < 0) {
      err(1, "Error installing SIGCHLD handler");
    }

    // pid mapping
    map_pid = simplemap_init();
    if (!map_pid) {
      err(1, "Error initialising PID session map.");
    }
  }

  for (;;) {
    int commfd = accept(svrfd, NULL, NULL);
    if (commfd < 0) {
      if (errno != EINTR)
        warn("Error accepting connection");
      continue;
    }
    set_fd_flags(commfd, true, O_NONBLOCK);

    if (!negotiate(commfd)) {
      warnx("Client negotiation failed.");
      goto on_error;
    }

    // check if client request persistent session
    uint16_t recv_len;
    enum data_type recv_type;
    if (!proto_read(commfd, &recv_len, &recv_type, rbuff)) {
      warn("Error receiving client persistency negotiation");
      goto on_error;
    }

    if (recv_type == DT_SESSID) {
      if (recv_len == SESSID_SIZE) {
        if (!opt->sessionsave) {
          warnx("Client requested session ID, but server-side feature is not enabled.");
          proto_write(commfd, 0, DT_CLOSE, NULL);
          goto on_error;
        }
        uint64_t sessid = *((uint64_t *)rbuff);
        struct session *sess = get_session(sessid);
        if (!sess) {
          warnx("Requested session ID %llx not found.", sessid);
          proto_write(commfd, 0, DT_CLOSE, NULL);
          goto on_error;
        }
        redirect_persistent_session(commfd, sess);
        continue;
      } else if (recv_len) {
        // unknown sessid size
        warnx("Invalid session ID request.");
        goto on_error;
      }
    } else {
      warnx("Client sent an invalid request.");
      goto on_error;
    }

    // normal "new connection" path
    struct session *sess = NULL;
    if (opt->sessionsave) {
      if (new_session(&sess) == INVALID_SESSION_ID) {
        goto on_error;
      }
      // create unix socket pair for transferring reconnection FD
      if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sess->fdpair) < 0) {
        warn("Error creating FD transfer socket pair");
        goto on_error;
      }
      for (int i = 0; i < 2; ++i) {
        if (fcntl(sess->fdpair[i], F_GETFD, FD_CLOEXEC) < 0) {
          warn("Error setting CLOEXEC on FD transfer socket pair");
          goto on_error;
        }
      }
    } else {
      // indicates that there is nothing for client to save
      proto_write(commfd, 0, DT_SESSID, NULL);
    }

    pid_t pid = fork();
    if (pid < 0) {
      warn("Fork failed");
      goto on_error;
    } else if (pid) {
      // parent
      // don't need commfd and the other end of socket pair
      close(commfd);
      if (sess) {
        close(sess->fdpair[1]);
        sess->fdpair[1] = -1;
        sess->childpid = pid;
        if (!simplemap_add(map_pid, pid, sess)) {
          warnx("Error mapping PID. Persistent session will not be available.");
          delete_session(sess->id);
        }
      }
    } else {
      // child will do all the job. no return.
      // uninstall SIGCHLD handler
      struct sigaction act = {0};
      sigfillset(&act.sa_mask);
      act.sa_handler = SIG_IGN;
      sigaction(SIGCHLD, &act, NULL);
      // let client know their persistent session ID (if any)
      int stfd = -1;
      if (sess) {
        // don't need the other end of socket pair
        close(sess->fdpair[0]);
        sess->fdpair[0] = -1;
        stfd = sess->fdpair[1];
        proto_write(commfd, SESSID_SIZE, DT_SESSID, &sess->id);
      }
      char *launchreq_c = alloca(strlen(opt->launchreq) + 1);
      strcpy(launchreq_c, opt->launchreq);
      warnx("New client successfully connected.");

      worker.commfd = commfd;
      worker.stfd = stfd;
      worker.pendcommfd = -1;
      server_worker_init(launchreq_c);
    }

    continue;
on_error:
    close(commfd);
  }

  return 0;
}

static void sigchld_handler(int sig) {
  if (sig != SIGCHLD) {
    return;
  }
  int status;
  pid_t stopped = waitpid(-1, &status, WNOHANG);
  if (stopped < 0) {
    return;
  }

  struct session *sess;
  if (!simplemap_get(map_pid, stopped, (void **)&sess)) {
    warnx("Received SIGCHLD on unregistered PID %d!", stopped);
    return;
  }

  delete_session(sess->id);
  simplemap_del(map_pid, stopped);
}

static void server_worker_init(char *launchreq) {
  // controlling (m) pty
  worker.ptym = open("/dev/ptmx", O_RDWR);
  if (worker.ptym < 0)
    err(1, "Error opening ptmx");
  set_fd_flags(worker.ptym, true, O_NONBLOCK);

  if (grantpt(worker.ptym) < 0)
    err(1, "grantpt error");
  if (unlockpt(worker.ptym) < 0)
    err(1, "unlockpt error");

  // controlled (s) pty
  char *ptsnameres = ptsname(worker.ptym);
  if (!ptsnameres)
    err(1, "Error getting name for sPTY");
  strcpy(worker.pts_name, ptsnameres);

  worker.ptys = open(worker.pts_name, O_RDWR);
  if (worker.ptys < 0)
    err(1, "Error opening sPTY");

  worker.shellpid = fork();
  if (worker.shellpid < 0)
    err(1, "Error spawning process");
  if (!worker.shellpid) {
    // child.
    server_worker_shell_exec(launchreq);
  }

  // sPTY is shell-side only
  close(worker.ptys);
  server_worker_main_loop();
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

static void server_worker_shell_exec(char *launchreq) {
  // from this point we're not in server process anymore
  // our goal is to launch the requested shell and connect it
  // to our PTY.

  // mPTY is server-side only
  close(worker.ptym);
  // child also no need to deal with commsock
  close(worker.commfd);

  // this must be done in this exact order to make this process
  // as both session leader and controlling terminal
  if (setsid() < 0)
    err(1, "Error setting session leader");
  if (ioctl(worker.ptys, TIOCSCTTY, 0) < 0)
    err(1, "Error setting controlling terminal");
  if (tcsetpgrp(worker.ptys, getpid()) < 0)
    err(1, "Error setting foreground process group");

  // make sPTY our stdio!
  for (int i = 0; i <= 2; ++i) {
    if (dup2(worker.ptys, i) != i)
      err(1, "Error dup2 sPTY to stdio");
  }

  char *args[2] = {launchreq, NULL};
  if (execvp(launchreq, args) < 0)
    err(1, "exec error");
}

static void server_worker_main_loop() {
  // this loop basically:
  // - read remote, write to PTM
  // - read PTM, write to remote
  struct pollfd pfds[3];
  uint8_t pollsz = 0;

  while (!(worker.errmsg || worker.stop_loop)) {
    // clear FDs to poll
    pollsz = 0;
    // we will pause reading (and thus polling) from ptym
    // if ptym's read buffer is full.
    if (ptym_buff.size < ptym_buff.capacity) {
      populate_poll(pfds, &pollsz, worker.ptym, POLLIN);
    }
    populate_poll(pfds, &pollsz, worker.commfd, POLLIN);
    populate_poll(pfds, &pollsz, worker.stfd, POLLIN);

    if (poll(pfds, pollsz, -1) < 0) {
      if (errno == EINTR)
        continue;
    }

    for (int i = 0; i < pollsz; ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP)))
        continue;
      int srcfd = pfds[i].fd;
      if (srcfd == worker.commfd) {
        handle_pollin_commfd();
      } else if (srcfd == worker.ptym) {
        handle_pollin_ptym();
      } else if (srcfd == worker.stfd) {
        handle_pollin_stfd();
      } else {
        // must be a bug
        errno = EINVAL;
        worker.errmsg = "unknown source FD.";
      }
    }
  }

  if (worker.errmsg) {
    warn("%s", worker.errmsg);
  }

  // don't forget to let client know if we're stopping
  proto_write(worker.commfd, 0, DT_CLOSE, NULL);

  close(worker.commfd);
  close(worker.ptym);

  warnx("Client disconnected.");
  exit(worker.errmsg ? 1 : 0);
}

static void handle_pollin_commfd() {
  uint16_t rdlen;
  enum data_type pdatatype;
  if (!proto_read(worker.commfd, &rdlen, &pdatatype, rbuff)) {
    set_commfd_error();
    return;
  }

  switch (pdatatype) {
  case DT_WINCH:
    set_winsize(worker.ptym, (struct winch_data *)rbuff);
    break;
  case DT_REGULAR:
    // send whatever client sent to shell
    if (!write_all(worker.ptym, rbuff, rdlen)) {
      worker.errmsg = "mPTY write error";
    }
    break;
  case DT_CLOSE:
    worker.stop_loop = true;
    break;
  case DT_NONE:
    break;
  default:
    warnx("Unrecognized data type %d", pdatatype);
  }
}

static void handle_pollin_ptym() {
  int rd = read(worker.ptym, ptym_buff.data + ptym_buff.size,
    ptym_buff.capacity - ptym_buff.size);
  if (rd <= 0) {
    if (rd < 0) {
      worker.errmsg = "mPTY read error";
    }
    worker.stop_loop = true;
  }
  ptym_buff.size += rd;

  // send shell's output to client
  if (!forward_pty_to_client()) {
    set_commfd_error();
  }
}

static void handle_pollin_stfd() {
  assert(worker.stfd >= 0);

  // the duplicated file descriptor is in control message
  uint8_t cmsghdrdata[CMSG_SPACE(sizeof(int))];
  struct cmsghdr *cmsg = (struct cmsghdr *)cmsghdrdata;

  // recvmsg requires something to be sent/received on main channel
  char dummy;
  struct iovec iov;
  iov.iov_base = &dummy;
  iov.iov_len = sizeof(dummy);

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_control = cmsg;
  msg.msg_controllen = sizeof(cmsghdrdata);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  if (recvmsg(worker.stfd, &msg, 0) < 0) {
    // fail silently
    return;
  }

  worker.pendcommfd = *((int *)CMSG_DATA(cmsg));
  if (worker.commfd < 0) {
    worker.commfd = worker.pendcommfd;
    // try resending
    if (!forward_pty_to_client()) {
      // then this new connection is not valid either :(
      worker.pendcommfd = worker.commfd = -1;
    }
  }
}

static void set_commfd_error() {
  if (worker.stfd < 0) {
    worker.errmsg = "Client connection error";
    return;
  }

  // mark current commfd as errorneous and set it to -1
  if (worker.pendcommfd >= 0) {
    // if we got a valid pendcommfd, try resend
    worker.commfd = worker.pendcommfd;
    worker.pendcommfd = -1;
    if (!forward_pty_to_client()) {
      // if error happens, the "else" part of the parent "if"
      // will be executed instead, so no infinite recursion.
      set_commfd_error();
    }
  } else {
    worker.commfd = -1;
  }
}

static void redirect_persistent_session(int commfd, struct session *sess) {
  // fd needs sizeof(int)
  uint8_t cmsghdrdata[CMSG_SPACE(sizeof(int))];
  struct cmsghdr *cmsg = (struct cmsghdr *)cmsghdrdata;

  // sendmsg requires something to be sent/received on main channel
  char dummy = 'a';
  struct iovec iov;
  iov.iov_base = &dummy;
  iov.iov_len = sizeof(dummy);

  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  // transfer ("dup2") commfd to worker
  *((int *)CMSG_DATA(cmsg)) = commfd;

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_control = cmsg;
  msg.msg_controllen = sizeof(cmsghdrdata);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  if (sendmsg(sess->fdpair[0], &msg, 0) < 0) {
    warn("Error transferring socket to worker");
  }
  close(commfd);
}

static bool forward_pty_to_client() {
  assert (worker.commfd >= 0);
  size_t offset = 0;
  while (offset < ptym_buff.size) {
    size_t to_send = ptym_buff.size - offset;
    if (to_send > BUFF_SIZE) {
      to_send = BUFF_SIZE;
    }
    if (!proto_write(worker.commfd, to_send, DT_REGULAR, ptym_buff.data + offset)) {
      // partial error = update ptym_buff to reflect what we've sent so far
      memmove(ptym_buff.data, ptym_buff.data + offset, ptym_buff.size - offset);
      ptym_buff.size -= offset;
      return false;
    }
    offset += to_send;
  }
  ptym_buff.size = 0;
  return true;
}
