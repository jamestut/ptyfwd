#pragma once

#include "common.h"
#include <stdbool.h>

#define MAX_FD 2

struct fd_list {
  UINT count;
  int fds[MAX_FD];
};

struct fd_list create_tcp_server(const char *host, const char *port);

int create_tcp_client(const char *host, const char *port);

struct fd_list create_uds_server(const char *path);

int create_uds_client(const char *path);

#ifdef __linux__

struct fd_list create_vsock_server(const char *cid, const char *port);

int create_vsock_client(const char *cid, const char *port);

#endif

int create_vsock_mult_client(const char *path, const char *cid, const char *port);
