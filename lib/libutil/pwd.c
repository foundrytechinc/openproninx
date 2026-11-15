// the account files, as the tools change them
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <fnu/sys.h>
#include <fnu/crypt.h>
#include <util.h>
#include <errno.h>
#include "pw_scan.h"

#define GROUP_TMP "/etc/gtmp"
#define LOCK_TRIES 10

int pwd_ask_new(char *out, int size) {
  char again[CRYPT_PASSWORD_MAX];
  int tries = -1;

  for (;;) {
    if (read_password("New password: ", out, size) < 0 || out[0] == 0) {
      printf("Password unchanged.\n");
      return -1;
    }
    if (++tries < CRYPT_PASSWORD_TRIES &&
        (int)strlen(out) < CRYPT_PASSWORD_MIN) {
      printf("Please enter a longer password, at least %d characters.\n",
             CRYPT_PASSWORD_MIN);
      continue;
    }
    if (read_password("Retype new password: ", again, sizeof(again)) == 0 &&
        strcmp(out, again) == 0) {
      memset(again, 0, sizeof(again));
      return 0;
    }
    printf("Mismatch; try again, EOF to quit.\n");
    memset(again, 0, sizeof(again));
    memset(out, 0, size);
  }
}

// the temp file is the lock
static FILE *lock_open(const char *path, mode_t mode) {
  struct timespec pause = {1, 0};
  mode_t mask;
  FILE *fp;
  int fd, i;

  for (i = 0; i < LOCK_TRIES; i++) {
    mask = umask(077);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    umask(mask);
    if (fd >= 0) {
      if ((fp = fdopen(fd, "w")) == 0) {
        close(fd);
        unlink(path);
      }
      return fp;
    }
    if (errno != EEXIST)
      return 0;
    nanosleep(&pause, 0);
  }
  errno = EBUSY;
  return 0;
}

// the new file lands before the old name goes: a crash leaves one of them
static int put_away(FILE *fp, const char *tmp, const char *path) {
  int bad = fflush(fp) != 0 || ferror(fp) || fsync(fileno(fp)) < 0;

  if (fclose(fp) != 0 || bad)
    return -1;
  return rename(tmp, path);
}

static void put_passwd(FILE *fp, const struct passwd *e, int master) {
  if (master)
    fprintf(fp, "%s:%s:%u:%u:%s:%ld:%ld:%s:%s:%s\n", e->pw_name, e->pw_passwd,
            e->pw_uid, e->pw_gid, e->pw_class ? e->pw_class : "",
            (long)e->pw_change, (long)e->pw_expire, e->pw_gecos, e->pw_dir,
            e->pw_shell);
  else
    fprintf(fp, "%s:*:%u:%u:%s:%s:%s\n", e->pw_name, e->pw_uid, e->pw_gid,
            e->pw_gecos, e->pw_dir, e->pw_shell);
}

// both files from master, one entry changed; lock last
int pwd_replace(const char *name, const struct passwd *entry) {
  char *line = 0, *copy = 0;
  size_t room = 0;
  struct passwd other;
  FILE *in, *master, *pub;
  int done = 0, fd;

  if (name == 0 || *name == 0 || (in = fopen(_PATH_MASTERPASSWD, "r")) == 0)
    return -1;
  if ((master = lock_open(_PATH_MASTERPASSWD_TMP, 0600)) == 0) {
    fclose(in);
    return -1;
  }
  if ((fd = open(_PATH_PASSWD_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0 ||
      (pub = fdopen(fd, "w")) == 0) {
    if (fd >= 0)
      close(fd);
    goto fail;
  }
  fchmod(fd, 0644);
  while (getline(&line, &room, in) > 0) {
    char *p = line;

    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '#' || *p == '\n' || *p == 0) {
      fputs(line, master);
      continue;
    }
    free(copy);
    if ((copy = strdup(line)) == 0) {
      fclose(pub);
      goto fail;
    }
    copy[strcspn(copy, "\n")] = 0;
    if (!__pw_scan(copy, &other, _PWSCAN_MASTER)) {
      fputs(line, master);
      continue;
    }
    if (strcmp(other.pw_name, name) == 0) {
      done = 1;
      if (entry == 0)
        continue;
      put_passwd(master, entry, 1);
      put_passwd(pub, entry, 0);
      continue;
    }
    put_passwd(master, &other, 1);
    put_passwd(pub, &other, 0);
  }
  if (!done && entry != 0) {
    put_passwd(master, entry, 1);
    put_passwd(pub, entry, 0);
    done = 1;
  }
  if (ferror(in)) {
    fclose(pub);
    goto fail;
  }
  free(line);
  free(copy);
  fclose(in);
  in = 0;
  if (put_away(pub, _PATH_PASSWD_TMP, _PATH_PASSWD) < 0) {
    unlink(_PATH_PASSWD_TMP);
    fclose(master);
    unlink(_PATH_MASTERPASSWD_TMP);
    return -1;
  }
  if (put_away(master, _PATH_MASTERPASSWD_TMP, _PATH_MASTERPASSWD) < 0) {
    unlink(_PATH_MASTERPASSWD_TMP);
    return -1;
  }
  if (!done)
    errno = ENOENT;
  return done ? 0 : -1;
fail:
  free(line);
  free(copy);
  if (in != 0)
    fclose(in);
  fclose(master);
  unlink(_PATH_PASSWD_TMP);
  unlink(_PATH_MASTERPASSWD_TMP);
  return -1;
}

int pwd_store(const struct passwd *entry) {
  if (entry == 0 || entry->pw_name == 0 || entry->pw_name[0] == 0)
    return -1;
  return pwd_replace(entry->pw_name, entry);
}

int pwd_remove(const char *name) { return pwd_replace(name, 0); }

// GROUPS

static void put_group(FILE *fp, const struct group *g) {
  int i;

  fprintf(fp, "%s:%s:%u:", g->gr_name, g->gr_passwd != 0 ? g->gr_passwd : "*",
          (unsigned)g->gr_gid);
  for (i = 0; g->gr_mem != 0 && g->gr_mem[i] != 0; i++)
    fprintf(fp, "%s%s", i ? "," : "", g->gr_mem[i]);
  fputc('\n', fp);
}

// name:passwd:gid:a,b,c, cut in place; mem is made to fit
static int scan_group(char *line, struct group *g, char ***mem) {
  char *f[4], *p = line, *m;
  int i, n = 0;

  for (i = 0; i < 4; i++) {
    f[i] = p;
    if (i < 3) {
      if ((p = strchr(p, ':')) == 0)
        return 0;
      *p++ = 0;
    }
  }
  f[3][strcspn(f[3], "\n")] = 0;
  for (i = 2, p = f[3]; *p != 0; p++)
    i += *p == ',';
  free(*mem);
  if ((*mem = calloc((size_t)i, sizeof(**mem))) == 0)
    return -1;
  g->gr_name = f[0];
  g->gr_passwd = f[1];
  g->gr_gid = (gid_t)strtoul(f[2], 0, 10);
  for (p = f[3]; *p != 0; p = m) {
    if ((m = strchr(p, ',')) != 0)
      *m++ = 0;
    else
      m = p + strlen(p);
    if (*p != 0)
      (*mem)[n++] = p;
  }
  (*mem)[n] = 0;
  g->gr_mem = *mem;
  return 1;
}

typedef int (*group_edit)(struct group *g, const void *arg, FILE *out);

static int group_rewrite(group_edit edit, const void *arg) {
  char *line = 0, *copy = 0, **mem = 0;
  size_t room = 0;
  struct group g;
  FILE *in, *out;
  int r = 0, s;

  if ((in = fopen(_PATH_GROUP, "r")) == 0)
    return -1;
  if ((out = lock_open(GROUP_TMP, 0644)) == 0) {
    fclose(in);
    return -1;
  }
  fchmod(fileno(out), 0644);
  while (r == 0 && getline(&line, &room, in) > 0) {
    char *p = line;

    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '#' || *p == '\n' || *p == 0) {
      fputs(line, out);
      continue;
    }
    free(copy);
    if ((copy = strdup(line)) == 0 || (s = scan_group(copy, &g, &mem)) < 0) {
      r = -1;
      break;
    }
    if (s == 0)
      fputs(line, out);
    else
      r = edit(&g, arg, out);
  }
  if (r == 0 && ferror(in))
    r = -1;
  if (r == 0)
    r = edit(0, arg, out);
  free(line);
  free(copy);
  free(mem);
  fclose(in);
  if (r < 0) {
    fclose(out);
    unlink(GROUP_TMP);
    return -1;
  }
  if (put_away(out, GROUP_TMP, _PATH_GROUP) < 0) {
    unlink(GROUP_TMP);
    return -1;
  }
  return 0;
}

struct replace_arg {
  const char *name;
  const struct group *with;
  int done;
};

static int replace_edit(struct group *g, const void *arg, FILE *out) {
  struct replace_arg *a = (struct replace_arg *)(uintptr_t)arg;

  if (g == 0) {
    if (!a->done && a->with != 0)
      put_group(out, a->with);
    return 0;
  }
  if (strcmp(g->gr_name, a->name) == 0) {
    a->done = 1;
    if (a->with != 0)
      put_group(out, a->with);
    return 0;
  }
  put_group(out, g);
  return 0;
}

int grp_replace(const char *name, const struct group *entry) {
  struct replace_arg a = {name, entry, 0};

  if (name == 0 || *name == 0 || group_rewrite(replace_edit, &a) < 0)
    return -1;
  if (!a.done && entry == 0) {
    errno = ENOENT;
    return -1;
  }
  return 0;
}

int grp_store(const struct group *entry) { return grp_replace(entry->gr_name, entry); }

int grp_remove(const char *name) { return grp_replace(name, 0); }

struct member_arg {
  const char *user, *rename;
  const char *const *groups;
};

static int listed(const char *const *list, const char *name) {
  int i;

  for (i = 0; list != 0 && list[i] != 0; i++)
    if (strcmp(list[i], name) == 0)
      return 1;
  return 0;
}

// ???
static int member_edit(struct group *g, const void *arg, FILE *out) {
  const struct member_arg *a = arg;
  char **mem, **old;
  int i, n = 0, want;

  if (g == 0)
    return 0;
  for (i = 0; g->gr_mem[i] != 0; i++)
    ;
  if ((mem = calloc((size_t)i + 2, sizeof(*mem))) == 0)
    return -1;
  want = a->groups != 0 && listed(a->groups, g->gr_name);
  for (i = 0; g->gr_mem[i] != 0; i++) {
    if (strcmp(g->gr_mem[i], a->user) == 0) {
      if (a->groups != 0 && !want)
        continue;
      mem[n++] = (char *)(uintptr_t)(a->rename != 0 ? a->rename : a->user);
      want = 0;
      continue;
    }
    mem[n++] = g->gr_mem[i];
  }
  if (want)
    mem[n++] = (char *)(uintptr_t)(a->rename != 0 ? a->rename : a->user);
  mem[n] = 0;
  old = g->gr_mem;
  g->gr_mem = mem;
  put_group(out, g);
  g->gr_mem = old;
  free(mem);
  return 0;
}

int grp_set_member(const char *user, const char *const *groups) {
  struct member_arg a = {user, 0, groups};

  return group_rewrite(member_edit, &a);
}

int grp_rename_member(const char *user, const char *rename) {
  struct member_arg a = {user, rename, 0};

  return group_rewrite(member_edit, &a);
}
