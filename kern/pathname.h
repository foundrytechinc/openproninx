#ifndef PRONINX_PATHNAME_H
#define PRONINX_PATHNAME_H

#include "inc/types.h"

struct pathname {
  char *pn_buf;
  size_t pn_len; // the end counted
};

int pn_get(uintptr_t upath, struct pathname *pn);
int pn_get_maybe(uintptr_t upath, struct pathname *pn);
void pn_free(struct pathname *pn);

#endif
