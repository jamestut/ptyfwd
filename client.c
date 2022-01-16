#include "protocol.h"
#include "utils.h"
#include "global.h"
#include "serverclient.h"
#include "session.h"
#include <err.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <openssl/sha.h>

static char rbuff[BUFF_SIZE];

static bool set_tty_raw(bool set);

static void install_signal_handlers();

static void sighandler(int sig);

static void send_window_size();

static bool negotiate();

static void query_sessid();

static int client_loop();

static void handle_pollin_sock();

static void handle_pollin_stdin();

static bool client_write(uint16_t len, enum data_type type, void *buff);

static bool client_read(uint16_t *len, enum data_type *type, void *buff);

static bool client_rw(bool write, uint16_t *len, enum data_type *type, void *buff);

// signal handler flags
static struct {
  bool winch;
  bool sighalt;
} operparams = {0};

static struct {
  struct client_conn_options clopt;
  int fd;
  uint64_t sessid;
  const char *errmsg;
  bool stop_loop;
} client;

int start_client(struct client_conn_options clopt) {
  client.sessid = INVALID_SESSION_ID;
  client.fd = create_client(&clopt);
  if (client.fd < 0) {
    err(1, "Error connecting to server");
  }
  // to support reconnection
  client.clopt = clopt;

  // nonblocking for stdio (we don't use stderr at the moment)
  for (int i = 0; i <= 1; ++i) {
    set_fd_flags(i, true, O_NONBLOCK);
  }

  if (!negotiate()) {
    warnx("Server negotiation failed.");
    return 1;
  }
  query_sessid();

  install_signal_handlers();

  if (!set_tty_raw(true))
    err(1, "Error setting terminal to raw mode");

  // send current window size (if exists)
  send_window_size();

  return client_loop();
}

static bool set_tty_raw(bool set) {
  int err;
  static bool is_raw = false;
  struct termios tgt_termios;
  static struct termios curr_termios;

  // target STDIN
  int fd = 0;

  if (set == is_raw)
    return true;

  if (is_raw && !set) {
    // caller request reset (revert raw mode)
    tcsetattr(fd, TCSAFLUSH, &curr_termios);
    is_raw = false;
    return true;
  }

  // from this point on, caller request term set to raw mode

  if (tcgetattr(fd, &curr_termios) < 0)
    return (-1);
  // keep a copy of curr_termios in case we want to restore later
  tgt_termios = curr_termios;

  // local flags
  // Echo off, canonical mode off, extended input processing off, signal chars off.
  tcflag_t c_lflag_off = (ECHO | ICANON | IEXTEN | ISIG);
  tgt_termios.c_lflag &= ~c_lflag_off;

  // input flags
  // No SIGINT on BREAK, CR-to-NL off, input parity check off,
  // don't strip 8th bit on input, output flow control off
  tcflag_t c_iflag_off = (BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  tgt_termios.c_iflag &= ~c_iflag_off;

  // control flags
  // Clear size bits, parity checking off.
  tcflag_t c_cflag_off = (CSIZE | PARENB);
  tgt_termios.c_cflag &= ~c_cflag_off;
  // Set 8 bits/char.
  tgt_termios.c_cflag |= CS8;

  // Output processing off.
  tcflag_t c_oflag_off = OPOST;
  tgt_termios.c_oflag &= ~c_oflag_off;

  // c_cc is special char settings
  // 1 byte at a time
  tgt_termios.c_cc[VMIN] = 1;
  // no timer
  tgt_termios.c_cc[VTIME] = 0;

  if (tcsetattr(fd, TCSAFLUSH, &tgt_termios) < 0)
    return false;

  // Verify that the changes stuck.  tcsetattr can return 0 on partial success.
  if (tcgetattr(fd, &tgt_termios) < 0) {
    err = errno;
    tcsetattr(fd, TCSAFLUSH, &curr_termios);
    errno = err;
    return false;
  }
  if ((tgt_termios.c_lflag & c_lflag_off) || (tgt_termios.c_iflag & c_iflag_off) ||
      ((tgt_termios.c_cflag & (CS8 | c_cflag_off)) != CS8) || (tgt_termios.c_oflag & c_oflag_off) ||
      tgt_termios.c_cc[VMIN] != 1 || tgt_termios.c_cc[VTIME] != 0) {
    /*
     * Only some of the changes were made.  Restore the
     * original settings.
     */
    tcsetattr(fd, TCSAFLUSH, &curr_termios);
    errno = EINVAL;
    return false;
  }

  is_raw = true;
  return true;
}

static void install_signal_handlers() {
  struct sigaction act = {0};
  sigfillset(&act.sa_mask);
  act.sa_handler = sighandler;

  int sig_to_handle[] = {SIGINT, SIGTERM, SIGWINCH, SIGHUP};
  for (UINT i = 0; i < sizeof(sig_to_handle) / sizeof(int); ++i) {
    if (sigaction(sig_to_handle[i], &act, NULL) < 0) {
      err(1, "Error installing handler for signal %d", sig_to_handle[i]);
    }
  }
}

static void sighandler(int sig) {
  switch (sig) {
  case SIGINT:
  case SIGTERM:
  case SIGHUP:
    operparams.sighalt = true;
    break;
  case SIGWINCH:
    operparams.winch = true;
    break;
  }
}

static void send_window_size() {
  struct winsize winsz;
  if (ioctl(0, TIOCGWINSZ, &winsz) >= 0) {
    struct winch_data wd = {.rows = winsz.ws_row, .cols = winsz.ws_col};
    client_write(sizeof(wd), DT_WINCH, &wd);
  }
}

static bool negotiate() {
  int fd = client.fd;
  uint16_t recv_len;
  enum data_type recv_type;

  if (!proto_read(fd, &recv_len, &recv_type, rbuff)) {
    warn("Error receiving server preamble");
    return false;
  }
  if (recv_len != sizeof(preamble) && recv_type != DT_PREAMBLE) {
    warnx("Got unknown preamble from server.");
    return false;
  }

  if (memcmp(rbuff, preamble, sizeof(preamble))) {
    warnx("Invalid server version!");
    return false;
  }

  // OK, send preamble back
  proto_write(fd, sizeof(preamble), DT_PREAMBLE, preamble);

  // authentication phase
  if (!proto_read(fd, &recv_len, &recv_type, rbuff)) {
    warn("Error receiving authentication request.");
    return false;
  }

  if (recv_type == DT_NONE) {
    // server does not require authentication
    if (cookie.size) {
      warnx("Warning: server does not require authentication.");
    }
    return true;
  } else if (recv_type == DT_AUTH) {
    if (!cookie.size) {
      warnx("Server requires authentication. Please supply cookie file.");
      return false;
    }

    if (recv_len != NONCE_SIZE) {
      warnx("Invalid nonce size.");
      return false;
    }

    // generate answer
    uint8_t answer[ANSWER_SIZE];
    SHA_CTX shactx;
    int sharesult = 1;
    sharesult &= SHA1_Init(&shactx);
    sharesult &= SHA1_Update(&shactx, rbuff, NONCE_SIZE);
    sharesult &= SHA1_Update(&shactx, cookie.data, cookie.size);
    sharesult &= SHA1_Final(answer, &shactx);

    proto_write(fd, ANSWER_SIZE, DT_AUTH, answer);

    if (!proto_read(fd, &recv_len, &recv_type, rbuff)) {
      warnx("Error reading authentication result.");
      return false;
    }
    switch (recv_type) {
    case DT_CLOSE:
      warnx("Access denied!");
      return false;
    case DT_NONE:
      // access granted!
      return true;
    default:
      warnx("Invalid server response.");
      return false;
    }
  } else {
    warnx("Server sent unknown response.");
    return false;
  }
}

static void query_sessid() {
  uint16_t recv_len;
  enum data_type recv_type;

  // tell the server that this is a brand new session
  proto_write(client.fd, 0, DT_SESSID, NULL);

  // retrieve our persistent session ID if any
  // we'll use this for reconnection in case it is dropped
  if (!proto_read(client.fd, &recv_len, &recv_type, rbuff)) {
    warn("Error receiving persistent session ID");
    return;
  }

  if (recv_type != DT_SESSID) {
    warnx("Received unexpected type while retreiving session ID.");
  }
  if (recv_len) {
    if (recv_len != SESSID_SIZE) {
      warnx("Invalid session ID size. Persistent session won't be enabled.");
    }
    memcpy(&client.sessid, rbuff, SESSID_SIZE);
  }
}

static int client_loop() {
  struct pollfd pfds[2];
  pfds[0].fd = client.fd;
  pfds[1].fd = 0; // stdin
  pfds[0].events = pfds[1].events = POLLIN;

  while (!(client.errmsg || client.stop_loop)) {
    if (operparams.sighalt) {
      warnx("Requested graceful stop");
      client.stop_loop = true;
      break;
    }
    if (operparams.winch) {
      operparams.winch = false;
      send_window_size();
    }

    if (poll(pfds, 2, -1) < 0) {
      if (errno == EINTR)
        continue;
      client.errmsg = "Wait error";
    }

    for (int i = 0; i < 2; ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP)))
        continue;
      int srcfd = pfds[i].fd;
      if (srcfd == client.fd) {
        handle_pollin_sock();
      } else if (srcfd == 0) {
        handle_pollin_stdin();
      } else {
        // must be a bug
        errno = EINVAL;
        client.errmsg = "unknown source FD.";
      }
    }
  }

  if (client.errmsg)
    warn("%s", client.errmsg);

  // don't forget to let server know if we're stopping
  proto_write(client.fd, 0, DT_CLOSE, NULL);

  // fd = comm socket
  close(client.fd);
  set_tty_raw(false);
  return client.errmsg ? 1 : 0;
}

static void handle_pollin_sock() {
  uint16_t rdlen;
  enum data_type pdatatype;
  if (!client_read(&rdlen, &pdatatype, rbuff)) {
    return;
  }

  switch (pdatatype) {
  case DT_REGULAR:
    // forward to stdout
    if (!write_all(1, rbuff, rdlen)) {
      client.errmsg = "stdout write error";
    }
    break;
  case DT_CLOSE:
    client.stop_loop = true;
    break;
  case DT_NONE:
    break;
  default:
    warnx("Got unrecognized data type from server: %d", pdatatype);
    break;
  }
}

static void handle_pollin_stdin() {
  int rd = read(0, rbuff, BUFF_SIZE);
  if (rd <= 0) {
    if (rd < 0)
      client.errmsg = "stdin read error";
    client.stop_loop = true;
  }
  client_write(rd, DT_REGULAR, rbuff);
}

static bool client_write(uint16_t len, enum data_type type, void *buff) {
  return client_rw(true, &len, &type, buff);
}

static bool client_read(uint16_t *len, enum data_type *type, void *buff) {
  return client_rw(false, len, type, buff);
}

static bool client_rw(bool write, uint16_t *len, enum data_type *type, void *buff) {
  bool status = false;
  do {
    if (client.fd >= 0) {
      if (write) {
        status = proto_write(client.fd, *len, *type, buff);
      } else {
        status = proto_read(client.fd, len, type, buff);
      }
    }

    if (!status) {
      if (client.sessid != INVALID_SESSION_ID) {
        warnx("Connection to server failed. Retrying ...");
        close(client.fd);
        client.fd = create_client(&client.clopt);
        if (client.fd < 0) {
          // wait a while before we retry to avoid spinning
          sleep(1);
        }
      } else {
        client.errmsg = "Connection to server failed and there is no persistent session ID.";
        return false;
      }
    }
  } while (!status);

  // OK
  assert(status);
  return true;
}
