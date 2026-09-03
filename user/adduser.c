#include "user.h"

/*
 * adduser is the administrator-facing spelling of useradd.  Accounts are
 * kept by the kernel authentication service, so no filesystem setup is
 * required here.
 */
int main(int argc, char **argv) {
  char password[65];
  int wheel = 0;

  if (argc == 3 && argv[1][0] == '-' && argv[1][1] == 'w' &&
      argv[1][2] == 0) {
    wheel = 1;
    argv++;
    argc--;
  }
  if (argc != 2) {
    printf("usage: adduser [-w] NAME\n");
    exit();
  }

  printf("New password: ");
  if (read_password(password, sizeof(password)) < 0 ||
      useradd(argv[1], password, wheel) < 0)
    printf("adduser: rejected (requires root; password is at least 8 characters)\n");
  else
    printf("adduser: %s created%s\n", argv[1], wheel ? " in wheel" : "");
  memset(password, 0, sizeof(password));
  exit();
}
