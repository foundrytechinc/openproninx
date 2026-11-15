// groupadd, groupdel, groupmod: the groups file, one group at a time
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <err.h>
#include <util.h>

extern char *__progname;

static void usage(void) {
  if (strcmp(__progname, "groupdel") == 0)
    dprintf(2, "usage: groupdel name\n");
  else if (strcmp(__progname, "groupmod") == 0)
    dprintf(2, "usage: groupmod [-o] [-g gid] [-n newname] name\n");
  else
    dprintf(2, "usage: groupadd [-o] [-g gid] name\n");
  exit(1);
}

static int gid_of(const char *s, gid_t *out) {
  char *end;
  unsigned long n = strtoul(s, &end, 10);

  if (*s == 0 || *end != 0 || n > 2147483647UL)
    return -1;
  *out = (gid_t)n;
  return 0;
}

static int add(int argc, char **argv) {
  struct users_conf conf;
  struct group g;
  char *none[] = {0};
  gid_t gid = (gid_t)-1;
  int ch, dup_ok = 0;

  while ((ch = getopt(argc, argv, "g:o")) != -1)
    switch (ch) {
    case 'g':
      if (gid_of(optarg, &gid) < 0)
        errx(1, "%s: not a gid", optarg);
      break;
    case 'o':
      dup_ok = 1;
      break;
    default:
      usage();
    }
  argc -= optind;
  argv += optind;
  if (argc != 1)
    usage();
  if (!users_name_ok(argv[0]))
    errx(1, "%s: not a name a group can have", argv[0]);
  if (getgrnam(argv[0]) != 0)
    errx(1, "%s already exists", argv[0]);
  users_conf(&conf);
  if (gid == (gid_t)-1) {
    if ((gid = users_free_gid(&conf, (gid_t)-1)) == (gid_t)-1)
      errx(1, "no gid free between %u and %u", conf.gid_lo, conf.gid_hi);
  } else if (!dup_ok && getgrgid(gid) != 0)
    errx(1, "gid %u is already in use", (unsigned)gid);
  g.gr_name = argv[0];
  g.gr_passwd = "*";
  g.gr_gid = gid;
  g.gr_mem = none;
  if (grp_store(&g) < 0)
    err(1, "%s", _PATH_GROUP);
  syslog(LOG_AUTH | LOG_NOTICE, "new group %s, gid %u", argv[0], (unsigned)gid);
  printf("%s: gid %u\n", argv[0], (unsigned)gid);
  return 0;
}

static int del(int argc, char **argv) {
  struct passwd *pw;

  if (argc != 2)
    usage();
  if (getgrnam(argv[1]) == 0)
    errx(1, "%s: no such group", argv[1]);
  setpwent();
  while ((pw = getpwent()) != 0)
    if (pw->pw_gid == getgrnam(argv[1])->gr_gid)
      errx(1, "%s is the group of %s; change that first", argv[1], pw->pw_name);
  endpwent();
  if (grp_remove(argv[1]) < 0)
    err(1, "%s", _PATH_GROUP);
  syslog(LOG_AUTH | LOG_NOTICE, "group %s removed", argv[1]);
  return 0;
}

static int mod(int argc, char **argv) {
  struct group *found, g;
  char name[64], **mem;
  gid_t gid = (gid_t)-1;
  const char *newname = 0;
  int ch, dup_ok = 0, n;

  while ((ch = getopt(argc, argv, "g:n:o")) != -1)
    switch (ch) {
    case 'g':
      if (gid_of(optarg, &gid) < 0)
        errx(1, "%s: not a gid", optarg);
      break;
    case 'n':
      newname = optarg;
      break;
    case 'o':
      dup_ok = 1;
      break;
    default:
      usage();
    }
  argc -= optind;
  argv += optind;
  if (argc != 1)
    usage();
  if ((found = getgrnam(argv[0])) == 0)
    errx(1, "%s: no such group", argv[0]);
  strlcpy(name, found->gr_name, sizeof(name));
  for (n = 0; found->gr_mem[n] != 0; n++)
    ;
  if ((mem = calloc((size_t)n + 1, sizeof(*mem))) == 0)
    err(1, 0);
  for (n = 0; found->gr_mem[n] != 0; n++)
    if ((mem[n] = strdup(found->gr_mem[n])) == 0)
      err(1, 0);
  g.gr_name = name;
  g.gr_passwd = "*";
  g.gr_gid = found->gr_gid;
  g.gr_mem = mem;
  if (newname != 0) {
    if (!users_name_ok(newname) || getgrnam(newname) != 0)
      errx(1, "%s: taken, or not a name", newname);
    g.gr_name = (char *)(uintptr_t)newname;
  }
  if (gid != (gid_t)-1) {
    if (!dup_ok && getgrgid(gid) != 0 && gid != g.gr_gid)
      errx(1, "gid %u is already in use", (unsigned)gid);
    g.gr_gid = gid;
  }
  if (grp_replace(name, &g) < 0)
    err(1, "%s", _PATH_GROUP);
  syslog(LOG_AUTH | LOG_NOTICE, "group %s changed", name);
  return 0;
}

int main(int argc, char **argv) {
  if (getuid() != 0)
    errx(1, "only root may change the groups");
  if (strcmp(__progname, "groupdel") == 0)
    return del(argc, argv);
  if (strcmp(__progname, "groupmod") == 0)
    return mod(argc, argv);
  return add(argc, argv);
}
