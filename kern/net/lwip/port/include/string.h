#ifndef PRONINX_LWIP_STRING_H
#define PRONINX_LWIP_STRING_H

#include "inc/string.h"

#define memcpy(destination, source, length) memmove(destination, source, length)

#endif
