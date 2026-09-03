#include "user.h"

int main(int argc, char **argv) {
  char password[65];
  char path[64];
  int i;
  pid_t pid;
  if (argc < 2) { printf("usage: doas command [arguments]\n"); exit(); }
  printf("doas password: ");
  if (read_password(password, sizeof(password)) < 0 || doas_auth(password) < 0) {
    memset(password, 0, sizeof(password));
    printf("doas: authentication failed or user is not in wheel\n");
    exit();
  }
  memset(password, 0, sizeof(password));
  if (argv[1][0] == '/') {
    strncpy(path, argv[1], sizeof(path));
  } else {
    path[0] = '/';
    path[1] = 'u'; path[2] = 's'; path[3] = 'r'; path[4] = '/';
    path[5] = 'b'; path[6] = 'i'; path[7] = 'n'; path[8] = '/';
    for (i = 0; argv[1][i] && i < (int)sizeof(path) - 10; i++)
      path[i + 9] = argv[1][i];
    path[i + 9] = 0;
  }
  pid = fork();
  if (pid == 0) {
    if (setuid(0) < 0 || exec(path, argv + 1) < 0)
      printf("doas: cannot execute %s\n", argv[1]);
    exit();
  }
  if (pid < 0) printf("doas: cannot fork\n"); else wait();
  exit();
}
