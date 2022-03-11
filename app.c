#include "common.h"
#include "global.h"
#include "socks.h"
#include <err.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

enum conn_mode { CM_NONE, CM_TCP, CM_TCP6, CM_UDS, CM_VSOCK, CM_VSOCKMULT };

int start_server(int svrfd, const char *launchreq);

int start_client(int fd);

static bool read_cookie(const char *cookiefile);

int main(int argc, char **argv) {
  bool servermode = false;
  enum conn_mode connmode = CM_NONE;
  char *targetaddr = NULL;
  char *cid = NULL;
  char *port = NULL;
  char *launchreq = NULL;
  char *cookiefile = NULL;

  char c;
  while ((c = getopt(argc, argv, "s:c:h:u:v:p:")) != EOF) {
    switch (c) {
    case 's':
      servermode = true;
      launchreq = optarg;
      break;
    case 'h': // implies TCP mode
      targetaddr = optarg;
      if (connmode != CM_NONE)
        goto usage;
      connmode = CM_TCP;
      break;
    case '6': // implies TCP on IPv6 mode
      targetaddr = optarg;
      if (connmode != CM_NONE)
        goto usage;
      connmode = CM_TCP6;
      break;
    case 'p':
      port = optarg;
      break;
    case 'u': // implies UDS mode (or VSOCK multiplexer mode)
      if (connmode == CM_VSOCK)
        connmode = CM_VSOCKMULT;
      else if (connmode == CM_NONE)
        connmode = CM_UDS;
      else
        goto usage;
      targetaddr = optarg;
      break;
    case 'v': // implies VSOCK / VSOCK multiplexer
      if (connmode == CM_UDS)
        connmode = CM_VSOCKMULT;
      else if (connmode == CM_NONE)
        connmode = CM_VSOCK;
      else
        goto usage;
      cid = optarg;
      break;
    case 'c':
      cookiefile = optarg;
      break;
    default:
      goto usage;
    }
  }

  if (cookiefile) {
    if (!read_cookie(cookiefile)) {
      return 1;
    }
  } else {
    if (servermode) {
      warnx("WARNING! Server is running without authentication!");
    }
  }

  if (servermode) {
    int svrfd;
    switch (connmode) {
    case CM_TCP:
    case CM_TCP6:
      svrfd = create_tcp_server(connmode == CM_TCP6, targetaddr, port);
      break;
    case CM_UDS:
      svrfd = create_uds_server(targetaddr);
      break;
#ifdef __linux__
    case CM_VSOCK:
      svrfd = create_vsock_server(cid, port);
      break;
#endif
    default:
      goto usage;
    }
    if (svrfd < 0)
      err(1, "Error creating socket server");
    return start_server(svrfd, launchreq);
  } else {
    int commfd;
    switch (connmode) {
    case CM_TCP:
    case CM_TCP6:
      commfd = create_tcp_client(connmode == CM_TCP6, targetaddr, port);
      break;
    case CM_UDS:
      commfd = create_uds_client(targetaddr);
      break;
#ifdef __linux__
    case CM_VSOCK:
      commfd = create_vsock_client(cid, port);
      break;
#endif
    case CM_VSOCKMULT:
      commfd = create_vsock_mult_client(targetaddr, cid, port);
      break;
    default:
      goto usage;
    }
    if (commfd < 0)
      err(1, "Error connecting to server");
    return start_client(commfd);
  }

usage:
  puts("Usage:");
  puts(" -s <app_to_run>");
  puts("  Specify server mode (listen and wait for connection).");
  puts("  Upon client connection, <app_to_run> will be opened.");
  puts("  If not specified, assumes client mode (connect to server).");
  puts(" -h <host>");
  puts("  Specify TCP IPv4 mode as well as the host name to connect/listen.");
  puts("  Requires '-p' to be present.");
  puts(" -6 <host>");
  puts("  Same as `-h`, but specify TCP on IPv6 mode instead of IPv4.");
  puts(" -u <path>");
  puts("  Specify Unix socket mode as well as the socket path to connect/listen.");
  puts(" -v <cid>");
  puts("  Specify the CID of the VSOCK. Requires '-p'.");
  puts("  Use together with '-u' to connect to a VSOCK multiplexer UDS.");
  puts("  Plain VSOCK without multiplexer is supported only on Linux.");
  puts(" -p <port>");
  puts("  Specify port number.");
  puts(" -c <cookiefile>");
  puts("  Enables authentication and specify a cookie file for authentication.");
  printf("  Cookie file must be within %u and %u bytes in size.\n", COOKIE_MIN_SIZE,
    COOKIE_MAX_SIZE);
  return 0;
}

static bool read_cookie(const char *cookiefile) {
  int fd = open(cookiefile, O_RDONLY);
  if (fd < 0) {
    warn("Cannot open cookie file");
    return false;
  }

  bool success = false;
  int rd;
  if ((rd = read(fd, cookie.data, COOKIE_MAX_SIZE)) < 0) {
    warn("Error reading cookie file");
    goto end;
  }

  if (rd < COOKIE_MIN_SIZE) {
    warnx("Cookie file too small.");
    goto end;
  }
  if (rd == COOKIE_MAX_SIZE) {
    // see if the supplied file is bigger than what we expected
    char dummy;
    if (read(fd, &dummy, sizeof(dummy)) > 0) {
      warnx("Cookie file too big.");
      goto end;
    }
  }

  cookie.size = rd;
  success = true;

end:
  close(fd);
  return success;
}
