#pragma once

#define SIMPLEMAP_MAX_ENTRIES 64

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// no destroy function here :)
void *simplemap_init();

bool simplemap_add(void *inst, uint64_t key, void *value);

void simplemap_del(void *inst, uint64_t key);

bool simplemap_get(void *inst, uint64_t key, void **res);

size_t simplemap_count(void *inst);
