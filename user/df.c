#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>
#include <fnu/sys.h>

static int human, inodes, local_only;
static char *types;
static int types_excluded;

static void usage(void) {
  fprintf(stderr, "usage: df [-hikl] [-t type[,type...]] [file ...]\n");
  exit(1);
}

static int type_wanted(const char *type) {
  const char *p = types, *e;
  size_t n = strlen(type);

  if (types == 0)
    return 1;
  while (*p != 0) {
    for (e = p; *e != 0 && *e != ','; e++)
      ;
    if ((size_t)(e - p) == n && strncmp(p, type, n) == 0)
      return !types_excluded;
    p = *e != 0 ? e + 1 : e;
  }
  return types_excluded;
}

static void amount(uint64_t bytes, int width) {
  char buf[5];

  if (human) {
    humanize_number(buf, sizeof(buf), (int64_t)bytes, "", HN_AUTOSCALE,
                    HN_B | HN_NOSPACE | HN_DECIMAL);
    printf(" %*s", width, buf);
  } else
    printf(" %*llu", width, (unsigned long long)(bytes / 1024));
}

static int percent(uint64_t used, uint64_t avail) {
  uint64_t all = used + avail;

  return all == 0 ? 0 : (int)((used * 100 + all - 1) / all);
}

static void show(const struct fnu_fsstat *v, int width) {
  uint64_t used = v->blocks - v->bfree;
  uint64_t avail = v->bavail > 0 ? (uint64_t)v->bavail : 0;

  printf("%-*s", width, v->from);
  amount(v->blocks * v->bsize, human ? 6 : 10);
  amount(used * v->bsize, human ? 6 : 10);
  amount(avail * v->bsize, human ? 6 : 10);
  printf(" %7d%%", percent(used, avail));
  if (inodes) {
    uint64_t iused = v->files - v->ffree;

    printf(" %8llu %8llu %5d%%", (unsigned long long)iused,
           (unsigned long long)v->ffree, percent(iused, v->ffree));
  }
  printf("  %s%s\n", v->on, (v->flags & FNU_MNT_RDONLY) ? " (ro)" : "");
}

static int by_mount(const void *a, const void *b) {
  return strcmp(((const struct fnu_fsstat *)a)->on,
                ((const struct fnu_fsstat *)b)->on);
}

int main(int argc, char **argv) {
  struct fnu_fsstat *v;
  struct fnu_stat st;
  int n, i, j, ch, width = (int)strlen("Filesystem"), status = 0;
  char *shown;

  while ((ch = getopt(argc, argv, "hiklt:")) != -1)
    switch (ch) {
    case 'h': human = 1; break;
    case 'i': inodes = 1; break;
    case 'k': human = 0; break;
    case 'l': local_only = 1; break;
    case 't':
      if (types != 0)
        errx(1, "only one -t");
      types_excluded = strncmp(optarg, "no", 2) == 0;
      types = types_excluded ? optarg + 2 : optarg;
      break;
    default: usage();
    }
  argc -= optind;
  argv += optind;

  if ((n = fnu_volumes(&v)) < 0)
    errx(1, "the volumes: %s", fnu_errstr(n));
  qsort(v, (size_t)n, sizeof(*v), by_mount);
  if ((shown = calloc((size_t)n + 1, 1)) == 0)
    err(1, 0);
  for (i = 0; i < n; i++)
    shown[i] = type_wanted(v[i].type) &&
               (!local_only || (v[i].flags & FNU_MNT_LOCAL) != 0);
  if (argc > 0) {
    memset(shown, 0, (size_t)n);
    for (j = 0; j < argc; j++) {
      int r = fnu_getattr(FNU_FD_CWD, argv[j], 0, &st), found = 0;

      if (r < 0) {
        warnx("%s: %s", argv[j], fnu_errstr(r));
        status = 1;
        continue;
      }
      for (i = 0; i < n; i++)
        if (v[i].dev == st.device) {
          shown[i] = 1;
          found = 1;
        }
      if (!found) {
        warnx("%s: not on a mounted volume", argv[j]);
        status = 1;
      }
    }
  }
  for (i = 0; i < n; i++)
    if (shown[i] && (int)strlen(v[i].from) > width)
      width = (int)strlen(v[i].from);

  printf("%-*s %*s %*s %*s %8s", width, "Filesystem", human ? 6 : 10,
         human ? "Size" : "1K-blocks", human ? 6 : 10, "Used", human ? 6 : 10,
         "Avail", "Capacity");
  if (inodes)
    printf(" %8s %8s %6s", "iused", "ifree", "%iused");
  printf("  Mounted on\n");
  for (i = 0; i < n; i++)
    if (shown[i])
      show(&v[i], width);
  free(shown);
  fnu_table_free(v);
  return status;
}
