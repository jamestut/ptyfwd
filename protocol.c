#include "protocol.h"
#include "utils.h"
#include <assert.h>
#include <stdint.h>
#include <unistd.h>

bool proto_read(int fd, uint16_t *length, enum data_type *type, void *buff) {
  unsigned char hbuff[2];
  // main header
  if (!read_all(fd, hbuff, 1))
    return false;
  int sizelen = (hbuff[0] & 0x80) ? 2 : 1;
  *type = hbuff[0] & 0x7F;

  // this will return wrong result on big endian! (but IDC :D)
  hbuff[1] = 0;
  if (!read_all(fd, hbuff, sizelen))
    return false;
  *length = *((uint16_t *)hbuff);

  return *length ? read_all(fd, buff, *length) : true;
}

bool proto_write(int fd, uint16_t length, enum data_type type, const void *buff) {
  unsigned char hbuff[3];
  int hlen = 2;
  assert(!(type & 0x80));
  hbuff[0] = type;
  if (length > 0xFF) {
    ++hlen;
    hbuff[0] |= 0x80;
  }
  *((uint16_t *)(hbuff + 1)) = length;

  if (!write_all(fd, hbuff, hlen))
    return false;

  return length ? write_all(fd, buff, length) : true;
}
