#include "common.h"
#include "socks.h"
#include <err.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum conn_mode { CM_NONE, CM_TCP, CM_UDS, CM_VSOCK, CM_VSOCKMULT };

int start_server(struct fd_list *fds, const char *launchreq);

int start_client(int fd);

int main(int argc, char **argv) {
  bool servermode = false;
  enum conn_mode connmode = CM_NONE;
  char *targetaddr = NULL;
  char *cid = NULL;
  char *port = NULL;
  char *launchreq = NULL;

  char c;
  while ((c = getopt(argc, argv, "s:ch:u:v:p:")) != EOF) {
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
    default:
      goto usage;
    }
  }

  if (servermode) {
    struct fd_list fds;
    switch (connmode) {
    case CM_TCP:
      fds = create_tcp_server(targetaddr, port);
      break;
    case CM_UDS:
      fds = create_uds_server(targetaddr);
      break;
#ifdef __linux__
    case CM_VSOCK:
      fds = create_vsock_server(cid, port);
      break;
#endif
    default:
      goto usage;
    }
    if (!fds.count)
      // something happened. just bail out. those create_* function should warn user appropriately
      return 1;
    return start_server(&fds, launchreq);
  } else {
    int commfd;
    switch (connmode) {
    case CM_TCP:
      commfd = create_tcp_client(targetaddr, port);
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
  puts("  Specify TCP mode as well as the host name to connect/listen.");
  puts("  Requires '-p' to be present.");
  puts(" -u <path>");
  puts("  Specify Unix socket mode as well as the socket path to connect/listen.");
  puts(" -v <cid>");
  puts("  Specify the CID of the VSOCK. Requires '-p'.");
  puts("  Use together with '-u' and '-c' to connect to a VSOCK multiplexer UDS.");
  puts("  Plain VSOCK without multiplexer is supported only on Linux.");
  puts(" -p <port>");
  puts("  Specify port number.");
  return 0;
}