// userdel: an account out of the files, its own group with it, -r its home
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <err.h>
#include <limits.h>
#include <util.h>

static void usage(void) {
  dprintf(2, "usage: userdel [-r] name\n");
  exit(1);
}

static int others_in(const struct group *g, const char *user) {
  int i;

  for (i = 0; g->gr_mem != 0 && g->gr_mem[i] != 0; i++)
    if (strcmp(g->gr_mem[i], user) != 0)
      return 1;
  return 0;
}

int main(int argc, char **argv) {
  char home[PATH_MAX], name[64], *none[] = {0};
  struct passwd *pw, *other;
  struct group *g;
  gid_t gid;
  uid_t uid;
  int ch, rmhome = 0, shared = 0;

  while ((ch = getopt(argc, argv, "r")) != -1)
    switch (ch) {
    case 'r':
      rmhome = 1;
      break;
    default:
      usage();
    }
  argc -= optind;
  argv += optind;
  if (argc != 1)
    usage();
  if (getuid() != 0)
    errx(1, "only root may remove an account");
  if ((pw = getpwnam(argv[0])) == 0)
    errx(1, "%s: no such account", argv[0]);
  if (pw->pw_uid == 0)
    errx(1, "%s is the superuser's account", argv[0]);
  strlcpy(name, pw->pw_name, sizeof(name));
  strlcpy(home, pw->pw_dir, sizeof(home));
  gid = pw->pw_gid;
  uid = pw->pw_uid;

  if (grp_set_member(name, (const char *const *)none) < 0)
    warn("%s", _PATH_GROUP);
  if (pwd_remove(name) < 0)
    err(1, "%s", _PATH_MASTERPASSWD);
  // its own group goes when nobody else is in it
  if ((g = getgrgid(gid)) != 0 && strcmp(g->gr_name, name) == 0 && !others_in(g, name)) {
    setpwent();
    while ((other = getpwent()) != 0)
      if (other->pw_gid == gid)
        shared = 1;
    endpwent();
    if (!shared && grp_remove(name) < 0)
      warn("%s", _PATH_GROUP);
  }
  if (rmhome && home[0] != 0 && strcmp(home, "/") != 0 && users_rmtree(home) < 0)
    warn("%s", home);
  syslog(LOG_AUTH | LOG_NOTICE, "account %s (uid %u) removed", name, (unsigned)uid);
  printf("%s (uid %u) removed%s\n", name, (unsigned)uid, rmhome ? ", and its home" : "");
  return 0;
}
