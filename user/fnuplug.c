#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <fnu/sys.h>
#include <fnu/fstab.h>
#include <err.h>
#include <util.h>

#define MOUNT_ROOT "/mnt"

static void device_path(char *out, int size, const char *name) {
  strlcpy(out, "/dev/", size);
  strlcpy(out + strlen(out), (char *)name, size - (int)strlen(out));
}

static void spare_place(char *out, int size, const char *name) {
  strlcpy(out, MOUNT_ROOT "/", size);
  strlcpy(out + strlen(out), (char *)name, size - (int)strlen(out));
}

static int mounted_on(dev_t dev, char *on, int size) {
  struct fnu_fsstat *table;
  int n, i, r = -1;

  if ((n = fnu_volumes(&table)) < 0)
    return -1;
  for (i = 0; i < n && r < 0; i++)
    if (table[i].dev == dev) {
      if (on != 0)
        strlcpy(on, table[i].on, size);
      r = 0;
    }
  fnu_table_free(table);
  return r;
}

static void attach(const char *name, uint flags, dev_t dev) {
  struct fstab_entry entry;
  struct fnu_mount_req request;
  char from[FNU_MNT_NAMELEN];
  char on[FNU_MNT_NAMELEN];

  device_path(from, sizeof(from), name);

  if (mounted_on(dev, 0, 0) == 0)
    return;

  if (fstab_by_device(dev, &entry) == 0) {
    if (entry.when != FSTAB_AT_PLUG)
      return;
    if (mount(entry.request.type, entry.request.on, (int)entry.request.flags, entry.request.from) < 0)
      warn("%s on %s", entry.request.from,
              entry.request.on);
    return;
  }

  if (!(flags & FNU_HOTPLUG_REMOVABLE))
    return;

  spare_place(on, sizeof(on), name);
  mkdir(on, 0777);

  memset(&request, 0, sizeof(request));
  strlcpy(request.from, from, FNU_MNT_NAMELEN);
  strlcpy(request.on, on, FNU_MNT_NAMELEN);
  request.flags |= FNU_MNT_NOSUID;
  if (mount(request.type, request.on, (int)request.flags, request.from) < 0)
    rmdir(on);
}

static void detach(const char *name, dev_t dev) {
  char on[FNU_MNT_NAMELEN];
  char spare[FNU_MNT_NAMELEN];

  if (mounted_on(dev, on, sizeof(on)) < 0)
    return;
  if (unmount(on, FNU_MNT_FORCE) < 0)
    return;
  spare_place(spare, sizeof(spare), name);
  if (strcmp(on, spare) == 0)
    rmdir(on);
}

int main(int argc, char **argv __unused) {
  struct fnu_hotplug_event event;
  struct pidfh *pfh;
  pid_t other;
  int fd;

  if (argc != 1) {
    fprintf(stderr, "usage: fnuplug\n");
    exit(1);
  }
  if ((pfh = pidfile_open(0, 0600, &other)) == 0) {
    if (errno == EEXIST)
      errx(1, "already running, pid %d", (int)other);
    warn("cannot open or create pidfile");
  }
  if ((fd = open("/dev/hotplug", O_RDONLY)) < 0) {
    warn("/dev/hotplug");
    exit(1);
  }
  pidfile_write(pfh);

  for (;;) {
    if (read(fd, &event, sizeof(event)) != (ssize_t)sizeof(event))
      break;
    if (event.devclass != FNU_DEVCLASS_DISK)
      continue;
    if (event.type == FNU_HOTPLUG_ATTACH)
      attach(event.name, event.flags, event.dev);
    else if (event.type == FNU_HOTPLUG_DETACH)
      detach(event.name, event.dev);
  }
  close(fd);
  pidfile_remove(pfh);
  exit(0);
}
