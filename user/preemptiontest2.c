#include "user.h"

char *msg = "I am preemptiontest2\n";

int main(void) {
  int pid;
  for (int i = 0; i < 100; i++) {
    printf("%s", msg);
  }
  printf("preemptiontest%d finished\n", 2);
}
