// useradd: an account, its group and its home; adduser asks for a password
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <fnu/crypt.h>
#include <err.h>
#include <limits.h>
#include <util.h>

extern char *__progname;

static void usage(void) {
  dprintf(2, "usage: %s [-mMo] [-c comment] [-d home] [-g group] [-G group,...]\n"
             "       %*s [-k skel] [-p hash] [-s shell] [-u uid] name\n"
             "       %s -D\n",
          __progname, (int)strlen(__progname), "", __progname);
  exit(1);
}

static void defaults(const struct users_conf *c) {
  printf("home=%s\nskel=%s\nshell=%s\ngroup=%s\nmode=%04o\nuid=%u-%u\ngid=%u-%u\n",
         c->home, c->skel, c->shell, c->group, (unsigned)c->mode, c->uid_lo, c->uid_hi,
         c->gid_lo, c->gid_hi);
}

static int number(const char *s, unsigned *out) {
  char *end;
  unsigned long n = strtoul(s, &end, 10);

  if (*s == 0 || *end != 0 || n > 2147483647UL)
    return -1;
  *out = (unsigned)n;
  return 0;
}

int main(int argc, char **argv) {
  struct users_conf conf;
  struct passwd account;
  struct group private, *gr;
  char home[PATH_MAX], pref[16], password[CRYPT_PASSWORD_MAX], hash[_PASSWORD_LEN];
  const char *comment = 0, *dir = 0, *group = 0, *more = 0, *skel = 0, *given = 0;
  const char *shell = 0;
  char **groups = 0, *nomem[] = {0};
  unsigned uid = 0, gid;
  int ch, mkhome = 0, nohome = 0, dup_ok = 0, have_uid = 0, show = 0, ask;

  ask = strcmp(__progname, "adduser") == 0;
  users_conf(&conf);
  while ((ch = getopt(argc, argv, "c:d:Dg:G:k:mMop:s:u:")) != -1)
    switch (ch) {
    case 'c':
      comment = optarg;
      break;
    case 'd':
      dir = optarg;
      break;
    case 'D':
      show = 1;
      break;
    case 'g':
      group = optarg;
      break;
    case 'G':
      more = optarg;
      break;
    case 'k':
      skel = optarg;
      break;
    case 'm':
      mkhome = 1;
      break;
    case 'M':
      nohome = 1;
      break;
    case 'o':
      dup_ok = 1;
      break;
    case 'p':
      given = optarg;
      break;
    case 's':
      shell = optarg;
      break;
    case 'u':
      if (number(optarg, &uid) < 0)
        errx(1, "%s: not a uid", optarg);
      have_uid = 1;
      break;
    default:
      usage();
    }
  argc -= optind;
  argv += optind;
  if (show) {
    if (argc != 0)
      usage();
    defaults(&conf);
    return 0;
  }
  if (argc != 1)
    usage();
  if (ask)
    mkhome = !nohome;
  if (getuid() != 0)
    errx(1, "only root may add an account");
  if (!users_name_ok(argv[0]))
    errx(1, "%s: not a name an account can have", argv[0]);
  if (getpwnam(argv[0]) != 0)
    errx(1, "%s already exists", argv[0]);
  if (have_uid) {
    if (!dup_ok && getpwuid(uid) != 0)
      errx(1, "uid %u is already in use", uid);
  } else if ((uid = users_free_uid(&conf, (uid_t)-1)) == (unsigned)-1)
    errx(1, "no uid free between %u and %u", conf.uid_lo, conf.uid_hi);
  if (more != 0 && (groups = users_grouplist(more)) == 0)
    exit(1);

  // the primary group: named, or one of its own
  if (group == 0 && strcmp(conf.group, "private") != 0)
    group = conf.group;
  memset(&private, 0, sizeof(private));
  if (group != 0) {
    if ((gr = getgrnam(group)) != 0)
      gid = gr->gr_gid;
    else if (number(group, &gid) < 0 || getgrgid(gid) == 0)
      errx(1, "%s: no such group", group);
  } else {
    if (getgrnam(argv[0]) != 0)
      errx(1, "a group %s exists already; name it with -g", argv[0]);
    if ((gid = users_free_gid(&conf, uid)) == (unsigned)-1)
      errx(1, "no gid free between %u and %u", conf.gid_lo, conf.gid_hi);
    private.gr_name = argv[0];
    private.gr_passwd = "*";
    private.gr_gid = gid;
    private.gr_mem = nomem;
  }

  memset(&account, 0, sizeof(account));
  account.pw_name = argv[0];
  account.pw_class = "";
  account.pw_uid = uid;
  account.pw_gid = gid;
  account.pw_gecos = (char *)(comment != 0 ? comment : "");
  if (dir != 0)
    strlcpy(home, dir, sizeof(home));
  else
    snprintf(home, sizeof(home), "%s/%s", conf.home, argv[0]);
  account.pw_dir = home;
  account.pw_shell = (char *)(shell != 0 ? shell : conf.shell);
  account.pw_passwd = (char *)(given != 0 ? given : "*");
  if (ask) {
    if (pwd_ask_new(password, sizeof(password)) == 0) {
      snprintf(pref, sizeof(pref), "bcrypt,%d", CRYPT_COST);
      if (crypt_newhash(password, pref, hash, sizeof(hash)) < 0)
        errx(1, "cannot hash it");
      account.pw_passwd = hash;
    }
    explicit_bzero(password, sizeof(password));
  }

  if (private.gr_name != 0 && grp_store(&private) < 0)
    err(1, "%s", _PATH_GROUP);
  if (pwd_store(&account) < 0) {
    warn("%s", _PATH_MASTERPASSWD);
    if (private.gr_name != 0)
      grp_remove(private.gr_name);
    exit(1);
  }
  if (groups != 0 && groups[0] != 0 &&
      grp_set_member(argv[0], (const char *const *)groups) < 0)
    warn("%s", _PATH_GROUP);
  if (mkhome && users_mkhome(&conf, home, skel != 0 ? skel : conf.skel, uid, gid) < 0)
    warn("%s", home);
  syslog(LOG_AUTH | LOG_NOTICE, "new account %s, uid %u, gid %u, home %s", argv[0], uid, gid,
         home);
  printf("%s: uid %u, gid %u, home %s%s\n", argv[0], uid, gid, home,
         strcmp(account.pw_passwd, "*") == 0 ? "; locked until passwd sets a password"
                                             : "");
  return 0;
}
