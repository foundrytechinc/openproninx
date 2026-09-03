#include "user.h"

static int number(char *s, int *value) {
  int n = 0, i;
  if (!s[0]) return -1;
  for (i = 0; s[i]; i++) {
    if (s[i] < '0' || s[i] > '9') return -1;
    n = n * 10 + s[i] - '0';
  }
  *value = n;
  return 0;
}

int main(int argc, char **argv) {
  char *separator;
  int uid, gid;
  if (argc != 3) {
    printf("usage: chown UID:GID PATH\n");
    exit();
  }
  separator = strchr(argv[1], ':');
  if (!separator) {
    printf("chown: owner and group are required\n");
    exit();
  }
  *separator = 0;
  if (number(argv[1], &uid) < 0 || number(separator + 1, &gid) < 0 ||
      chown(argv[2], uid, gid) < 0)
    printf("chown: operation not permitted\n");
  exit();
}
