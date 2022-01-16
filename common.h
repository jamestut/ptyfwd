#pragma once

#ifdef __linux__
// for arc4random
#include <bsd/stdlib.h>
#endif

#define BUFF_SIZE 65536ULL
_Static_assert(BUFF_SIZE >= 65536ULL, "Buffer size needs to be able to fit 16 bit length");

#define DEFAULT_PTYBUFSZ 1048576ULL

#define COOKIE_MIN_SIZE 64
#define COOKIE_MAX_SIZE 1024

#define NONCE_SIZE 16
#define ANSWER_SIZE 20 // output of SHA1

#define PROTOCOL_VERSION 3

typedef unsigned int UINT;
