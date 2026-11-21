// pathchk — check path names for potential problems (POSIX)
// -p: portable mode (historical limits: 14-byte components, 255-byte path)
#include <stdio.h>
#include <string.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

static int check(const char *path, int portable) {
  int bad = 0;
  size_t len = strlen(path);
  const char *p;

  if (len == 0) {
    fprintf(stderr, "pathchk: empty path\n");
    return 1;
  }
  if (len >= PATH_MAX) {
    fprintf(stderr, "pathchk: %s: path too long (%zu >= %d)\n", path, len,
        PATH_MAX);
    bad = 1;
  }
  if (!portable)
    return bad;

  if (len > 255) {
    fprintf(stderr, "pathchk: %s: path longer than 255\n", path);
    bad = 1;
  }
  p = path;
  while (*p) {
    const char *slash = strchr(p, '/');
    size_t clen = slash ? (size_t)(slash - p) : strlen(p);
    if (clen > 255) {
      fprintf(stderr, "pathchk: %s: component longer than 255\n", path);
      bad = 1;
    }
    if (clen > 14) {
      fprintf(stderr, "pathchk: %s: component longer than 14\n", path);
      bad = 1;
    }
    if (!slash)
      break;
    p = slash + 1;
    if (!*p)
      break;
  }
  return bad;
}

int main(int argc, char **argv) {
  int i, portable = 0, rc = 0, npath = 0;
  int endopts = 0;

  for (i = 1; i < argc; i++) {
    if (!endopts && strcmp(argv[i], "--") == 0) {
      endopts = 1;
      continue;
    }
    if (!endopts && strcmp(argv[i], "-p") == 0) {
      portable = 1;
      continue;
    }
    if (!endopts && argv[i][0] == '-' && argv[i][1] != 0) {
      fprintf(stderr, "usage: pathchk [-p] path...\n");
      return 2;
    }
    npath++;
    if (check(argv[i], portable))
      rc = 1;
  }
  if (npath == 0) {
    fprintf(stderr, "usage: pathchk [-p] path...\n");
    return 2;
  }
  return rc;
}
