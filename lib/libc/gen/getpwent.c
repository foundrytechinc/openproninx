// as getgrent.c reads the group file
#include <sys/types.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "namespace.h"
#include "pw_scan.h"

#define MAXLINELENGTH 1024

static FILE *_pw_fp;
static struct passwd _pw_passwd;
static char *line;
static int _pw_stayopen;
static int _pw_master;

static int start_pw(void) {
  int fd;

  if (_pw_fp != 0) {
    rewind(_pw_fp);
    return 1;
  }
  if (line == 0 && (line = malloc(MAXLINELENGTH)) == 0)
    return 0;
  _pw_master = 1;
  if ((fd = __pledge_open(_PATH_MASTERPASSWD, O_RDONLY | O_CLOEXEC)) < 0) {
    _pw_master = 0;
    fd = __pledge_open(_PATH_PASSWD, O_RDONLY | O_CLOEXEC);
  }
  if (fd >= 0 && (_pw_fp = fdopen(fd, "r")) == 0)
    close(fd);
  return _pw_fp != 0;
}

static int pwscan(int search, uid_t uid, const char *name) {
  char *cp;

  for (;;) {
    if (fgets(line, MAXLINELENGTH, _pw_fp) == 0)
      return 0;
    if ((cp = strchr(line, '\n')) != 0)
      *cp = '\0';
    else if (strlen(line) == MAXLINELENGTH - 1) {
      int c;

      while ((c = getc(_pw_fp)) != '\n' && c != EOF)
        ;
      continue;
    }
    for (cp = line; *cp == ' ' || *cp == '\t'; cp++)
      ;
    if (*cp == '#' || *cp == '\0')
      continue;
    if (!__pw_scan(line, &_pw_passwd, _pw_master ? _PWSCAN_MASTER : 0))
      continue;
    if (!search)
      return 1;
    if (name != 0) {
      if (strcmp(_pw_passwd.pw_name, name) == 0)
        return 1;
    } else if (_pw_passwd.pw_uid == uid)
      return 1;
  }
}

struct passwd *getpwent(void) {
  if ((_pw_fp == 0 && !start_pw()) || !pwscan(0, 0, 0))
    return 0;
  return &_pw_passwd;
}

struct passwd *getpwnam(const char *name) {
  int rval;

  if (!start_pw())
    return 0;
  rval = pwscan(1, 0, name);
  if (!_pw_stayopen)
    endpwent();
  return rval ? &_pw_passwd : 0;
}

struct passwd *getpwuid(uid_t uid) {
  int rval;

  if (!start_pw())
    return 0;
  rval = pwscan(1, uid, 0);
  if (!_pw_stayopen)
    endpwent();
  return rval ? &_pw_passwd : 0;
}

int setpassent(int stayopen) {
  if (!start_pw())
    return 0;
  _pw_stayopen = stayopen;
  return 1;
}

void setpwent(void) { (void)setpassent(0); }

void endpwent(void) {
  if (_pw_fp != 0) {
    fclose(_pw_fp);
    _pw_fp = 0;
  }
}

static int copy_r(struct passwd *pw, char *buf, size_t buflen,
                  struct passwd **result) {
  const char *src[6] = {_pw_passwd.pw_name, _pw_passwd.pw_passwd,
                        _pw_passwd.pw_class, _pw_passwd.pw_gecos,
                        _pw_passwd.pw_dir, _pw_passwd.pw_shell};
  char **dst[6] = {&pw->pw_name, &pw->pw_passwd, &pw->pw_class,
                   &pw->pw_gecos, &pw->pw_dir, &pw->pw_shell};
  size_t used = 0, n;
  int i;

  *pw = _pw_passwd;
  for (i = 0; i < 6; i++) {
    n = strlen(src[i]) + 1;
    if (used + n > buflen) {
      *result = 0;
      return ERANGE;
    }
    memcpy(buf + used, src[i], n);
    *dst[i] = buf + used;
    used += n;
  }
  *result = pw;
  return 0;
}

int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t buflen,
               struct passwd **result) {
  if (getpwnam(name) == 0) {
    *result = 0;
    return 0;
  }
  return copy_r(pw, buf, buflen, result);
}

int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t buflen,
               struct passwd **result) {
  if (getpwuid(uid) == 0) {
    *result = 0;
    return 0;
  }
  return copy_r(pw, buf, buflen, result);
}
