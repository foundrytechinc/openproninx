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

#define DOAS_CONF "/etc/doas.conf"
#define DOAS_LINE 256

struct rule {
  int permit;
  int nopass;
  char identity[LOGIN_NAME_MAX];
  char target[LOGIN_NAME_MAX];
  char command[PATH_MAX];
  int matched;
};

static const char *word(const char *p, char *out, int size) {
  int n = 0;

  while (*p == ' ' || *p == '\t')
    p++;
  if (*p == 0)
    return 0;
  while (*p != 0 && *p != ' ' && *p != '\t') {
    if (n < size - 1)
      out[n++] = *p;
    p++;
  }
  out[n] = 0;
  return p;
}

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

static int identity_matches(const char *identity, const struct passwd *who) {
  if (identity[0] == ':')
    return in_group(identity + 1, who->pw_name, who->pw_gid);
  return strcmp((char *)identity, (char *)who->pw_name) == 0;
}

// the last rule that matches
static int policy(const struct passwd *who, const char *command,
                  struct rule *out) {
  char line[DOAS_LINE], token[PATH_MAX];
  struct rule rule;
  const char *p;
  int fd, found = 0, n, i;
  char buf[512];
  int len = 0, pos = 0;

  if ((fd = open(DOAS_CONF, O_RDONLY)) < 0)
    return -1;

  for (;;) {
    n = 0;
    for (;;) {
      if (pos == len) {
        len = (int)read(fd, buf, sizeof(buf));
        pos = 0;
        if (len <= 0)
          break;
      }
      if (buf[pos] == '\n') {
        pos++;
        break;
      }
      if (buf[pos] != '\r' && n < (int)sizeof(line) - 1)
        line[n++] = buf[pos];
      pos++;
    }
    line[n] = 0;
    if (n == 0 && len <= 0)
      break;

    for (i = 0; line[i] == ' ' || line[i] == '\t'; i++)
      ;
    if (line[i] == 0 || line[i] == '#')
      continue;

    memset(&rule, 0, sizeof(rule));
    p = word(line, token, sizeof(token));
    if (p == 0)
      continue;
    if (strcmp(token, "permit") == 0)
      rule.permit = 1;
    else if (strcmp(token, "deny") == 0)
      rule.permit = 0;
    else
      continue;

    for (;;) {
      p = word(p, token, sizeof(token));
      if (p == 0)
        break;
      if (strcmp(token, "nopass") == 0)
        rule.nopass = 1;
      else
        break;
    }
    if (p == 0 || token[0] == 0)
      continue;
    strlcpy(rule.identity, token, sizeof(rule.identity));

    while ((p = word(p, token, sizeof(token))) != 0) {
      if (strcmp(token, "as") == 0) {
        if ((p = word(p, token, sizeof(token))) == 0)
          break;
        strlcpy(rule.target, token, sizeof(rule.target));
      } else if (strcmp(token, "cmd") == 0) {
        if ((p = word(p, token, sizeof(token))) == 0)
          break;
        strlcpy(rule.command, token, sizeof(rule.command));
      }
    }

    if (!identity_matches(rule.identity, who))
      continue;
    if (rule.command[0] != 0 && strcmp(rule.command, (char *)command) != 0)
      continue;
    *out = rule;
    found = 1;
  }
  close(fd);
  return found ? 0 : -1;
}

static void usage(void) {
  dprintf(2, "usage: doas [-ns] [-u user] command [argument ...]\n");
  exit(1);
}

int main(int argc, char **argv) {
  char password[CRYPT_PASSWORD_MAX];
  char path[PATH_MAX], termbuf[64], prompt[64 + LOGIN_NAME_MAX], whobuf[1024];
  char *shargv[2];
  struct passwd who, target, *found;
  struct rule rule;
  const char *asuser = 0, *term;
  int i = 1, nflag = 0, sflag = 0;
  uint me;

  for (; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    const char *p = argv[i] + 1;

    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *p != 0; p++) {
      if (*p == 'n')
        nflag = 1;
      else if (*p == 's')
        sflag = 1;
      else if (*p == 'u') {
        asuser = p[1] != 0 ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (asuser == 0)
          usage();
        p += strlen(p) - 1;
      } else
        usage();
    }
  }
  if (sflag ? i != argc : i >= argc)
    usage();
  if (sflag) {
    const char *sh = getenv("SHELL");

    shargv[0] = (char *)(sh != 0 && *sh != 0 ? sh : "/usr/bin/sh");
    shargv[1] = 0;
    argv = shargv;
    argc = 1;
    i = 0;
  }

  me = (uint)getuid();
  if (getpwuid_r(me, &who, whobuf, sizeof(whobuf), &found) != 0 || found == 0) {
    dprintf(2, "doas: no account owns uid %d\n", me);
    exit(1);
  }

  if (policy(&who, argv[i], &rule) < 0 || !rule.permit) {
    syslog(LOG_AUTH | LOG_NOTICE, "command not permitted for %s: %s", who.pw_name, argv[i]);
    dprintf(2, "doas: %s is not permitted to run %s\n", who.pw_name, argv[i]);
    exit(1);
  }
  if (asuser != 0) {
    if (rule.target[0] != 0 && strcmp(rule.target, (char *)asuser) != 0) {
      dprintf(2, "doas: %s is not permitted to run it as %s\n", who.pw_name,
              asuser);
      exit(1);
    }
    strlcpy(rule.target, (char *)asuser, sizeof(rule.target));
  }

  if (!rule.nopass && me != 0) {
    if (nflag) {
      dprintf(2, "doas: a password is required\n");
      exit(1);
    }
    snprintf(prompt, sizeof(prompt), "doas (%s) password: ", who.pw_name);
    if (read_password(prompt, password, sizeof(password)) < 0 ||
        crypt_checkpass(password, who.pw_passwd) != 0) {
      memset(password, 0, sizeof(password));
      syslog(LOG_AUTH | LOG_NOTICE, "failed auth for %s", who.pw_name);
      dprintf(2, "doas: incorrect\n");
      exit(1);
    }
    memset(password, 0, sizeof(password));
  }

  syslog(LOG_AUTH | LOG_INFO, "%s ran command %s as %s", who.pw_name, argv[i],
         rule.target[0] != 0 ? rule.target : "root");
  memset(&target, 0, sizeof(target));
  if (rule.target[0] != 0) {
    if ((found = getpwnam(rule.target)) == 0) {
      dprintf(2, "doas: no account called %s\n", rule.target);
      exit(1);
    }
    target = *found;
  } else if ((found = getpwnam("root")) != 0)
    target = *found;
  else {
    target.pw_name = "root";
    target.pw_dir = "/";
    target.pw_shell = "";
  }

  if (argv[i][0] == '/') {
    strlcpy(path, argv[i], sizeof(path));
  } else {
    strlcpy(path, "/usr/bin/", sizeof(path));
    strlcpy(path + strlen(path), argv[i], sizeof(path) - (int)strlen(path));
  }

  // groups before the uid
  if (initgroups(target.pw_name, target.pw_gid) < 0)
    warn("setgroups");
  if (setgid(target.pw_gid) < 0 || setuid(target.pw_uid) < 0) {
    warn("%s",
            rule.target[0] != 0 ? rule.target : "root");
    exit(1);
  }
  term = getenv("TERM");
  if (term != 0) {
    strlcpy(termbuf, term, sizeof(termbuf));
    term = termbuf;
  }
  environ = 0;
  setenv("DOAS_USER", who.pw_name, 1);
  setenv("USER", target.pw_name, 1);
  setenv("LOGNAME", target.pw_name, 1);
  setenv("HOME", target.pw_dir[0] != 0 ? target.pw_dir : "/", 1);
  setenv("SHELL", target.pw_shell[0] != 0 ? target.pw_shell : "/usr/bin/sh", 1);
  setenv("PATH", "/usr/bin:/bin", 1);
  setenv("TERM", term != 0 ? term : "vt100", 1);
  execv(path, argv + i);
  warn("%s", path);
  exit(1);
}
