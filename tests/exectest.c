// EXEC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <fnu/sys.h>

#define SELF "/usr/tests/exectest"
#define COPY "/exectest.copy"
#define PAGE 4096

static int bad;

static char data[3 * PAGE] = {1};
static int counter = 42;

static void check(int ok, const char *what) {
  printf("exectest: %-48s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok)
    bad++;
}

__attribute__((noinline)) static int marker(void) { return 0x34; }

static int (*volatile marker_p)(void) = marker;

static int pristine(void) {
  int i;

  for (i = 1; i < (int)sizeof(data); i++)
    if (data[i] != 0)
      return 0;
  return data[0] == 1 && counter == 42;
}

static int run(const char *path, const char *mode) {
  int status;
  pid_t pid = fork();

  if (pid == 0) {
    execl(path, path, mode, (char *)0);
    _exit(errno == ETXTBSY ? 100 : 101);
  }
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
    return -1;
  return WEXITSTATUS(status);
}

static int copy_self(int *fdp) {
  char buf[PAGE];
  int in, out;
  ssize_t n;

  unlink(COPY);
  if ((in = open(SELF, O_RDONLY)) < 0)
    return -1;
  if ((out = open(COPY, O_WRONLY | O_CREAT | O_TRUNC, 0755)) < 0) {
    close(in);
    return -1;
  }
  while ((n = read(in, buf, sizeof(buf))) > 0)
    if (write(out, buf, (size_t)n) != n) {
      n = -1;
      break;
    }
  close(in);
  if (n < 0) {
    close(out);
    return -1;
  }
  *fdp = out;
  return 0;
}

static void busy(void) {
  int fd, p[2];
  pid_t pid;
  char c;

  if (pipe(p) < 0 || (pid = fork()) < 0)
    return;
  if (pid == 0) {
    dup2(p[1], 1);
    close(p[0]);
    execl(COPY, COPY, "wait", (char *)0);
    _exit(1);
  }
  close(p[1]);
  check(read(p[0], &c, 1) == 1, "the copy runs");
  errno = 0;
  fd = open(COPY, O_WRONLY);
  check(fd < 0 && errno == ETXTBSY, "a running program is not opened to write");
  if (fd >= 0)
    close(fd);
  errno = 0;
  check(truncate(COPY, 0) < 0 && errno == ETXTBSY,
        "nor truncated");
  kill(pid, SIGKILL);
  waitpid(pid, 0, 0);
  close(p[0]);
  fd = open(COPY, O_WRONLY);
  check(fd >= 0, "once it is gone it may be written");
  if (fd >= 0)
    close(fd);
}

static void own_file(void) {
  char *page = (char *)(((uintptr_t)data + PAGE) & ~(uintptr_t)(PAGE - 1));
  char *want = malloc(PAGE);
  int fd = open(SELF, O_RDONLY);

  if (fd < 0 || want == 0) {
    check(0, "the program's own file opens");
    return;
  }
  check(read(fd, page, PAGE) == PAGE,
        "its file reads into a page of its file");
  lseek(fd, 0, SEEK_SET);
  check(read(fd, want, PAGE) == PAGE && memcmp(page, want, PAGE) == 0,
        "and the bytes are the file's");
  close(fd);
  free(want);
  check(run(SELF, "pristine") == 0, "another run sees its data unchanged");
}

static void poke(void) {
  uintptr_t at = (uintptr_t)marker;
  int status;
  int64_t word;
  pid_t pid = fork();

  if (pid == 0) {
    ptrace(PT_TRACE_ME, 0, 0, 0);
    execl(SELF, SELF, "marker", (char *)0);
    _exit(1);
  }
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
    check(0, "a traced child stops at exec");
    return;
  }
  errno = 0;
  word = ptrace(PT_READ_I, pid, at, 0);
  check(errno == 0, "the debugger reads the text");
  // mov $0x77, %eax; ret
  word = (word & ~(int64_t)0xffffffffffff) | 0xc300000077b8;
  check(ptrace(PT_WRITE_I, pid, at, word) == 0, "and writes into it");
  ptrace(PT_CONTINUE, pid, (uintptr_t)1, 0);
  check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0x77,
        "the child runs what was written");
  check(marker_p() == 0x34 && run(SELF, "marker") == 0x34,
        "the file and this process keep their own");
}

// the kernel must refuse these, not take them to iretq
static uint64_t bad_rip, bad_cs;

static void forge(int sig, siginfo_t *si, void *ctx) {
  struct fnu_sigcontext *sc = ctx;

  (void)sig;
  (void)si;
  if (bad_rip != 0)
    sc->rip = bad_rip;
  if (bad_cs != 0)
    sc->cs = (uint16_t)bad_cs;
}

static int forged_return(uint64_t rip, uint64_t cs) {
  struct sigaction sa;
  int status;
  pid_t pid = fork();

  if (pid == 0) {
    bad_rip = rip;
    bad_cs = cs;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = forge;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, 0);
    raise(SIGUSR1);
    _exit(0);
  }
  if (pid < 0 || waitpid(pid, &status, 0) != pid)
    return 0;
  return WIFSIGNALED(status);
}

static void bad_entry(void) {
  uint64_t entry = 0x8000000000000000ull;
  int fd;

  if (copy_self(&fd) < 0) {
    check(0, "the program copies");
    return;
  }
  check(pwrite(fd, &entry, sizeof(entry), 24) == sizeof(entry), "its entry made non-canonical");
  close(fd);
  check(run(COPY, "pristine") == 101, "and it is not run");
  unlink(COPY);
}

int main(int argc, char **argv) {
  int fd = -1, ret;

  if (argc > 1 && strcmp(argv[1], "pristine") == 0)
    return pristine() ? 0 : 1;
  if (argc > 1 && strcmp(argv[1], "marker") == 0)
    return marker_p();
  if (argc > 1 && strcmp(argv[1], "wait") == 0) {
    write(1, "r", 1);
    for (;;)
      pause();
  }

  errno = 0;
  fd = open(SELF, O_WRONLY);
  check(fd < 0 && errno == ETXTBSY, "this program is not opened to write");
  if (fd >= 0)
    close(fd);

  counter = 7;
  data[2] = 9;
  check(run(SELF, "pristine") == 0, "data written here is not another's");

  if (copy_self(&fd) < 0) {
    check(0, "the program copies");
  } else {
    ret = run(COPY, "pristine");
    check(ret == 100, "a file open to write is not run");
    close(fd);
    check(run(COPY, "pristine") == 0, "closed, it runs");
    busy();
    unlink(COPY);
  }

  own_file();
  poke();
  bad_entry();
  check(forged_return(0x8000000000000000ull, 0), "a non-canonical rip from a handler kills");
  check(forged_return(0, 0x0b), "a kernel cs from a handler kills");

  printf(bad == 0 ? "exectest: ok\n" : "exectest: %d failed\n", bad);
  return bad != 0;
}
