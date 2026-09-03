#include "user.h"

int main(void) {
  struct user_info info;
  int index;
  printf("name             uid  gid  groups\n");
  for (index = 0; users(&info, index) == 0; index++)
    printf("%-16s %d %d  %s\n", info.name, info.uid, info.gid,
           info.wheel ? "wheel" : "");
  exit();
}
