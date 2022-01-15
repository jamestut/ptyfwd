#include "session.h"
#include <stdlib.h>
#include <err.h>
#include <unistd.h>
#include <string.h>

#define HASHMAP_SIZE 2
#define MAX_SESSIONS 32

typedef struct session_node session_node_t;

struct session_node {
  session_node_t *prev;
  session_node_t *next;
  struct session data;
};

struct {
  session_node_t *map[HASHMAP_SIZE];
  size_t count;
} sessdata = {};

session_node_t *add_to_hashmap(uint64_t sessid);

session_node_t *get_session_node(uint64_t sessid);

uint64_t new_session(struct session **newsess) {
  uint64_t sessid = INVALID_SESSION_ID;

  if (sessdata.count >= MAX_SESSIONS) {
    warnx("Maximum session count exceeded.");
    return INVALID_SESSION_ID;
  }

  for (;;) {
    arc4random_buf(&sessid, sizeof(sessid));
    if (sessid != INVALID_SESSION_ID && !get_session_node(sessid)) {
      session_node_t *newnode = add_to_hashmap(sessid);
      if (!newnode) {
        warn("Error allocating session data");
        return INVALID_SESSION_ID;
      }
      *newsess = &newnode->data;
      ++sessdata.count;
      return sessid;
    }
  }
}

session_node_t *add_to_hashmap(uint64_t sessid) {
  session_node_t *ret = calloc(sizeof(session_node_t), 1);
  if (!ret) {
    return NULL;
  }
  ret->data.id = sessid;
  memset(ret->data.fdpair, -1, sizeof(ret->data.fdpair));

  session_node_t *node = sessdata.map[sessid % HASHMAP_SIZE];
  ret->next = node;
  if (node) {
    node->prev = ret;
  }
  sessdata.map[sessid % HASHMAP_SIZE] = ret;

  return ret;
}

void delete_session(uint64_t sessid) {
  session_node_t *node = get_session_node(sessid);
  if (!node) {
    return;
  }

  // close unix socket
  for (int i = 0; i < 2; ++i) {
    if (node->data.fdpair[i] >= 0) {
      close(node->data.fdpair[i]);
      node->data.fdpair[i] = -1;
    }
  }

  // remove from hashmap
  if (node->prev) {
    node->prev->next = node->next;
  } else {
    // this is a head node
    sessdata.map[sessid % HASHMAP_SIZE] = node->next;
  }
  if (node->next) {
    node->next->prev = node->prev;
  }
  --sessdata.count;

  free(node);
}

session_node_t *get_session_node(uint64_t sessid) {
  session_node_t *node = sessdata.map[sessid % HASHMAP_SIZE];
  if (!node) {
    return NULL;
  }

  while (node) {
    if (node->data.id == sessid) {
      break;
    }
    node = node->next;
  }
  return node;
}

struct session *get_session(uint64_t sessid) {
  session_node_t *ret = get_session_node(sessid);
  if (!ret) {
    return NULL;
  }
  return &ret->data;
}
