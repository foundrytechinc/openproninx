// nologin: the shell of an account nobody logs into
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define NOLOGIN_TXT "/etc/nologin.txt"
#define SAID "This account is currently not available.\n"

int main(void) {
  struct passwd *pw = getpwuid(getuid());
  const char *tty = ttyname(STDIN_FILENO);
  char buf[512];
  ssize_t n;
  int fd;

  openlog("nologin", LOG_CONS, LOG_AUTH);
  syslog(LOG_NOTICE, "attempted login by %s on %s", pw != 0 ? pw->pw_name : "?",
         tty != 0 ? tty : "no tty");
  closelog();
  if ((fd = open(NOLOGIN_TXT, O_RDONLY)) >= 0) {
    while ((n = read(fd, buf, sizeof(buf))) > 0)
      write(STDOUT_FILENO, buf, (size_t)n);
    close(fd);
  } else {
    write(STDOUT_FILENO, SAID, strlen(SAID));
  }
  return 1;
}
