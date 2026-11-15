
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FILE_ "/tmp/identtest.f"
#define USER 1000
#define GROUP 100
#define OTHER 5

static int failures;

static void ok(const char *what, int good) {
  printf("identtest: %-48s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

static gid_t group_of(const char *path) {
  struct stat st;

  return stat(path, &st) == 0 ? st.st_gid : (gid_t)-1;
}

static void ordinary(void) {
  gid_t list[1] = {GROUP};
  char name[64];

  ok("root sets the groups", setgroups(1, list) == 0);
  ok("and the group", setgid(GROUP) == 0);
  ok("and the login name", setlogin("identtest") == 0);
  ok("and becomes the user", setuid(USER) == 0);
  ok("the real and effective user", getuid() == USER && geteuid() == USER);
  ok("the group and one group", getgid() == GROUP && getgroups(0, 0) == 1);
  ok("the login name kept",
     getlogin_r(name, sizeof(name)) == 0 && strcmp(name, "identtest") == 0);
  ok("a login name too short a room is ERANGE",
     getlogin_r(name, 4) == ERANGE);

  ok("the owner gives the file to a group of theirs",
     chown(FILE_, (uid_t)-1, GROUP) == 0 && group_of(FILE_) == GROUP);
  ok("but not to one they are not in",
     chown(FILE_, (uid_t)-1, OTHER) < 0 && errno == EPERM);
  ok("nor to another user", chown(FILE_, 0, (gid_t)-1) < 0 && errno == EPERM);
  ok("asking for no change is no change", chown(FILE_, (uid_t)-1, (gid_t)-1) == 0);
  ok("the owner sets the mode", chmod(FILE_, 0600) == 0);

  ok("its niceness starts at 0", getpriority(PRIO_PROCESS, 0) == 0);
  ok("it may be nicer", setpriority(PRIO_PROCESS, 0, 5) == 0 &&
                            getpriority(PRIO_PROCESS, 0) == 5);
  ok("but not less nice again",
     setpriority(PRIO_PROCESS, 0, 2) < 0 && errno == EACCES);
  errno = 0;
  ok("nor through nice", nice(-1) == -1 && errno == EACCES);
  ok("nice moves it on", nice(2) == 7);
  ok("the user's least nice is its own", getpriority(PRIO_USER, USER) == 7);
  ok("init is not its to move",
     setpriority(PRIO_PROCESS, 1, 10) < 0 && errno == EPERM);
  ok("no such process is ESRCH",
     getpriority(PRIO_PROCESS, 999999) == -1 && errno == ESRCH);

  ok("it cannot become root", setuid(0) < 0 && errno == EPERM);
  ok("nor take groups", setgroups(1, list) < 0 && errno == EPERM);
  ok("nor name the session", setlogin("root") < 0 && errno == EPERM);
}

int main(void) {
  pid_t pid;
  int st, fd;

  if (getuid() != 0)
    errx(1, "run as root");
  ok("nothing changed who it is", issetugid() == 0);
  if ((fd = open(FILE_, O_CREAT | O_TRUNC | O_WRONLY, 0644)) < 0)
    err(1, "%s", FILE_);
  close(fd);
  ok("root gives the file away", chown(FILE_, USER, 0) == 0);

  fflush(stdout);
  if ((pid = fork()) < 0)
    err(1, "fork");
  if (pid == 0) {
    if (setsid() < 0)
      err(1, "setsid");
    ordinary();
    fflush(stdout);
    _exit(failures != 0);
  }
  ok("the ordinary user's rules held",
     waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
  ok("the caller's niceness is untouched", getpriority(PRIO_PROCESS, 0) == 0);
  unlink(FILE_);

  printf(failures ? "identtest: FAILED\n" : "identtest: ok\n");
  return failures != 0;
}
