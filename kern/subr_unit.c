// UNIT NUMBERS
#include "defs.h"
#include "malloc.h"
#include "mutex.h"
#include "unit.h"

void init_unrhdr(struct unrhdr *uh, const char *name) {
  memset(uh, 0, sizeof(*uh));
  mtx_init(&uh->lock, name);
}

int alloc_unr(struct unrhdr *uh) {
  uint8_t *grown, *old;
  int i, n;

  for (;;) {
    mtx_enter(&uh->lock);
    for (i = 0; i < uh->n; i++)
      if (!uh->used[i]) {
        uh->used[i] = 1;
        mtx_leave(&uh->lock);
        return i;
      }
    n = uh->n;
    mtx_leave(&uh->lock);
    grown = malloc((size_t)n * 2 + 8, M_DEVBUF, M_WAITOK | M_ZERO);
    mtx_enter(&uh->lock);
    if (uh->n != n) {
      mtx_leave(&uh->lock);
      free(grown, M_DEVBUF);
      continue;
    }
    if (n > 0)
      memmove(grown, uh->used, (size_t)n);
    old = uh->used;
    uh->used = grown;
    uh->n = n * 2 + 8;
    mtx_leave(&uh->lock);
    if (old != 0)
      free(old, M_DEVBUF);
  }
}

void free_unr(struct unrhdr *uh, int unit) {
  mtx_enter(&uh->lock);
  if (unit >= 0 && unit < uh->n)
    uh->used[unit] = 0;
  mtx_leave(&uh->lock);
}
