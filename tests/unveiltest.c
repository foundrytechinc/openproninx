
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DIR_ "/tmp/unveiltest"

static int failures;

static void ok(const char *what, int good) {
  printf("unveiltest: %-48s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

static int opens(const char *path, int flags) {
  int fd = open(path, flags, 0644);

  if (fd >= 0)
    close(fd);
  return fd >= 0 ? 0 : errno;
}

static void veiled(void) {
  struct stat st;
  char *argv[] = {"sh", "-c", "test -r /etc/passwd", 0};
  pid_t pid;
  int status;

  ok("/etc unveiled for reading", unveil("/etc", "r") == 0);
  ok("one name in the test directory, rwc", unveil(DIR_ "/mine", "rwc") == 0);
  ok("the shell to run, x", unveil("/usr/bin/sh", "rx") == 0);
  ok("a mount point, /dev", unveil("/dev", "r") == 0);
  ok("a bad permission is EINVAL", unveil("/etc", "q") < 0 && errno == EINVAL);

  ok("a file under /etc reads", opens("/etc/passwd", O_RDONLY) == 0);
  ok("but does not write: EACCES", opens("/etc/passwd", O_WRONLY) == EACCES);
  ok("what is not unveiled is not there", opens("/usr/bin/true", O_RDONLY) == ENOENT);
  ok("not even the root", stat("/", &st) < 0 && errno == ENOENT);
  ok("the name unveiled is made", opens(DIR_ "/mine", O_CREAT | O_WRONLY) == 0);
  ok("its neighbour is not", opens(DIR_ "/other", O_CREAT | O_WRONLY) == ENOENT);
  ok("a relative walk out of /etc fails", chdir("/etc") == 0 &&
                                             opens("../usr/bin/true", O_RDONLY) == ENOENT);
  ok("and in it works", opens("passwd", O_RDONLY) == 0);
  ok("a device under the mount point reads", opens("/dev/null", O_RDONLY) == 0);
  ok("and the mount point lists", opens("/dev", O_RDONLY | O_DIRECTORY) == 0);

  ok("unveil(NULL, NULL) closes the list", unveil(0, 0) == 0);
  ok("then nothing more is unveiled", unveil("/usr", "r") < 0 && errno == EPERM);

  if ((pid = fork()) == 0) {
    execv("/usr/bin/sh", argv);
    _exit(127);
  }
  ok("an exec sees every name again",
     waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
  pid_t pid;
  int status;

  mkdir(DIR_, 0755);
  unlink(DIR_ "/mine");
  unlink(DIR_ "/other");
  fflush(stdout);
  if ((pid = fork()) < 0)
    err(1, "fork");
  if (pid == 0) {
    veiled();
    fflush(stdout);
    _exit(failures != 0);
  }
  ok("the veiled child's checks held",
     waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  ok("the parent still sees the root", access("/", R_OK) == 0);
  unlink(DIR_ "/mine");
  rmdir(DIR_);
  printf(failures ? "unveiltest: FAILED\n" : "unveiltest: ok\n");
  return failures != 0;
}
