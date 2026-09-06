/*
 * FNU/OpenProninx man — wrapper for fnuman
 */
#include "user.h"

int main(int argc, char **argv) {
  argv[0] = "fnuman";
  exec("/fnuman", argv);
  exec("/usr/bin/fnuman", argv);
  dprintf(2, "man: cannot execute fnuman\n");
  exit();
}
