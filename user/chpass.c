// chpass, chsh, chfn: an account's shell and name, by its owner or root
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <err.h>
#include <limits.h>
#include <util.h>

#define SHELLS "/etc/shells"

extern char *__progname;

static void usage(void) {
  dprintf(2, "usage: chsh [-s shell] [user]\n"
             "       chfn [-f name] [user]\n"
             "       chpass [-f name] [-s shell] [user]\n");
  exit(1);
}

// getusershell: what /etc/shells lists
static int listed_shell(const char *shell) {
  char line[PATH_MAX];
  FILE *fp;
  int found = 0;

  if ((fp = fopen(SHELLS, "r")) == 0)
    return 0;
  while (!found && fgets(line, sizeof(line), fp) != 0) {
    line[strcspn(line, "#\r\n \t")] = 0;
    found = line[0] == '/' && strcmp(line, shell) == 0;
  }
  fclose(fp);
  return found;
}

static void ask(const char *what, const char *now, char *out, size_t size) {
  printf("%s [%s]: ", what, now);
  fflush(stdout);
  if (fgets(out, (int)size, stdin) == 0)
    exit(1);
  out[strcspn(out, "\r\n")] = 0;
  if (out[0] == 0)
    strlcpy(out, now, size);
}

int main(int argc, char **argv) {
  char name[64], hash[256], gecos[256], dir[PATH_MAX], shell[PATH_MAX], class[64];
  const char *newshell = 0, *newname = 0;
  struct passwd *found, pw;
  uid_t me = getuid();
  int ch, doshell, doname;

  doshell = strcmp(__progname, "chfn") != 0;
  doname = strcmp(__progname, "chsh") != 0;
  while ((ch = getopt(argc, argv, "f:s:")) != -1)
    switch (ch) {
    case 'f':
      if (!doname)
        usage();
      newname = optarg;
      break;
    case 's':
      if (!doshell)
        usage();
      newshell = optarg;
      break;
    default:
      usage();
    }
  argc -= optind;
  argv += optind;
  if (argc > 1)
    usage();
  found = argc == 1 ? getpwnam(argv[0]) : getpwuid(me);
  if (found == 0)
    errx(1, "%s: no such account", argc == 1 ? argv[0] : "you");
  if (me != 0 && found->pw_uid != me)
    errx(1, "%s is not yours to change", found->pw_name);
  strlcpy(name, found->pw_name, sizeof(name));
  strlcpy(hash, found->pw_passwd, sizeof(hash));
  strlcpy(gecos, found->pw_gecos, sizeof(gecos));
  strlcpy(dir, found->pw_dir, sizeof(dir));
  strlcpy(shell, found->pw_shell, sizeof(shell));
  strlcpy(class, found->pw_class != 0 ? found->pw_class : "", sizeof(class));
  pw = *found;
  pw.pw_name = name;
  pw.pw_passwd = hash;
  pw.pw_gecos = gecos;
  pw.pw_dir = dir;
  pw.pw_shell = shell;
  pw.pw_class = class;

  if (doshell) {
    // a shell not in the list is root's to give or take
    if (me != 0 && shell[0] != 0 && !listed_shell(shell))
      errx(1, "your shell %s is not in %s; ask root", shell, SHELLS);
    if (newshell != 0)
      strlcpy(shell, newshell, sizeof(shell));
    else if (newname == 0)
      ask("Shell", pw.pw_shell, shell, sizeof(shell));
    if (me != 0 && !listed_shell(shell))
      errx(1, "%s is not in %s", shell, SHELLS);
    if (access(shell, X_OK) < 0)
      err(1, "%s", shell);
  }
  if (doname) {
    if (newname != 0)
      strlcpy(gecos, newname, sizeof(gecos));
    else if (newshell == 0)
      ask("Full name", found->pw_gecos, gecos, sizeof(gecos));
    if (strpbrk(gecos, ":\n") != 0)
      errx(1, "a name may not have : in it");
  }
  if (pwd_store(&pw) < 0)
    err(1, "%s", _PATH_MASTERPASSWD);
  syslog(LOG_AUTH | LOG_INFO, "%s changed by uid %u", name, (unsigned)me);
  return 0;
}
