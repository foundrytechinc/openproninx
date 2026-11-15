
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <fnu/sys.h>

static int quiet;

static int bytype(const char *name, struct fnu_sysctl_info *info) {
  int i;

  for (i = 0; fnu_sysctlnode(i, info) == 0; i++)
    if (strcmp(info->name, name) == 0)
      return 0;
  return -1;
}

static int show(const char *name, int type) {
  char text[256];
  size_t len = sizeof(text);
  int32_t i32;
  int64_t i64;

  switch (type) {
  case FNU_CTL_INT:
    len = sizeof(i32);
    if (sysctlbyname(name, &i32, &len, 0, 0) < 0)
      goto bad;
    if (quiet)
      printf("%d\n", i32);
    else
      printf("%s=%d\n", name, i32);
    return 0;
  case FNU_CTL_QUAD:
    len = sizeof(i64);
    if (sysctlbyname(name, &i64, &len, 0, 0) < 0)
      goto bad;
    if (quiet)
      printf("%lld\n", (long long)i64);
    else
      printf("%s=%lld\n", name, (long long)i64);
    return 0;
  case FNU_CTL_OPAQUE:
    if (sysctlbyname(name, 0, &len, 0, 0) < 0)
      goto bad;
    if (quiet)
      printf("%zu\n", len);
    else
      printf("%s: %zu bytes\n", name, len);
    return 0;
  default:
    if (sysctlbyname(name, text, &len, 0, 0) < 0)
      goto bad;
    if (quiet)
      printf("%s\n", text);
    else
      printf("%s=%s\n", name, text);
    return 0;
  }
bad:
  warn("%s", name);
  return 1;
}

static int set(const char *name, int type, const char *value) {
  char *end;
  int32_t i32;
  int64_t i64;
  int r;

  switch (type) {
  case FNU_CTL_OPAQUE:
    warnx("%s: not set as text", name);
    return 1;
  case FNU_CTL_INT:
    i32 = (int32_t)strtol(value, &end, 0);
    if (*value == 0 || *end != 0) {
      warnx("%s: %s is not a number", name, value);
      return 1;
    }
    r = sysctlbyname(name, 0, 0, &i32, sizeof(i32));
    break;
  case FNU_CTL_QUAD:
    i64 = strtoll(value, &end, 0);
    if (*value == 0 || *end != 0) {
      warnx("%s: %s is not a number", name, value);
      return 1;
    }
    r = sysctlbyname(name, 0, 0, &i64, sizeof(i64));
    break;
  default:
    r = sysctlbyname(name, 0, 0, value, strlen(value) + 1);
    break;
  }
  if (r < 0) {
    warn("%s", name);
    return 1;
  }
  if (!quiet)
    printf("%s=%s\n", name, value);
  return 0;
}

static int bypath(const void *a, const void *b) {
  return strcmp(((const struct fnu_sysctl_info *)a)->name,
                ((const struct fnu_sysctl_info *)b)->name);
}

static int list(const char *under) {
  struct fnu_sysctl_info *node = 0, *grown;
  int i, n = 0, cap = 0, status = 0, matched = 0;
  size_t ulen = under != 0 ? strlen(under) : 0;

  for (;;) {
    if (n == cap) {
      cap = cap ? cap * 2 : 256;
      if ((grown = reallocarray(node, (size_t)cap, sizeof(*node))) == 0)
        err(1, 0);
      node = grown;
    }
    if (fnu_sysctlnode(n, &node[n]) != 0)
      break;
    n++;
  }
  qsort(node, (size_t)n, sizeof(node[0]), bypath);
  for (i = 0; i < n; i++) {
    if (ulen != 0 && (strncmp(node[i].name, under, ulen) != 0 ||
                      (node[i].name[ulen] != 0 && node[i].name[ulen] != '.')))
      continue;
    matched = 1;
    if (node[i].type != FNU_CTL_OPAQUE || ulen == strlen(node[i].name))
      status |= show(node[i].name, (int)node[i].type);
  }
  free(node);
  return under != 0 && !matched ? 2 : status;
}

static int assign(char *arg) {
  struct fnu_sysctl_info info;
  char *eq = strchr(arg, '=');

  if (eq != 0)
    *eq = 0;
  if (bytype(arg, &info) < 0) {
    if (eq == 0 && list(arg) == 0)
      return 0;
    warnx("%s: unknown oid", arg);
    return 1;
  }
  return eq != 0 ? set(arg, (int)info.type, eq + 1) : show(arg, (int)info.type);
}

// one name=value a line, # to the end of a line ignored
static int from_file(const char *path) {
  char line[512], *p, *end;
  int status = 0, n = 0;
  FILE *fp;

  if ((fp = fopen(path, "r")) == 0) {
    warn("%s", path);
    return 1;
  }
  while (fgets(line, sizeof(line), fp) != 0) {
    n++;
    if ((p = strchr(line, '#')) != 0)
      *p = 0;
    for (p = line; *p == ' ' || *p == '\t'; p++)
      ;
    for (end = p + strlen(p); end > p && (end[-1] == '\n' || end[-1] == ' ' ||
                                          end[-1] == '\t' || end[-1] == '\r');)
      *--end = 0;
    if (*p == 0)
      continue;
    if (strchr(p, '=') == 0) {
      warnx("%s:%d: no value for %s", path, n, p);
      status = 1;
      continue;
    }
    status |= assign(p);
  }
  fclose(fp);
  return status;
}

int main(int argc, char **argv) {
  int ch, all = 0, status = 0;
  const char *file = 0;

  while ((ch = getopt(argc, argv, "af:nw")) != -1)
    switch (ch) {
    case 'a':
      all = 1;
      break;
    case 'f':
      file = optarg;
      break;
    case 'n':
      quiet = 1;
      break;
    case 'w':
      break;
    default:
      fprintf(stderr, "usage: sysctl [-n] [-a] [-f file] [name[=value] ...]\n");
      return 1;
    }
  argc -= optind;
  argv += optind;
  if (file != 0)
    return from_file(file);
  if (all || argc == 0)
    return list(0);
  for (; argc > 0; argc--, argv++)
    status |= assign(*argv);
  return status;
}
