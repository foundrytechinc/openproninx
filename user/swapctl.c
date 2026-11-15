#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fnu/sys.h>
#include <fnu/fstab.h>
#include <err.h>

#define DEV_BSIZE 512

static long blocksize = DEV_BSIZE;
static const char *header = "512-blocks";

static void usage(void) {
  dprintf(2, "usage: swapctl -A | swapctl -a device | swapctl -d device\n");
  dprintf(2, "       swapctl -l [-k] | swapctl -s [-k]\n");
  exit(1);
}

static int swap_state(struct fnu_info *inf) {
  int r;

  if ((r = fnu_info(inf)) < 0) {
    warnx("swapinfo: %s", fnu_errstr(r));
    exit(1);
  }
  return inf->swap_total != 0;
}

static void list_long(void) {
  struct fnu_info inf;
  long size, used;

  if (!swap_state(&inf)) {
    dprintf(2, "swapctl: no swap devices configured\n");
    exit(1);
  }
  size = (long)(inf.swap_total / (uint64_t)blocksize);
  used = (long)(inf.swap_used / (uint64_t)blocksize);
  printf("%-11s %10s %8s %8s %8s\n", "Device", header, "Used", "Avail",
         "Capacity");
  printf("%-11s %10ld %8ld %8ld %7ld%%\n", "swap", size, used, size - used,
         size != 0 ? used * 100 / size : 0);
  printf("pages out %ld, pages in %ld\n", (long)inf.swap_outs,
         (long)inf.swap_ins);
}

static void list_short(void) {
  struct fnu_info inf;
  long size, used;

  if (!swap_state(&inf)) {
    printf("total: 0 %s allocated, 0 used, 0 available\n", header);
    return;
  }
  size = (long)(inf.swap_total / (uint64_t)blocksize);
  used = (long)(inf.swap_used / (uint64_t)blocksize);
  printf("total: %ld %s allocated, %ld used, %ld available\n", size, header,
         used, size - used);
}

// the swap lines of the filesystem table
static int add_all(void) {
  struct fstab_entry entry;
  struct fstab table;
  int r, status = 0;

  if (fstab_open(&table) < 0)
    return 0;
  while (fstab_next(&table, &entry) == 0) {
    if (entry.when != FSTAB_AT_BOOT || strcmp(entry.request.type, "swap") != 0)
      continue;
    if ((r = fnu_swapon(entry.request.from)) < 0) {
      warnx("%s: %s", entry.request.from, fnu_errstr(r));
      status = 1;
    }
  }
  fstab_close(&table);
  return status;
}

int main(int argc, char **argv) {
  int i = 1, cmd = 0, r;
  const char *device = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    const char *p;

    for (p = argv[i] + 1; *p != 0; p++) {
      if (*p == 'A' || *p == 'a' || *p == 'd' || *p == 'l' || *p == 's') {
        if (cmd != 0)
          usage();
        cmd = *p;
      } else if (*p == 'k') {
        blocksize = 1024;
        header = "1K-blocks";
      } else
        usage();
    }
  }
  if (cmd == 0)
    cmd = i == argc ? 'l' : 0;
  if (cmd == 'A') {
    if (i != argc)
      usage();
    exit(add_all());
  }
  if (cmd == 'a' || cmd == 'd') {
    if (i != argc - 1)
      usage();
    device = argv[i];
  } else if (i != argc)
    usage();

  switch (cmd) {
  case 'a':
    if ((r = fnu_swapon(device)) < 0) {
      warnx("%s: %s", device, fnu_errstr(r));
      exit(1);
    }
    break;
  case 'd':
    if ((r = fnu_swapoff()) < 0) {
      warnx("%s: %s", device, fnu_errstr(r));
      exit(1);
    }
    break;
  case 'l':
    list_long();
    break;
  case 's':
    list_short();
    break;
  default:
    usage();
  }
  exit(0);
}
