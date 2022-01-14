#include "global.h"

struct cookie cookie = {};

const uint8_t preamble[8] = {'p', 't', 'y', 'f', 'w', 'd', PROTOCOL_VERSION & 0xFF, (PROTOCOL_VERSION >> 8) & 0xFF};
