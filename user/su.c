#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <fnu/sys.h>
#include <fnu/crypt.h>
#include <err.h>
#include <limits.h>
#include <libgen.h>
#include <util.h>

#define ROOT_GROUP "wheel"
#define MAXSHARGS 64

static int in_group(const char *group, const char *name, gid_t gid) {
  struct group *gr = getgrnam(group);
  int i;

  if (gr == 0)
    return 0;
  if (gr->gr_gid == gid)
    return 1;
  for (i = 0; gr->gr_mem[i] != 0; i++)
    if (strcmp(gr->gr_mem[i], name) == 0)
      return 1;
  return 0;
}

static void usage(void) {
  dprintf(2, "usage: su [-flm] [-s shell] [login [argument ...]]\n");
  exit(1);
}

int main(int argc, char **argv) {
  char password[CRYPT_PASSWORD_MAX];
  char *shargv[MAXSHARGS], name0[PATH_MAX + 1];
  struct passwd me, target, *found;
  const char *want = "root", *altshell = 0;
  char *path;
  int asthem = 0, asme = 0, i = 1, n = 0;
  uint caller;

  for (; i < argc && argv[i][0] == '-'; i++) {
    const char *p = argv[i] + 1;

    if (argv[i][1] == 0) {
      asthem = 1;
      asme = 0;
      continue;
    }
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *p != 0; p++) {
      if (*p == 'l') {
        asthem = 1;
        asme = 0;
      } else if (*p == 'm') {
        asme = 1;
        asthem = 0;
      } else if (*p == 'f')
        ;
      else if (*p == 's') {
        altshell = p[1] != 0 ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (altshell == 0)
          usage();
        p += strlen(p) - 1;
      } else if (*p == 'c' || *p == 'a') {
        dprintf(2, "su: there are no login classes here\n");
        exit(1);
      } else
        usage();
    }
  }
  if (i < argc)
    want = argv[i++];

  caller = (uint)getuid();
  if ((found = getpwuid(caller)) == 0) {
    dprintf(2, "su: no account owns uid %d\n", caller);
    exit(1);
  }
  me = *found;
  if ((found = getpwnam(want)) == 0) {
    dprintf(2, "su: unknown login %s\n", want);
    exit(1);
  }
  target = *found;
  if (caller != 0 && altshell != 0) {
    dprintf(2, "su: only the superuser may specify a login shell\n");
    exit(1);
  }

  if (caller != 0 && target.pw_uid == 0 &&
      !in_group(ROOT_GROUP, me.pw_name, me.pw_gid)) {
    dprintf(2, "su: you are not in the correct group to su %s\n", target.pw_name);
    exit(1);
  }

  if (caller != 0 && caller != target.pw_uid) {
    if (read_password("Password:", password, sizeof(password)) < 0 ||
        crypt_checkpass(password, target.pw_passwd) != 0) {
      memset(password, 0, sizeof(password));
      syslog(LOG_AUTH | LOG_NOTICE, "BAD SU %s to %s on %s", me.pw_name, target.pw_name,
             ttyname(0) != 0 ? ttyname(0) : "?");
      dprintf(2, "Sorry\n");
      exit(1);
    }
    memset(password, 0, sizeof(password));
  }

  syslog(LOG_AUTH | (target.pw_uid == 0 ? LOG_NOTICE : LOG_INFO), "%s to %s on %s",
         me.pw_name, target.pw_name, ttyname(0) != 0 ? ttyname(0) : "?");
  // groups before the uid
  if (initgroups(target.pw_name, target.pw_gid) < 0)
    warn("setgroups");
  if (setgid(target.pw_gid) < 0 || setuid(target.pw_uid) < 0) {
    warn("%s", target.pw_name);
    exit(1);
  }

  if (altshell != 0)
    path = (char *)altshell;
  else if (asme && me.pw_shell[0] != 0)
    path = me.pw_shell;
  else
    path = target.pw_shell[0] != 0 ? target.pw_shell : "/usr/bin/sh";

  if (asthem) {
    const char *term = getenv("TERM");

    setenv("USER", target.pw_name, 1);
    setenv("LOGNAME", target.pw_name, 1);
    setenv("HOME", target.pw_dir[0] != 0 ? target.pw_dir : "/", 1);
    setenv("SHELL", path, 1);
    setenv("PATH", "/usr/bin:/bin", 1);
    setenv("TERM", term != 0 ? term : "vt100", 1);
    if (target.pw_dir[0] != 0 && chdir(target.pw_dir) < 0) {
      printf("No home directory %s!\n", target.pw_dir);
      printf("Logging in with home = \"/\".\n");
      chdir("/");
    }
  } else if (!asme) {
    setenv("USER", target.pw_name, 1);
    setenv("LOGNAME", target.pw_name, 1);
    setenv("HOME", target.pw_dir[0] != 0 ? target.pw_dir : "/", 1);
    setenv("SHELL", path, 1);
  }

  strlcpy(name0 + 1, basename(path), sizeof(name0) - 1);
  name0[0] = '-';
  shargv[n++] = asthem ? name0 : name0 + 1;
  for (; i < argc && n < MAXSHARGS - 1; i++)
    shargv[n++] = argv[i];
  shargv[n] = 0;

  execv(path, shargv);
  warn("%s", path);
  exit(1);
}
