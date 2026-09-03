#include "user.h"

int main(int argc, char **argv) {
  int mode = 0;
  int i;
  if (argc != 3) {
    printf("usage: chmod MODE PATH\n");
    exit();
  }
  for (i = 0; argv[1][i]; i++) {
    if (argv[1][i] < '0' || argv[1][i] > '7') {
      printf("chmod: invalid mode\n");
      exit();
    }
    mode = mode * 8 + argv[1][i] - '0';
  }
  if (mode > 07777 || chmod(argv[2], mode) < 0)
    printf("chmod: operation not permitted\n");
  exit();
}
