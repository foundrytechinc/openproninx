// dir and name put together
#include <string.h>
#include <util.h>

void pathjoin(char *out, int size, const char *dir, const char *name) {
  int n;

  strlcpy(out, (char *)dir, size);
  n = (int)strlen(out);
  if (n > 0 && out[n - 1] != '/' && n + 1 < size) {
    out[n++] = '/';
    out[n] = 0;
  }
  strlcpy(out + n, (char *)name, size - n);
}
