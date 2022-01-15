#include "simplemap.h"
#include <err.h>
#include <stdlib.h>

#define HASHMAP_SIZE 16

typedef struct map_node map_node_t;

struct map_node {
  map_node_t *prev;
  map_node_t *next;
  uint64_t key;
  void *data;
};

struct simplemap {
  map_node_t *map[HASHMAP_SIZE];
  size_t count;
};

map_node_t *get_node(struct simplemap *cinst, uint64_t key);

void *simplemap_init() {
  void *ret = calloc(1, sizeof(struct simplemap));
  return ret;
}

bool simplemap_add(void *inst, uint64_t key, void *value) {
  struct simplemap *cinst = (struct simplemap *)inst;

  if (cinst->count >= SIMPLEMAP_MAX_ENTRIES) {
    warnx("Map content exceeded.");
    return false;
  }

  map_node_t *newnode = calloc(sizeof(map_node_t), 1);
  if (!newnode) {
    warnx("Error allocating map data.");
    return false;
  }
  newnode->key = key;
  newnode->data = value;

  map_node_t *node = cinst->map[key % HASHMAP_SIZE];
  newnode->next = node;
  if (node) {
    node->prev = newnode;
  }

  cinst->map[key % HASHMAP_SIZE] = newnode;
  ++cinst->count;

  return true;
}

void simplemap_del(void *inst, uint64_t key) {
  struct simplemap *cinst = (struct simplemap *)inst;
  map_node_t *node = get_node(cinst, key);
  if (!node) {
    return;
  }

  if (node->prev) {
    node->prev->next = node->next;
  } else {
    cinst->map[key % HASHMAP_SIZE] = node->next;
  }
  if (node->next) {
    node->next->prev = node->prev;
  }
  --cinst->count;

  free(node);
}

bool simplemap_get(void *inst, uint64_t key, void **res) {
  struct simplemap *cinst = (struct simplemap *)inst;
  map_node_t *node = get_node(cinst, key);
  if (!node) {
    return false;
  }

  *res = node->data;
  return true;
}

size_t simplemap_count(void *inst) {
  struct simplemap *cinst = (struct simplemap *)inst;
  return cinst->count;
}

map_node_t *get_node(struct simplemap *cinst, uint64_t key) {
  map_node_t *node = cinst->map[key % HASHMAP_SIZE];
  while (node) {
    if (node->key == key) {
      break;
    }
    node = node->next;
  }
  return node;
}
