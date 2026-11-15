#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf back;
static int bad;

static void caught(int sig) {
  (void)sig;
  siglongjmp(back, 1);
}

static const unsigned char ret_only[] = {0xc3};

static void check(int ok, const char *what) {
  printf("nxtest: %-34s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok)
    bad++;
}

static int refuses(void *mem) {
  void (*fn)(void) = (void (*)(void))mem;

  if (sigsetjmp(back, 1) == 0) {
    fn();
    return 0;
  }
  return 1;
}

int main(void) {
  unsigned char stackbuf[16];
  unsigned char *heap;
  unsigned char *page;
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = caught;
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
  sigaction(SIGILL, &sa, 0);

  memcpy(stackbuf, ret_only, sizeof(ret_only));
  check(refuses(stackbuf), "the stack does not execute");

  if ((heap = malloc(64)) == 0)
    return 1;
  memcpy(heap, ret_only, sizeof(ret_only));
  check(refuses(heap), "the heap does not execute");

  page = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (page == MAP_FAILED)
    return 1;
  memcpy(page, ret_only, sizeof(ret_only));
  check(refuses(page), "a mapping without PROT_EXEC does not");
  munmap(page, 4096);

  page = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
              MAP_PRIVATE | MAP_ANON, -1, 0);
  if (page == MAP_FAILED)
    return 1;
  memcpy(page, ret_only, sizeof(ret_only));
  check(!refuses(page), "a mapping with PROT_EXEC does");
  munmap(page, 4096);

  printf(bad == 0 ? "nxtest: ok\n" : "nxtest: %d failed\n", bad);
  return bad != 0;
}
