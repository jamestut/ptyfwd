#pragma once

#include <stdbool.h>
#include <stddef.h>

struct server_options {
  const char *launchreq;
  bool sessionsave;
  size_t ptybufsz;
};

int start_server(int svrfd, struct server_options *opt);

int start_client(int fd);
