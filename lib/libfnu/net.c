// THE NETWORK'S SMALL THINGS
#include "inc/fnu.h"

int fnu_addr_parse(const char *text, uint8 addr[4]) {
  int i, v, digits;

  for (i = 0; i < 4; i++) {
    for (v = 0, digits = 0; *text >= '0' && *text <= '9'; text++) {
      v = v * 10 + (*text - '0');
      if (++digits > 3 || v > 255)
        return -FNU_ERR_INVALID;
    }
    if (digits == 0)
      return -FNU_ERR_INVALID;
    addr[i] = (uint8)v;
    if (i < 3 && *text++ != '.')
      return -FNU_ERR_INVALID;
  }
  return *text == 0 ? 0 : -FNU_ERR_INVALID;
}

int fnu_waitread(int fd, int ms) {
  struct fnu_ready r = {fd, FNU_READY_IN, 0};
  struct fnu_timespec t = {ms / 1000, (ms % 1000) * 1000000, 0};
  int n;

  if ((n = fnu_ready(&r, 1, &t, 0)) < 0)
    return n;
  return n > 0 && (r.got & FNU_READY_IN) ? 0 : -FNU_ERR_TIMEDOUT;
}
