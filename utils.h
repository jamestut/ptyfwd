#pragma once

#include "common.h"
#include "socks.h"
#include <stdbool.h>
#include <poll.h>
#include <stdint.h>
#include <stddef.h>

struct client_conn_options {
  enum conn_mode connmode;
  const char *targetaddr;
  const char *cid;
  const char *port;
};

int set_fd_flags(int fd, bool set, int flags);

// WARNING! this function is NOT thread safe!
bool write_all(int fd, const void *buff, UINT len);

bool read_all(int fd, const void *buff, UINT len);

void populate_poll(struct pollfd *pfds, uint8_t *idx, int fd, int events);

int create_client(struct client_conn_options *opt);

void random_fill(void *buff, size_t size);

void wait_debugger();
