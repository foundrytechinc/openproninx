// unlink — remove a single file (POSIX)
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int i, rc = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: unlink file\n");
    return 2;
  }
  for (i = 1; i < argc; i++)
    if (unlink(argv[i]) != 0) {
      perror(argv[i]);
      rc = 1;
    }
  return rc;
}
