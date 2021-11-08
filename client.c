#include "select.h"
#include "utils.h"
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#define BUFF_SIZE 4096

static char rbuff[BUFF_SIZE];

bool set_tty_raw();

int start_client(int fd) {
  // nonblocking for stdio (we don't use stderr at the moment)
  for (int i = 0; i <= 1; ++i) {
    set_fd_flags(i, true, O_NONBLOCK);
  }

  // TODO: set stdio to raw mode!
  if (!set_tty_raw())
    err(1, "Error setting terminal to raw mode");

  struct wait_list wl[2];
  wl[0].fd = fd;
  wl[1].fd = 0; // stdin
  wl[0].wm = wl[1].wm = WM_READ;
  void *sinst = select_init(wl, 2);
  if (!sinst)
    err(1, "select init error");

  for (;;) {
    int readyfds[2];
    int readyfdcount = select_wait(sinst, readyfds);
    if (readyfdcount < 0) {
      if (errno == EINTR)
        // TODO: handle signal?
        continue;
      err(1, "Wait error");
    }
    for (int i = 0; i < readyfdcount; ++i) {
      int destfd = !readyfds[i] ? fd : 1;
      int rd = read(readyfds[i], rbuff, BUFF_SIZE);
      if (rd < 0)
        err(1, "Read error");
      // write entire buffer no matter what
      if (!write_all(destfd, rbuff, rd))
        err(1, "Write error");
    }
  }

  select_destroy(sinst);
}

bool set_tty_raw() {
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
