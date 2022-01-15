#pragma once

#include <stdint.h>
#include <unistd.h>

#define INVALID_SESSION_ID 0ULL

struct session {
  uint64_t id;
  pid_t childpid;
  int fdpair[2];
};

uint64_t new_session(struct session **newsess);

void delete_session(uint64_t sessid);

struct session *get_session(uint64_t sessid);
