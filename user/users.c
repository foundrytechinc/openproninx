#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnu/sys.h>

static int by_name(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(void) {
  struct fnu_procinfo *t;
  char **names;
  int n, i, k = 0;

  if ((n = fnu_processes(&t)) < 0)
    errx(1, "the processes: %s", fnu_errstr(n));
  if ((names = calloc((size_t)n + 1, sizeof(*names))) == 0)
    err(1, 0);
  for (i = 0; i < n; i++)
    if ((t[i].flags & (FNU_PROC_LEADER | FNU_PROC_CTTY | FNU_PROC_SYSTEM)) ==
            (FNU_PROC_LEADER | FNU_PROC_CTTY) &&
        t[i].login[0] != 0)
      names[k++] = t[i].login;
  qsort(names, (size_t)k, sizeof(*names), by_name);
  for (i = 0; i < k; i++)
    printf("%s%s", i ? " " : "", names[i]);
  if (k > 0)
    printf("\n");
  free(names);
  fnu_table_free(t);
  return 0;
}
