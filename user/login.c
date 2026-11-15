#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <fnu/sys.h>
#include <fnu/crypt.h>
#include <err.h>
#include <limits.h>
#include <util.h>

#define NOLOGIN "/etc/nologin"
#define MOTD "/etc/motd"
#define HUSHLOGIN ".hushlogin"

static const char *nobody_hash =
    "$2b$10$liZTtiuUbZgkft3xcdUiDem8dFBFU0/.5ntYNJ5ScDSB7wQqmjRN2";

static const char *here(void) {
  const char *t = ttyname(0);

  return t == 0 ? "?" : strncmp(t, "/dev/", 5) == 0 ? t + 5 : t;
}

// the terminal's service said secure=no: insecure, ttys style
static int insecure_here(void) {
  const char *v = getenv("LOGIN_SECURE");

  return v != 0 && strcmp(v, "no") == 0;
}

static void show_motd(const struct passwd *account) {
  char path[PATH_MAX];
  char buf[256];
  int fd, n;

  strlcpy(path, account->pw_dir[0] != 0 ? account->pw_dir : "/", sizeof(path));
  if (path[strlen(path) - 1] != '/')
    strlcpy(path + strlen(path), "/", (int)sizeof(path) - (int)strlen(path));
  strlcpy(path + strlen(path), HUSHLOGIN,
             (int)sizeof(path) - (int)strlen(path));
  if ((fd = open(path, O_RDONLY)) >= 0) {
    close(fd);
    return;
  }

  if ((fd = open(MOTD, O_RDONLY)) < 0)
    return;
  while ((n = (int)read(fd, buf, sizeof(buf))) > 0)
    write(1, buf, n);
  close(fd);
}

static int nobody_but_root(void) {
  int fd = open(NOLOGIN, O_RDONLY);

  if (fd < 0)
    return 0;
  close(fd);
  return 1;
}

int main(void) {
  char name[LOGIN_NAME_MAX], password[CRYPT_PASSWORD_MAX];
  struct passwd account, *found;
  char *shell[] = {"-sh", 0};
  char *path = "/usr/bin/sh", *line;
  int known;

  for (;;) {
    printf("OpenProninx login: ");
    fflush(stdout);
    if (fgets(name, sizeof(name), stdin) == 0)
      exit(0);
    if (strlen(name) &&
        (name[strlen(name) - 1] == '\n' || name[strlen(name) - 1] == '\r'))
      name[strlen(name) - 1] = 0;
    if (name[0] == 0) {
      sleep(1);
      continue;
    }

    if (read_password("Password: ", password, sizeof(password)) < 0) {
      memset(password, 0, sizeof(password));
      continue;
    }

    if (nobody_but_root() && (found = getpwnam(name)) != 0 &&
        found->pw_uid != 0) {
      printf("Logins are disabled: %s\n", NOLOGIN);
      sleep(1);
      continue;
    }

    known = (found = getpwnam(name)) != 0;
    if (known)
      account = *found;
    if (crypt_checkpass(password, known ? account.pw_passwd : nobody_hash) != 0)
      known = 0;
    memset(password, 0, sizeof(password));
    if (known && account.pw_uid == 0 && insecure_here()) {
      printf("root login refused on this terminal\n");
      syslog(LOG_AUTH | LOG_NOTICE, "ROOT LOGIN REFUSED ON %s", here());
      sleep(1);
      continue;
    }
    if (known)
      break;
    syslog(LOG_AUTH | LOG_NOTICE, "LOGIN FAILURE ON %s, %s", here(), name);
    printf("Login incorrect\n");
    sleep(1);
  }

  if (account.pw_uid == 0)
    syslog(LOG_AUTH | LOG_NOTICE, "ROOT LOGIN ON %s", here());
  else
    syslog(LOG_AUTH | LOG_INFO, "login on %s as %s", here(), account.pw_name);
  // the shell takes this session
  if (setlogin(account.pw_name) < 0)
    warn("setlogin");
  if ((line = ttyname(0)) != 0)
    logwtmp(strncmp(line, "/dev/", 5) == 0 ? line + 5 : line, account.pw_name, "");
  // groups before the uid
  if (initgroups(account.pw_name, account.pw_gid) < 0)
    warn("setgroups");
  if (setgid(account.pw_gid) < 0 || setuid(account.pw_uid) < 0)
    err(1, "%s", account.pw_name);
  setenv("USER", account.pw_name, 1);
  setenv("LOGNAME", account.pw_name, 1);
  setenv("HOME", account.pw_dir[0] != 0 ? account.pw_dir : "/", 1);
  setenv("SHELL", account.pw_shell[0] != 0 ? account.pw_shell : "/usr/bin/sh", 1);
  setenv("PATH", "/usr/bin:/bin", 1);
  setenv("TERM", "xterm", 1);
  setenv("ENV", "$HOME/.shrc", 1);

  if (account.pw_dir[0] != 0 && chdir(account.pw_dir) < 0)
    printf("login: no directory %s, starting at /\n", account.pw_dir);
  show_motd(&account);
  if (account.pw_shell[0] != 0)
    path = account.pw_shell;
  execv(path, shell);
  err(1, "%s", path);
}
