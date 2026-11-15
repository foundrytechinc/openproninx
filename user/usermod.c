// usermod: an account changed in place
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <err.h>
#include <limits.h>
#include <util.h>

static void usage(void) {
  dprintf(2, "usage: usermod [-aLmoU] [-c comment] [-d home] [-g group] [-G group,...]\n"
             "               [-l newname] [-p hash] [-s shell] [-u uid] name\n");
  exit(1);
}

// the secondary groups it is in now, then those added
static char **current_groups(const char *user, char **add) {
  char **list = 0;
  struct group *g;
  int n = 0, i;

  setgrent();
  while ((g = getgrent()) != 0)
    for (i = 0; g->gr_mem != 0 && g->gr_mem[i] != 0; i++)
      if (strcmp(g->gr_mem[i], user) == 0) {
        if ((list = reallocarray(list, (size_t)n + 2, sizeof(*list))) == 0 ||
            (list[n] = strdup(g->gr_name)) == 0)
          err(1, 0);
        list[++n] = 0;
        break;
      }
  endgrent();
  for (i = 0; add != 0 && add[i] != 0; i++) {
    if ((list = reallocarray(list, (size_t)n + 2, sizeof(*list))) == 0)
      err(1, 0);
    list[n++] = add[i];
    list[n] = 0;
  }
  return list;
}

int main(int argc, char **argv) {
  struct users_conf conf;
  struct passwd *found, pw;
  struct group *g;
  char name[64], passwd[256], gecos[256], dir[PATH_MAX], shell[PATH_MAX], class[64];
  char locked[260], hashbuf[256];
  const char *comment = 0, *home = 0, *group = 0, *more = 0, *newname = 0;
  const char *hash = 0, *newshell = 0;
  char **groups = 0, **set;
  unsigned long uid = 0;
  int ch, append = 0, lock = 0, unlock = 0, move = 0, dup_ok = 0, have_uid = 0;
  char *end;

  users_conf(&conf);
  while ((ch = getopt(argc, argv, "ac:d:g:G:l:Lmop:s:u:U")) != -1)
    switch (ch) {
    case 'a':
      append = 1;
      break;
    case 'c':
      comment = optarg;
      break;
    case 'd':
      home = optarg;
      break;
    case 'g':
      group = optarg;
      break;
    case 'G':
      more = optarg;
      break;
    case 'l':
      newname = optarg;
      break;
    case 'L':
      lock = 1;
      break;
    case 'm':
      move = 1;
      break;
    case 'o':
      dup_ok = 1;
      break;
    case 'p':
      hash = optarg;
      break;
    case 's':
      newshell = optarg;
      break;
    case 'u':
      uid = strtoul(optarg, &end, 10);
      if (*optarg == 0 || *end != 0)
        errx(1, "%s: not a uid", optarg);
      have_uid = 1;
      break;
    case 'U':
      unlock = 1;
      break;
    default:
      usage();
    }
  argc -= optind;
  argv += optind;
  if (argc != 1 || (lock && unlock) || (append && more == 0))
    usage();
  if (getuid() != 0)
    errx(1, "only root may change an account");
  if ((found = getpwnam(argv[0])) == 0)
    errx(1, "%s: no such account", argv[0]);
  strlcpy(name, found->pw_name, sizeof(name));
  strlcpy(passwd, found->pw_passwd, sizeof(passwd));
  strlcpy(gecos, found->pw_gecos, sizeof(gecos));
  strlcpy(dir, found->pw_dir, sizeof(dir));
  strlcpy(shell, found->pw_shell, sizeof(shell));
  strlcpy(class, found->pw_class != 0 ? found->pw_class : "", sizeof(class));
  pw = *found;
  pw.pw_name = name;
  pw.pw_passwd = passwd;
  pw.pw_gecos = gecos;
  pw.pw_dir = dir;
  pw.pw_shell = shell;
  pw.pw_class = class;

  if (newname != 0) {
    if (!users_name_ok(newname))
      errx(1, "%s: not a name an account can have", newname);
    if (getpwnam(newname) != 0)
      errx(1, "%s already exists", newname);
  }
  if (have_uid) {
    if (!dup_ok && getpwuid((uid_t)uid) != 0 && uid != pw.pw_uid)
      errx(1, "uid %lu is already in use", uid);
    pw.pw_uid = (uid_t)uid;
  }
  if (group != 0) {
    if ((g = getgrnam(group)) != 0)
      pw.pw_gid = g->gr_gid;
    else {
      unsigned long n = strtoul(group, &end, 10);

      if (*group == 0 || *end != 0 || getgrgid((gid_t)n) == 0)
        errx(1, "%s: no such group", group);
      pw.pw_gid = (gid_t)n;
    }
  }
  if (more != 0 && (groups = users_grouplist(more)) == 0)
    exit(1);
  if (comment != 0)
    strlcpy(gecos, comment, sizeof(gecos));
  if (newshell != 0)
    strlcpy(shell, newshell, sizeof(shell));
  if (hash != 0)
    strlcpy(passwd, hash, sizeof(passwd));
  if (lock && passwd[0] != '*') {
    snprintf(locked, sizeof(locked), "*%s", passwd);
    strlcpy(passwd, locked, sizeof(passwd));
  }
  if (unlock && passwd[0] == '*' && passwd[1] != 0) {
    strlcpy(hashbuf, passwd + 1, sizeof(hashbuf));
    strlcpy(passwd, hashbuf, sizeof(passwd));
  }
  if (home != 0) {
    if (move && rename(dir, home) < 0)
      err(1, "%s to %s", dir, home);
    strlcpy(dir, home, sizeof(dir));
  }
  if (newname != 0)
    pw.pw_name = (char *)(uintptr_t)newname;

  if (pwd_replace(name, &pw) < 0)
    err(1, "%s", _PATH_MASTERPASSWD);
  syslog(LOG_AUTH | LOG_NOTICE, "account %s changed%s%s", name, newname != 0 ? " to " : "",
         newname != 0 ? newname : "");
  set = groups != 0 ? (append ? current_groups(name, groups) : groups) : 0;
  if (set != 0 && grp_set_member(name, (const char *const *)set) < 0)
    warn("%s", _PATH_GROUP);
  if (newname != 0) {
    if (grp_rename_member(name, newname) < 0)
      warn("%s", _PATH_GROUP);
    // its own group follows its name
    if ((g = getgrnam(name)) != 0 && g->gr_gid == pw.pw_gid) {
      struct group renamed = *g;

      renamed.gr_name = (char *)(uintptr_t)newname;
      if (grp_replace(name, &renamed) < 0)
        warn("%s", _PATH_GROUP);
    }
  }
  if (have_uid && home == 0 && chown(dir, pw.pw_uid, (gid_t)-1) < 0 && errno != ENOENT)
    warn("%s", dir);
  return 0;
}
