#pragma once

#include <stdbool.h>
#include <stdint.h>

// the protocol of ptyfwd is in format:
//  - 1 byte type
//  - 1 byte size if !(type & 0x80), else 2 byte size
//  - data of len `size`

// NOTE: all data in host (sender) byte order!
// no one uses big-endian anyway :D

enum data_type {
  DT_PREAMBLE,
  DT_AUTH,
  DT_NONE,      // move on to the next data
  DT_CLOSE,     // request to close/finish session
  DT_REGULAR,   // forward this data as-is to mPTY or stdio
  DT_WINCH,     // window size information
  DT_SESSID,    // persistent session id
};

struct winch_data {
  uint16_t rows;
  uint16_t cols;
};

bool proto_read(int fd, uint16_t *length, enum data_type *type, void *buff);

bool proto_write(int fd, uint16_t length, enum data_type type, const void *buff);
