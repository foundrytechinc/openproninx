#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <err.h>

#define PATTERN 0xA5

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/tmp/synctest";
  char out[4096], in[4096];
  int fd, i, bad = 0;

  for (i = 0; i < (int)sizeof(out); i++)
    out[i] = (char)(PATTERN ^ i);

  if ((fd = open(path, O_CREAT | O_RDWR, 0666)) < 0) {
    warn("%s", path);
    exit(1);
  }
  for (i = 0; i < 16; i++)
    if (write(fd, out, sizeof(out)) != (int)sizeof(out)) {
      dprintf(2, "synctest: short write at block %d\n", i);
      close(fd);
      exit(1);
    }
  if (fsync(fd) < 0) {
    warn("fsync");
    close(fd);
    exit(1);
  }
  close(fd);

  if ((fd = open(path, O_RDONLY)) < 0) {
    warn("%s", path);
    exit(1);
  }
  for (i = 0; i < 16; i++) {
    if (read(fd, in, sizeof(in)) != (int)sizeof(in)) {
      printf("synctest: short read at block %d\n", i);
      bad++;
      break;
    }
    if (memcmp(in, out, sizeof(in)) != 0) {
      printf("synctest: block %d came back wrong\n", i);
      bad++;
    }
  }
  close(fd);
  printf("synctest: %s\n", bad == 0 ? "64 KB written, fsynced and read back"
                                    : "FAILED");
  exit(bad != 0);
}
