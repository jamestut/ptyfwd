#include "session.h"
#include "simplemap.h"
#include <stdlib.h>
#include <err.h>
#include <unistd.h>
#include <string.h>

static void *map_sessid = NULL;

uint64_t new_session(struct session **newsess) {
  if (!map_sessid) {
    map_sessid = simplemap_init();
  }
  if (!map_sessid) {
    warnx("Error allocating map.");
    return INVALID_SESSION_ID;
  }
  if (simplemap_count(map_sessid) >= SIMPLEMAP_MAX_ENTRIES) {
    warnx("Maximum session count exceeded. Rejecting request.");
    return INVALID_SESSION_ID;
  }

  uint64_t sessid = INVALID_SESSION_ID;

  for (;;) {
    arc4random_buf(&sessid, sizeof(sessid));
    void *dummy;
    if (sessid != INVALID_SESSION_ID && !simplemap_get(map_sessid, sessid, &dummy)) {
      struct session *sess = malloc(sizeof(struct session));
      if (!sess) {
        warnx("Error allocating session data.");
        return INVALID_SESSION_ID;
      }
      sess->id = sessid;
      sess->childpid = sess->fdpair[0] = sess->fdpair[1] = -1;

      if (!simplemap_add(map_sessid, sessid, sess)) {
        free(sess);
        warnx("Error adding session data to map.");
        return INVALID_SESSION_ID;
      }

      *newsess = sess;
      return sessid;
    }
  }
}

void delete_session(uint64_t sessid) {
  struct session *sess = get_session(sessid);
  if (!sess) {
    return;
  }

  // close unix socket
  for (int i = 0; i < 2; ++i) {
    if (sess->fdpair[i] >= 0) {
      close(sess->fdpair[i]);
      sess->fdpair[i] = -1;
    }
  }

  simplemap_del(map_sessid, sessid);
  free(sess);
}

struct session *get_session(uint64_t sessid) {
  struct session *sess;
  if (!simplemap_get(map_sessid, sessid, (void **)&sess)) {
    return NULL;
  }
  return sess;
}
