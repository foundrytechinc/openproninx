#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fnu/sys.h>
#include <err.h>

static int bad;

static void check(int ok, const char *what) {
  if (!ok) {
    printf("mmaptest: %s: FAILED\n", what);
    bad++;
  }
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/tmp/mmapfile";
  char buf[8192], *shared, *private, *ro;
  int e;
  int fd, i, pid, n;

  for (i = 0; i < (int)sizeof(buf); i++)
    buf[i] = (char)('a' + i % 26);
  if ((fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0666)) < 0) {
    warn("%s", path);
    exit(1);
  }
  write(fd, buf, sizeof(buf));

  ro = mmap(0, sizeof(buf), PROT_READ, MAP_PRIVATE, fd, 0);
  check(ro != MAP_FAILED, "map read-only");
  check(ro != MAP_FAILED && memcmp(ro, buf, sizeof(buf)) == 0, "read through the mapping");

  shared = mmap(0, sizeof(buf), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  check(shared != MAP_FAILED, "map shared");
  shared[100] = 'X';
  shared[5000] = 'Y';
  check(ro[100] == 'X' && ro[5000] == 'Y', "the second mapping sees the shared write");

  private = mmap(0, sizeof(buf), PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  check(private != MAP_FAILED, "map private");
  check(private[100] == 'X', "private mapping starts as the file is");
  private[100] = 'Z';
  private[7000] = 'Q';
  check(shared[100] == 'X' && shared[7000] == buf[7000], "private write stays private");
  check(private[100] == 'Z', "private write is seen by its writer");

  if ((pid = fork()) == 0) {
    check(shared[100] == 'X' && private[100] == 'Z', "child inherits both mappings");
    shared[200] = 'C';
    private[200] = 'P';
    check(shared[200] == 'C', "child's shared write");
    exit(bad != 0);
  }
  while (waitpid(pid, &e, 0) != pid)
    ;
  check(WIFEXITED(e) && WEXITSTATUS(e) == 0, "child checks");
  check(shared[200] == 'C', "parent sees the child's shared write");
  check(private[200] != 'P', "child's private write stayed with the child");

  check(msync(shared, sizeof(buf), MS_SYNC) == 0, "msync");
  lseek(fd, 0, SEEK_SET);
  n = (int)read(fd, buf, sizeof(buf));
  check(n == (int)sizeof(buf) && buf[100] == 'X' && buf[5000] == 'Y' && buf[200] == 'C' &&
        buf[7000] != 'Q', "the file holds the shared writes and not the private ones");

  check(munmap(private, sizeof(buf)) == 0, "unmap private");
  check(munmap(shared, sizeof(buf)) == 0, "unmap shared");
  check(munmap(ro, sizeof(buf)) == 0, "unmap read-only");
  close(fd);

  if ((fd = open(path, O_RDONLY)) >= 0) {
    n = (int)read(fd, buf, sizeof(buf));
    check(n == (int)sizeof(buf) && buf[100] == 'X' && buf[200] == 'C', "the file after unmapping");
    close(fd);
  }
  check(mmap((void *)(0x800000000000UL - 4096), 8192, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED,
        "no mapping runs past the user's half");
  check(mmap((void *)0xffff800000000000UL, 4096, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED,
        "no mapping lands in the kernel's half");
  check(mmap((void *)0xffffffff80000000UL, 4096, PROT_READ,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED,
        "nor on the kernel's text");
  check(munmap((void *)0xffff800000000000UL, 4096) != 0,
        "nor is the kernel's half unmapped");
  unlink(path);
  printf("mmaptest: %s\n", bad ? "FAILED" : "ok");
  exit(bad != 0);
}
