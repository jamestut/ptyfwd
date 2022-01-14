#pragma once

// store all global app config here

#include <stdint.h>
#include "common.h"

struct cookie {
  uint16_t size;
  uint8_t data[COOKIE_MAX_SIZE];
};

extern struct cookie cookie;

extern const uint8_t preamble[8];
