#include "protocol.h"
#include "select.h"
#include "utils.h"
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static char rbuff[BUFF_SIZE];

static bool set_tty_raw();

static void install_signal_handlers();

static void sighandler(int sig);

static void send_window_size(int commfd);

static struct {
  bool winch;
  bool sighalt;
} operparams = {0};

int start_client(int fd) {
  // nonblocking for stdio (we don't use stderr at the moment)
  for (int i = 0; i <= 1; ++i) {
    set_fd_flags(i, true, O_NONBLOCK);
  }

  install_signal_handlers();

  if (!set_tty_raw())
    err(1, "Error setting terminal to raw mode");

  // send current window size (if exists)
  send_window_size(fd);

  struct wait_list wl[2];
  wl[0].fd = fd;
  wl[1].fd = 0; // stdin
  wl[0].wm = wl[1].wm = WM_READ;
  void *sinst = select_init(wl, 2);
  if (!sinst)
    err(1, "select init error");

  const char *errmsg = NULL;
  bool stop = false;
  while (!(errmsg || stop)) {
    int readyfds[2];
    int readyfdcount = select_wait(sinst, readyfds);

    if (operparams.sighalt) {
      warnx("Requested graceful stop");
      stop = true;
      break;
    }
    if (operparams.winch) {
      operparams.winch = false;
      send_window_size(fd);
    }

    if (readyfdcount < 0) {
      if (errno == EINTR)
        continue;
      errmsg = "Wait error";
    }

    for (int i = 0; i < readyfdcount; ++i) {
      int srcfd = readyfds[i];
      if (srcfd == fd) {
        uint16_t rdlen;
        enum data_type pdatatype;
        if (!proto_read(fd, &rdlen, &pdatatype, rbuff)) {
          errmsg = "Socket read error";
          break;
        }

        switch (pdatatype) {
        case DT_REGULAR:
          if (!write_all(1, rbuff, rdlen))
            errmsg = "stdout write error";
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
      } else if (srcfd == 0) {
        int rd = read(0, rbuff, BUFF_SIZE);
        if (rd <= 0) {
          if (rd < 0)
            errmsg = "stdin read error";
          stop = true;
          break;
        }

        if (!proto_write(fd, rd, DT_REGULAR, rbuff)) {
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

  // don't forget to let server know if we're stopping
  proto_write(fd, 0, DT_CLOSE, NULL);

  // fd = comm socket
  close(fd);
  select_destroy(sinst);
  return errmsg ? 1 : 0;
}

static bool set_tty_raw() {
  int err;
  struct termios tgt_termios;
  struct termios curr_termios;

  // target STDIN
  int fd = 0;

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

  return true;
}

static void install_signal_handlers() {
  struct sigaction act = {0};
  sigfillset(&act.sa_mask);
  act.sa_handler = sighandler;

  int sig_to_handle[] = {SIGINT, SIGTERM, SIGWINCH, SIGHUP};
  for (int i = 0; i < sizeof(sig_to_handle) / sizeof(int); ++i) {
    if (sigaction(sig_to_handle[i], &act, NULL) < 0)
      err(1, "Error installing handler for signal %d", sig_to_handle[i]);
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

static void send_window_size(int commfd) {
  struct winsize winsz;
  if (ioctl(0, TIOCGWINSZ, &winsz) >= 0) {
    struct winch_data wd = {.rows = winsz.ws_row, .cols = winsz.ws_col};
    proto_write(commfd, sizeof(wd), DT_WINCH, &wd);
  }
}
