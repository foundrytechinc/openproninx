#ifndef _SYS_UN_H_
#define _SYS_UN_H_

#include <sys/types.h>
#include "inc/abi.h"

// a local socket's name: a path in the filesystem
struct sockaddr_un {
  uint8_t sun_len;
  sa_family_t sun_family;
  char sun_path[104];
};

#define SUN_LEN(su) (sizeof(*(su)) - sizeof((su)->sun_path) + strlen((su)->sun_path))

#endif
