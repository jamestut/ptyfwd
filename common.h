#pragma once

#define BUFF_SIZE 65536
_Static_assert(BUFF_SIZE >= 65536, "Buffer size needs to be able to fit 16 bit length");

typedef unsigned int UINT;
