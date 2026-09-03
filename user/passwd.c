#include "user.h"

int main(int argc, char **argv) {
  char password[65], name[USER_NAME_MAX];
  struct user_info info;
  int index;
  if (argc > 2) { printf("usage: passwd [USER]\n"); exit(); }
  if (argc == 2) {
    strncpy(name, argv[1], sizeof(name));
    name[sizeof(name) - 1] = 0;
  } else {
    for (index = 0; users(&info, index) == 0; index++)
      if (info.uid == (uint)getuid()) break;
    if (users(&info, index) < 0) { printf("passwd: unknown user\n"); exit(); }
    strncpy(name, info.name, sizeof(name));
    name[sizeof(name) - 1] = 0;
  }
  printf("New password: ");
  if (read_password(password, sizeof(password)) < 0 || passwd(name, password) < 0)
    printf("passwd: rejected\n");
  else
    printf("passwd: updated\n");
  memset(password, 0, sizeof(password));
  exit();
}
