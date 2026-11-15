
#include <err.h>
#include <sys/wait.h>
#include <fnu/sys.h>
#include <fnu/fstab.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UFS2_SBLOCK 65536
#define UFS2_MAGIC_AT 1372

static void usage(void) {
  dprintf(2, "usage: fsck [-dfnpvy] [-t type] [device | mount point]\n");
  exit(1);
}

static int is_fat(const unsigned char *b) {
  unsigned int bps = b[11] | b[12] << 8, fat16 = b[22] | b[23] << 8;

  return b[510] == 0x55 && b[511] == 0xAA && bps == 512 && b[13] != 0 &&
         ((fat16 == 0 && (b[66] == 0x29 || b[66] == 0x28)) ||
          (fat16 != 0 && (b[38] == 0x29 || b[38] == 0x28)));
}

static const char *probe(const char *path) {
  unsigned char b[512];
  int fd, ufs = 0, fat = 0;

  if ((fd = open(path, O_RDONLY)) < 0)
    err(1, "%s", path);
  if (pread(fd, b, sizeof(b), 0) == (ssize_t)sizeof(b))
    fat = is_fat(b);
  if (pread(fd, b, 4, UFS2_SBLOCK + UFS2_MAGIC_AT) == 4)
    ufs = b[0] == 0x19 && b[1] == 0x01 && b[2] == 0x54 && b[3] == 0x19;
  close(fd);
  return ufs ? "ffs" : fat ? "msdos" : 0;
}

static const char *checker(const char *dev, const char *type) {
  if (type == 0 || *type == 0)
    type = probe(dev);
  if (type == 0)
    return 0;
  if (strcmp(type, "ufs2") == 0 || strcmp(type, "ffs") == 0)
    return "fsck_ffs";
  if (strcmp(type, "fat32") == 0 || strcmp(type, "fat") == 0 ||
      strcmp(type, "msdos") == 0)
    return "fsck_msdos";
  return 0;
}

static int mounted_rw(const char *on) {
  struct fnu_fsstat *table;
  int n, i, rw = 0;

  if ((n = fnu_volumes(&table)) < 0)
    return 0;
  for (i = 0; i < n; i++)
    if (strcmp(table[i].on, on) == 0 && !(table[i].flags & FNU_MNT_RDONLY))
      rw = 1;
  fnu_table_free(table);
  return rw;
}

// a mount point names its volume: the mounted one, else the table's
static const char *resolve(const char *name, const char **type) {
  static char from[FNU_MNT_NAMELEN], kind[FNU_MNT_TYPELEN];
  struct fnu_fsstat *table;
  struct fstab_entry entry;
  int n, i, found = 0;

  if ((n = fnu_volumes(&table)) >= 0) {
    for (i = 0; i < n && !found; i++)
      if (strcmp(table[i].on, name) == 0) {
        strlcpy(from, table[i].from, sizeof(from));
        strlcpy(kind, table[i].type, sizeof(kind));
        found = 1;
      }
    fnu_table_free(table);
  }
  if (!found && fstab_by_mountpoint(name, &entry) == 0) {
    strlcpy(from, entry.request.from, sizeof(from));
    strlcpy(kind, entry.request.type, sizeof(kind));
    found = 1;
  }
  if (!found)
    return name;
  if (*type == 0 && kind[0] != 0)
    *type = kind;
  return from;
}

// no device: every boot fs
static int check_all(char **flags, int nflags) {
  struct fstab_entry entry;
  struct fstab table;
  const char *prog;
  char **args;
  int status = 0, e, i;
  pid_t pid;

  if (fstab_open(&table) < 0)
    err(8, "%s", FSTAB_PATH);
  if ((args = calloc((size_t)nflags + 3, sizeof(*args))) == 0)
    err(8, 0);
  while (fstab_next(&table, &entry) == 0) {
    if (entry.when != FSTAB_AT_BOOT || strcmp(entry.request.type, "swap") == 0 ||
        mounted_rw(entry.request.on))
      continue;
    if ((prog = checker(entry.request.from, entry.request.type)) == 0) {
      warnx("%s: no checker for it", entry.request.from);
      continue;
    }
    args[0] = (char *)prog;
    for (i = 0; i < nflags; i++)
      args[i + 1] = flags[i];
    args[nflags + 1] = entry.request.from;
    args[nflags + 2] = 0;
    if ((pid = fork()) == 0) {
      execvp(prog, args);
      err(8, "%s", prog);
    }
    if (pid < 0 || waitpid(pid, &e, 0) != pid) {
      status = 8;
      continue;
    }
    e = WIFEXITED(e) ? WEXITSTATUS(e) : 8;
    if (e > status)
      status = e;
  }
  fstab_close(&table);
  free(args);
  return status;
}

int main(int argc, char **argv) {
  const char *type = 0, *dev;
  char prog[32], **args;
  int i, n = 0;

  if ((args = calloc((size_t)argc + 1, sizeof(*args))) == 0)
    err(1, 0);
  args[n++] = prog;
  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    if (strcmp(argv[i], "-t") == 0) {
      if (++i >= argc)
        usage();
      type = argv[i];
    } else if (strncmp(argv[i], "-t", 2) == 0)
      type = argv[i] + 2;
    else
      args[n++] = argv[i];
  }
  if (i == argc)
    exit(check_all(args + 1, n - 1));
  if (i != argc - 1)
    usage();
  dev = resolve(argv[i], &type);
  args[n++] = (char *)dev;
  if (type == 0 && (type = probe(dev)) == 0)
    errx(1, "%s carries no filesystem this knows", argv[i]);
  if (strcmp(type, "ufs2") == 0)
    type = "ffs";
  if (strcmp(type, "fat32") == 0 || strcmp(type, "fat") == 0)
    type = "msdos";
  snprintf(prog, sizeof(prog), "fsck_%s", type);
  execvp(prog, args);
  err(1, "%s", prog);
}
