#include "socks.h"
#include "utils.h"
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/vm_sockets.h>
#endif

#define ERROR(...)                                                                                                     \
  {                                                                                                                    \
    warn(__VA_ARGS__);                                                                                                 \
    return (struct fd_list){0};                                                                                        \
  }

struct fd_list create_tcp_server(const char *host, const char *port) {
  struct fd_list ret = {0};
  int st;

  struct addrinfo addrhints;
  struct addrinfo *addrres;

  memset(&addrhints, 0, sizeof(addrhints));
  addrhints.ai_family = PF_UNSPEC;
  addrhints.ai_socktype = SOCK_STREAM;

  st = getaddrinfo(host, port, &addrhints, &addrres);
  if (st < 0)
    ERROR("getaddrinfo error");

  for (struct addrinfo *res = addrres; res && ret.count < MAX_FD; res = res->ai_next) {
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) {
      warn("Error creating socket");
      continue;
    }

    int val = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    if (set_fd_flags(s, true, O_NONBLOCK) < 0) {
      warn("Error setting socket nonblock");
      goto errcont;
    }

    if (bind(s, res->ai_addr, res->ai_addrlen) < 0) {
      warn("Error binding socket");
      goto errcont;
    }

    ret.fds[ret.count++] = s;

    continue;
  errcont:
    close(s);
    continue;
  }

  return ret;
}

int create_tcp_client(const char *host, const char *port) {
  int st;

  struct addrinfo addrhints;
  struct addrinfo *addrres;

  memset(&addrhints, 0, sizeof(addrhints));
  addrhints.ai_family = PF_UNSPEC;
  addrhints.ai_socktype = SOCK_STREAM;

  st = getaddrinfo(host, port, &addrhints, &addrres);
  if (st < 0)
    return -1;

  int s = -1;
  for (struct addrinfo *res = addrres; res; res = res->ai_next) {
    s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0)
      continue;

    // blocking connect
    set_fd_flags(s, false, O_NONBLOCK);
    if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
      close(s);
      s = -1;
      continue;
    }

    // nonblocking comm socket
    set_fd_flags(s, true, O_NONBLOCK);

    // connect OK
    break;
  }
  return s;
}

struct fd_list create_uds_server(const char *path) {
  struct fd_list ret = {.count = 1};
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    ERROR("Error creating Unix socket");
  set_fd_flags(s, true, O_NONBLOCK);

  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path));

  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    ERROR("Error binding socket");

  ret.fds[0] = s;
  return ret;
}

int create_uds_client(const char *path) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    return -1;
  set_fd_flags(s, true, O_NONBLOCK);

  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path));

  if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(s);
    return -1;
  }

  return s;
}

#ifdef __linux

// struct fd_list create_vsock_server(const char *cid, const char *port) {
//   struct fd_list ret = {.count = 1};
//   int s = socket(AF_VSOCK, SOCK_STREAM, 0);
//   if (s < 0)
//     ERROR("Error creating VSOCK");

//   struct sockaddr_vm addr = {0};
//   addr.svm_family = AF_UNIX;
// }

#endif

int create_vsock_mult_client(const char *path, const char *s_cid, const char *s_port) {
  UINT cid, port;
  if (!sscanf("%u", s_cid, &cid)) {
    warnx("Invalid CID value");
    goto err_inval;
  }
  if (!sscanf("%u", s_port, &port)) {
    warnx("Invalid port value");
    goto err_inval;
  }

  int s = create_uds_client(path);
  if (s < 0)
    return -1;

  char preamble[8 + 1 + 8 + 1 + 1] = {0}; // "%08x.%08x\n" + \0 (cid, port)
  int preamblelen = sprintf(preamble, "%08x.%08x\n", cid, port);
  if (!write_all(s, preamble, preamblelen)) {
    close(s);
    return -1;
  }

  return s;

err_inval:
  errno = EINVAL;
  return -1;
}