#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fnu/sys.h>
#include <fnu/fstab.h>
#include <err.h>

static void usage(void) {
  dprintf(2, "usage: mount [-a] | [-frsvw] [-o options] [-t type] device directory\n"
             "       mount -u [-frsvw] [-o options] directory\n");
  exit(1);
}

static int options(const char *s, uint *flags) {
  char word[32];
  int n;

  while (*s != 0) {
    for (n = 0; *s != 0 && *s != ',' && n < (int)sizeof(word) - 1; s++)
      word[n++] = *s;
    word[n] = 0;
    while (*s == ',')
      s++;
    if (strcmp(word, "ro") == 0 || strcmp(word, "rdonly") == 0)
      *flags |= FNU_MNT_RDONLY;
    else if (strcmp(word, "rw") == 0)
      *flags &= ~FNU_MNT_RDONLY;
    else if (strcmp(word, "nosuid") == 0)
      *flags |= FNU_MNT_NOSUID;
    else if (strcmp(word, "suid") == 0)
      *flags &= ~FNU_MNT_NOSUID;
    else if (strcmp(word, "force") == 0)
      *flags |= FNU_MNT_FORCE;
    else if (strcmp(word, "auto") == 0 || strcmp(word, "noauto") == 0)
      ;
    else {
      dprintf(2, "mount: %s: not an option this system has\n", word);
      return -1;
    }
  }
  return 0;
}

static void show(void) {
  struct fnu_fsstat *table;
  int n, i;

  if ((n = fnu_volumes(&table)) < 0) {
    warnx("the mount table: %s", fnu_errstr(n));
    exit(1);
  }
  for (i = 0; i < n; i++)
    printf("%s on %s type %s%s%s\n", table[i].from, table[i].on,
           table[i].type,
           (table[i].flags & FNU_MNT_RDONLY) ? " (read-only)" : "",
           (table[i].flags & FNU_MNT_NOSUID) ? " (nosuid)" : "");
  fnu_table_free(table);
}

static void attach_all(void) {
  struct fstab_entry entry;
  struct fstab table;
  int e;

  if (fstab_open(&table) < 0) {
    warn("%s", FSTAB_PATH);
    return;
  }
  while (fstab_next(&table, &entry) == 0) {
    if (entry.when != FSTAB_AT_BOOT || strcmp(entry.request.on, "/") == 0 ||
        strcmp(entry.request.type, "swap") == 0)
      continue;
    if ((e = fnu_mount(&entry.request)) < 0)
      warnx("%s on %s: %s", entry.request.from, entry.request.on, fnu_errstr(e));
  }
  fstab_close(&table);
}

// what is mounted on a directory now
static int current(const char *on, struct fnu_fsstat *out) {
  struct fnu_fsstat *table;
  int n, i, found = -1;

  if ((n = fnu_volumes(&table)) < 0)
    return -1;
  for (i = 0; i < n; i++)
    if (strcmp(table[i].on, on) == 0) {
      *out = table[i];
      found = 0;
    }
  fnu_table_free(table);
  return found;
}

int main(int argc, char *argv[]) {
  struct fnu_mount_req request;
  struct fnu_fsstat now;
  struct fstab_entry entry;
  const char *opts = 0;
  int ch, all = 0, update = 0, verbose = 0, set = 0, clear = 0, e;

  memset(&request, 0, sizeof(request));
  while ((ch = getopt(argc, argv, "afo:rst:uvw")) != -1)
    switch (ch) {
    case 'a':
      all = 1;
      break;
    case 'f':
      set |= FNU_MNT_FORCE;
      break;
    case 'o':
      opts = optarg;
      break;
    case 'r':
      set |= FNU_MNT_RDONLY;
      clear &= ~FNU_MNT_RDONLY;
      break;
    case 's':
      set |= FNU_MNT_NOSUID;
      break;
    case 't':
      strlcpy(request.type, optarg, FNU_MNT_TYPELEN);
      break;
    case 'u':
      update = 1;
      break;
    case 'v':
      verbose = 1;
      break;
    case 'w':
      clear |= FNU_MNT_RDONLY;
      set &= ~FNU_MNT_RDONLY;
      break;
    default:
      usage();
    }
  argc -= optind;
  argv += optind;

  if (all) {
    attach_all();
    exit(0);
  }
  if (update) {
    if (argc != 1)
      usage();
    if (current(argv[0], &now) < 0)
      errx(1, "%s: not a mount point", argv[0]);
    request.flags = now.flags & (FNU_MNT_RDONLY | FNU_MNT_NOSUID);
    if (set == 0 && clear == 0 && opts == 0 &&
        fstab_by_mountpoint(argv[0], &entry) == 0)
      request.flags = entry.request.flags & (FNU_MNT_RDONLY | FNU_MNT_NOSUID);
    request.flags = (request.flags | (uint)set) & ~(uint)clear;
    if (opts != 0 && options(opts, &request.flags) < 0)
      exit(1);
    request.flags |= FNU_MNT_UPDATE;
    strlcpy(request.on, argv[0], FNU_MNT_NAMELEN);
    strlcpy(request.from, now.from, FNU_MNT_NAMELEN);
    if ((e = fnu_mount(&request)) < 0)
      errx(1, "%s: %s", argv[0], fnu_errstr(e));
    if (verbose)
      printf("%s on %s%s\n", now.from, argv[0],
             (request.flags & FNU_MNT_RDONLY) ? " (read-only)" : "");
    exit(0);
  }
  if (argc == 0) {
    show();
    exit(0);
  }
  if (argc != 2)
    usage();
  request.flags = (uint)set & ~(uint)clear;
  if (opts != 0 && options(opts, &request.flags) < 0)
    exit(1);
  strlcpy(request.from, argv[0], FNU_MNT_NAMELEN);
  strlcpy(request.on, argv[1], FNU_MNT_NAMELEN);
  if ((e = fnu_mount(&request)) < 0)
    errx(1, "%s on %s: %s", argv[0], argv[1], fnu_errstr(e));
  if (verbose)
    printf("%s on %s%s%s\n", argv[0], argv[1],
           request.type[0] != 0 ? " type " : "", request.type);
  exit(0);
}
