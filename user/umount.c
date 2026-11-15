#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fnu/sys.h>
#include <err.h>

static unsigned int flags;
static int verbose, status;

static void usage(void) {
  dprintf(2, "usage: umount [-fv] directory | special ...\n");
  dprintf(2, "       umount -a [-fv] [-t type]\n");
  exit(1);
}

static void one(const char *arg) {
  struct fnu_fsstat *table;
  char where[FNU_MNT_NAMELEN];
  struct stat st;
  const char *on = arg;
  int n, i;

  if (stat(arg, &st) == 0 && S_ISBLK(st.st_mode)) {
    if ((n = fnu_volumes(&table)) < 0) {
      warnx("the mount table: %s", fnu_errstr(n));
      status = 1;
      return;
    }
    for (i = 0; i < n && table[i].dev != st.st_rdev; i++)
      ;
    if (i == n) {
      dprintf(2, "umount: %s is not mounted\n", arg);
      fnu_table_free(table);
      status = 1;
      return;
    }
    strlcpy(where, table[i].on, sizeof(where));
    fnu_table_free(table);
    on = where;
  }
  if (unmount(on, flags) < 0) {
    dprintf(2, "umount: %s is busy or not mounted\n", on);
    status = 1;
    return;
  }
  if (verbose)
    printf("%s unmounted\n", on);
}

// -a: the deepest first
static void all(const char *type) {
  struct fnu_fsstat *table;
  int n, i, k, deepest;
  char *done;

  if ((n = fnu_volumes(&table)) < 0) {
    warnx("the mount table: %s", fnu_errstr(n));
    exit(1);
  }
  if ((done = calloc((size_t)n + 1, 1)) == 0)
    err(1, "the mount table");
  for (k = 0; k < n; k++) {
    int best = -1, bestlen = -1;

    for (i = 0; i < n; i++) {
      int len = (int)strlen(table[i].on);

      if (done[i] || strcmp(table[i].on, "/") == 0)
        continue;
      if (type != 0 && strcmp(table[i].type, (char *)type) != 0)
        continue;
      if (len > bestlen) {
        bestlen = len;
        best = i;
      }
    }
    if (best < 0)
      break;
    deepest = best;
    done[deepest] = 1;
    one(table[deepest].on);
  }
  free(done);
  fnu_table_free(table);
}

int main(int argc, char *argv[]) {
  const char *type = 0;
  int i = 1, aflag = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    const char *p = argv[i] + 1;

    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *p != 0; p++) {
      if (*p == 'f')
        flags |= FNU_MNT_FORCE;
      else if (*p == 'v')
        verbose = 1;
      else if (*p == 'a')
        aflag = 1;
      else if (*p == 't') {
        type = p[1] != 0 ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (type == 0)
          usage();
        p += strlen(p) - 1;
      } else
        usage();
    }
  }
  if (aflag) {
    if (i != argc)
      usage();
    all(type);
    exit(status);
  }
  if (i >= argc)
    usage();
  for (; i < argc; i++)
    one(argv[i]);
  exit(status);
}
