// shared by the account tools: users.conf, free ids, homes
#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <util.h>

#define USERS_CONF "/etc/users.conf"

static void trim(char **v) {
  char *end;

  while (**v == ' ' || **v == '\t')
    (*v)++;
  for (end = *v + strlen(*v); end > *v && (end[-1] == ' ' || end[-1] == '\t' ||
                                           end[-1] == '\n' || end[-1] == '\r');)
    *--end = 0;
}

static void range(const char *v, unsigned *lo, unsigned *hi) {
  char *end;
  unsigned long a = strtoul(v, &end, 10), b = a;

  if (*end == '-')
    b = strtoul(end + 1, &end, 10);
  if (*end == 0 && a > 0 && b >= a) {
    *lo = (unsigned)a;
    *hi = (unsigned)b;
  }
}

int users_conf(struct users_conf *c) {
  char line[512], *eq, *k, *v;
  FILE *fp;

  memset(c, 0, sizeof(*c));
  strlcpy(c->home, "/home", sizeof(c->home));
  strlcpy(c->skel, "/etc/skel", sizeof(c->skel));
  strlcpy(c->shell, "/usr/bin/sh", sizeof(c->shell));
  strlcpy(c->group, "private", sizeof(c->group));
  c->mode = 0700;
  c->uid_lo = c->gid_lo = 1000;
  c->uid_hi = c->gid_hi = 60000;
  if ((fp = fopen(USERS_CONF, "r")) == 0)
    return 0;
  while (fgets(line, sizeof(line), fp) != 0) {
    line[strcspn(line, "#")] = 0;
    if ((eq = strchr(line, '=')) == 0)
      continue;
    *eq = 0;
    k = line;
    v = eq + 1;
    trim(&k);
    trim(&v);
    if (strcmp(k, "home") == 0)
      strlcpy(c->home, v, sizeof(c->home));
    else if (strcmp(k, "skel") == 0)
      strlcpy(c->skel, v, sizeof(c->skel));
    else if (strcmp(k, "shell") == 0)
      strlcpy(c->shell, v, sizeof(c->shell));
    else if (strcmp(k, "group") == 0)
      strlcpy(c->group, v, sizeof(c->group));
    else if (strcmp(k, "mode") == 0)
      c->mode = (mode_t)strtoul(v, 0, 8) & 07777;
    else if (strcmp(k, "uid") == 0)
      range(v, &c->uid_lo, &c->uid_hi);
    else if (strcmp(k, "gid") == 0)
      range(v, &c->gid_lo, &c->gid_hi);
    else
      fprintf(stderr, "%s: %s is not a setting here\n", USERS_CONF, k);
  }
  fclose(fp);
  return 0;
}

uid_t users_free_uid(const struct users_conf *c, uid_t want) {
  uid_t u;

  if (want >= c->uid_lo && want <= c->uid_hi && getpwuid(want) == 0)
    return want;
  for (u = c->uid_lo; u <= c->uid_hi; u++)
    if (getpwuid(u) == 0)
      return u;
  return (uid_t)-1;
}

gid_t users_free_gid(const struct users_conf *c, gid_t want) {
  gid_t g;

  if (want >= c->gid_lo && want <= c->gid_hi && getgrgid(want) == 0)
    return want;
  for (g = c->gid_lo; g <= c->gid_hi; g++)
    if (getgrgid(g) == 0)
      return g;
  return (gid_t)-1;
}

static int copy_file(const char *from, const char *to, mode_t mode, uid_t uid, gid_t gid) {
  char buf[8192];
  ssize_t n;
  int in, out, r = 0;

  if ((in = open(from, O_RDONLY)) < 0)
    return -1;
  if ((out = open(to, O_WRONLY | O_CREAT | O_EXCL, mode & 0777)) < 0) {
    r = errno == EEXIST ? 0 : -1;
    close(in);
    return r;
  }
  while ((n = read(in, buf, sizeof(buf))) > 0)
    if (write(out, buf, (size_t)n) != n) {
      r = -1;
      break;
    }
  if (n < 0)
    r = -1;
  if (fchown(out, uid, gid) < 0)
    r = -1;
  close(in);
  close(out);
  return r;
}

// dot.profile in the skeleton is .profile in the home
int users_skel(const char *skel, const char *home, uid_t uid, gid_t gid) {
  char from[1024], to[1024];
  struct dirent *de;
  struct stat st;
  int r = 0;
  DIR *d;

  if ((d = opendir(skel)) == 0)
    return errno == ENOENT ? 0 : -1;
  while ((de = readdir(d)) != 0) {
    const char *name = de->d_name;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    snprintf(from, sizeof(from), "%s/%s", skel, name);
    if (strncmp(name, "dot.", 4) == 0)
      snprintf(to, sizeof(to), "%s/.%s", home, name + 4);
    else
      snprintf(to, sizeof(to), "%s/%s", home, name);
    if (lstat(from, &st) < 0) {
      r = -1;
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if ((mkdir(to, st.st_mode & 0777) < 0 && errno != EEXIST) ||
          chown(to, uid, gid) < 0 || users_skel(from, to, uid, gid) < 0)
        r = -1;
    } else if (S_ISREG(st.st_mode) && copy_file(from, to, st.st_mode, uid, gid) < 0)
      r = -1;
  }
  closedir(d);
  return r;
}

int users_mkhome(const struct users_conf *c, const char *home, const char *skel,
                 uid_t uid, gid_t gid) {
  char parent[1024], *slash;

  strlcpy(parent, home, sizeof(parent));
  if ((slash = strrchr(parent, '/')) != 0 && slash != parent) {
    *slash = 0;
    if (mkdir(parent, 0755) < 0 && errno != EEXIST)
      return -1;
  }
  if (mkdir(home, c->mode) < 0 && errno != EEXIST)
    return -1;
  if (chown(home, uid, gid) < 0 || chmod(home, c->mode) < 0)
    return -1;
  return users_skel(skel, home, uid, gid);
}

// rm -r, never across into another filesystem
int users_rmtree(const char *path) {
  char sub[1024];
  struct stat st, top;
  struct dirent *de;
  DIR *d;
  int r = 0;

  if (lstat(path, &top) < 0)
    return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(top.st_mode))
    return unlink(path);
  if ((d = opendir(path)) == 0)
    return -1;
  while ((de = readdir(d)) != 0) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
    if (lstat(sub, &st) < 0) {
      r = -1;
      continue;
    }
    if (S_ISDIR(st.st_mode) && st.st_dev != top.st_dev) {
      r = -1;
      continue;
    }
    if ((S_ISDIR(st.st_mode) ? users_rmtree(sub) : unlink(sub)) < 0)
      r = -1;
  }
  closedir(d);
  return rmdir(path) < 0 ? -1 : r;
}

// a,b,c into a list; the groups must exist
char **users_grouplist(const char *s) {
  char **list = 0, **more, *copy, *p, *next;
  int n = 0;

  if ((copy = strdup(s)) == 0)
    return 0;
  for (p = copy; p != 0 && *p != 0; p = next) {
    if ((next = strchr(p, ',')) != 0)
      *next++ = 0;
    if (*p == 0)
      continue;
    if (getgrnam(p) == 0) {
      fprintf(stderr, "no group %s\n", p);
      free(copy);
      free(list);
      return 0;
    }
    if ((more = reallocarray(list, (size_t)n + 2, sizeof(*list))) == 0) {
      free(copy);
      free(list);
      return 0;
    }
    list = more;
    list[n++] = p;
    list[n] = 0;
  }
  if (list == 0 && (list = calloc(1, sizeof(*list))) == 0)
    return 0;
  return list;
}

// a name a UNIX takes for an account or a group
int users_name_ok(const char *name) {
  size_t i, n = strlen(name);

  if (n == 0 || n > 31 || name[0] == '-')
    return 0;
  for (i = 0; i < n; i++)
    if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') ||
          name[i] == '_' || name[i] == '-' || name[i] == '.' ||
          (name[i] >= 'A' && name[i] <= 'Z')))
      return 0;
  return 1;
}
