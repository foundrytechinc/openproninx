#include "user.h"

int main(void) {
  struct user_info info;
  int index;
  for (index = 0; users(&info, index) == 0; index++)
    if (info.uid == (uint)getuid()) { printf("%s\n", info.name); exit(); }
  printf("unknown\n");
  exit();
}
