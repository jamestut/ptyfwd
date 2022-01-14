#pragma once

#define BUFF_SIZE 65536
_Static_assert(BUFF_SIZE >= 65536, "Buffer size needs to be able to fit 16 bit length");

#define COOKIE_MIN_SIZE 64
#define COOKIE_MAX_SIZE 1024

typedef unsigned int UINT;
