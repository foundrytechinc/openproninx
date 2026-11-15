// volume discovery
#include "blockdev.h"
#include "defs.h"
#include "file.h"
#include "fs.h"
#include "param.h"
#include "ufs2.h"
#include "vfs.h"
#include "buf.h"
#include "fat32.h"
#include "devfs.h"
#include "inc/stat.h"
#include "hibernate.h"

// THE ROOT
#define ROOT_SPEC 128
#define ROOT_ASKS 100

static struct mutex root_holds_mtx = MUTEX_INITIALIZER("rootholds");
static struct root_hold *root_holds;
static uint root_published;
static int root_mount_timeout = 3;

void root_mount_hold(struct root_hold *h, const char *who) {
  mtx_enter(&root_holds_mtx);
  if (!h->held) {
    h->who = who;
    h->held = 1;
    h->next = root_holds;
    root_holds = h;
  }
  mtx_leave(&root_holds_mtx);
}

void root_mount_rel(struct root_hold *h) {
  struct root_hold **pp;

  mtx_enter(&root_holds_mtx);
  for (pp = &root_holds; *pp != 0; pp = &(*pp)->next)
    if (*pp == h) {
      *pp = h->next;
      h->held = 0;
      wakeup(&root_holds);
      break;
    }
  mtx_leave(&root_holds_mtx);
}

void root_mount_published(void) {
  mtx_enter(&root_holds_mtx);
  root_published++;
  wakeup(&root_holds);
  mtx_leave(&root_holds_mtx);
}

static void root_holds_wait(void) {
  struct root_hold *h;
  char who[64];
  uint64_t said = 0;
  int n, first = 1;

  mtx_enter(&root_holds_mtx);
  while (root_holds != 0) {
    if (first || ticks - said >= HZ) {
      first = 0;
      said = ticks;
      for (n = 0, h = root_holds; h != 0 && n < (int)sizeof(who) - 1; h = h->next)
        n += snprintf(who + n, sizeof(who) - n, " %s", h->who);
      mtx_leave(&root_holds_mtx);
      printf("ROOT: waiting for%s\n", who);
      mtx_enter(&root_holds_mtx);
      continue;
    }
    msleep(&root_holds, &root_holds_mtx, PRIBIO, "roothold", HZ);
  }
  mtx_leave(&root_holds_mtx);
}

static int root_await(const char *what, int (*find)(const char *, uint *),
                      const char *arg, uint *dev) {
  uint64_t start;
  uint seen;

  blockdev_publish_all();
  if (find(arg, dev) == 0)
    return 0;
  root_holds_wait();
  if (find(arg, dev) == 0)
    return 0;
  printf("ROOT: waiting for device %s\n", what);
  start = ticks;
  mtx_enter(&root_holds_mtx);
  for (;;) {
    seen = root_published;
    mtx_leave(&root_holds_mtx);
    blockdev_publish_all();
    if (find(arg, dev) == 0)
      return 0;
    mtx_enter(&root_holds_mtx);
    if (ticks - start >= (uint64_t)root_mount_timeout * HZ)
      break;
    if (root_published == seen)
      msleep(&root_holds, &root_holds_mtx, PRIBIO, "rmdev",
             (int)((uint64_t)root_mount_timeout * HZ - (ticks - start)));
  }
  mtx_leave(&root_holds_mtx);
  return -1;
}

static int by_path(const char *path, uint *dev) {
  struct devfs_node node;
  char name[DEVFS_NAME_MAX + 1];
  uint dir = DEVFS_ROOT_INO;
  int n;

  if (strncmp(path, "/dev/", 5) == 0)
    path += 5;
  node.type = 0;
  while (*path != 0) {
    for (n = 0; path[n] != 0 && path[n] != '/'; n++)
      ;
    if (n == 0 || n > DEVFS_NAME_MAX)
      return -1;
    memmove(name, path, n);
    name[n] = 0;
    if (devfs_lookup(dir, name, &node) < 0)
      return -1;
    dir = node.inum;
    for (path += n; *path == '/'; path++)
      ;
  }
  if (node.type != FNU_TYPE_DISK)
    return -1;
  *dev = makedev((uint)node.major, (uint)node.minor);
  return 0;
}

static int by_partuuid(const char *uuid, uint *dev) {
  return (*dev = partition_by_uuid(uuid)) != 0 ? 0 : -1;
}

static int one_label(uint *dev) {
  int n;

  blockdev_publish_all();
  root_holds_wait();
  if ((*dev = partition_labelled("fnu-root", &n)) == 0)
    return -1;
  if (n == 1)
    return 0;
  printf("ROOT: %d partitions are called fnu-root, and nothing says which\n", n);
  return 1;
}

static int root_options(const char *opts, int *ro, int *nosuid) {
  char word[16];
  int n;

  *ro = 0;
  *nosuid = 0;
  while (opts != 0 && *opts != 0) {
    for (n = 0; opts[n] != 0 && opts[n] != ','; n++)
      ;
    if (n >= (int)sizeof(word)) {
      printf("ROOT: no such mount option %s\n", opts);
      return -1;
    }
    memmove(word, opts, n);
    word[n] = 0;
    if (strncmp(word, "ro", sizeof(word)) == 0)
      *ro = 1;
    else if (strncmp(word, "rw", sizeof(word)) == 0)
      *ro = 0;
    else if (strncmp(word, "nosuid", sizeof(word)) == 0)
      *nosuid = 1;
    else if (strncmp(word, "suid", sizeof(word)) == 0)
      *nosuid = 0;
    else if (n != 0) {
      printf("ROOT: no such mount option %s\n", word);
      return -1;
    }
    opts += n;
    if (*opts == ',')
      opts++;
  }
  return 0;
}

static int root_on(uint dev, const char *type, int ro, int nosuid) {
  if (getenv_flag("boot_single"))
    ro = 1;
  if (fs_mountroot(dev, type, ro, nosuid) == 0)
    return 0;
  printf("ROOT: %s (%d,%d) carries no %s filesystem\n",
          blockdev_driver_name(dev), major(dev), minor(dev),
          type[0] != 0 ? type : "known");
  return -1;
}

static int root_try(const char *spec, int ro, int nosuid) {
  char type[VFS_TYPELEN], what[ROOT_SPEC];
  const char *colon;
  uint dev;
  int n;

  for (colon = spec; *colon != 0 && *colon != ':'; colon++)
    ;
  n = (int)(colon - spec);
  if (*colon != ':' || n == 0 || n >= (int)sizeof(type) ||
      strlen(colon + 1) >= sizeof(what) || colon[1] == 0) {
    printf("ROOT: %s is not type:device\n", spec);
    return -1;
  }
  memmove(type, spec, n);
  type[n] = 0;
  strlcpy(what, colon + 1, sizeof(what));
  if (strncmp(type, "auto", sizeof(type)) == 0)
    type[0] = 0;
  else if (strncmp(type, "ufs2", sizeof(type)) != 0 &&
           strncmp(type, "fat32", sizeof(type)) != 0) {
    printf("ROOT: no filesystem called %s\n", type);
    return -1;
  }
  if (root_await(what, by_path, what, &dev) < 0) {
    printf("ROOT: no device %s\n", what);
    return -1;
  }
  return root_on(dev, type, ro, nosuid);
}

static int root_list(const char *list, int ro, int nosuid) {
  char spec[VFS_TYPELEN + ROOT_SPEC + 1];
  int n;

  while (*list != 0) {
    for (n = 0; list[n] != 0 && list[n] != ';'; n++)
      ;
    if (n < (int)sizeof(spec)) {
      memmove(spec, list, n);
      spec[n] = 0;
      if (root_try(spec, ro, nosuid) == 0)
        return 0;
    }
    list += n;
    if (*list == ';')
      list++;
  }
  return -1;
}

static int root_bootdisk(const char *uuid, uint *dev) {
  uint part, disk;

  if (root_await(uuid, by_partuuid, uuid, &part) < 0) {
    printf("ROOT: the boot partition %s is not here\n", uuid);
    return -1;
  }
  if ((disk = partition_disk(part)) == 0 || (*dev = partition_root(disk)) == 0) {
    printf("ROOT: no root partition on the boot disk\n");
    return -1;
  }
  return 0;
}

static void root_list_disks(void) {
  struct devfs_node node;
  char name[DEVFS_NAME_MAX + 1];
  uint i;

  printf("disks:");
  for (i = 0; devfs_nth(DEVFS_ROOT_INO, i, &node) == 0; i++)
    if (node.type == FNU_TYPE_DISK &&
        blockdev_name(makedev((uint)node.major, (uint)node.minor), name,
                      sizeof(name)) == 0)
      printf(" %s", name);
  printf("\n");
}

static void root_ask(void) {
  char line[ROOT_SPEC + VFS_TYPELEN];
  int i;

  printf("\nManual root filesystem specification:\n"
          "  <fstype>:<device>  ufs2, fat32 or auto, e.g. ufs2:/dev/sd0p3\n"
          "  ?                  the disks there are\n"
          "  panic              stop here\n");
  for (i = 0; i < ROOT_ASKS; i++) {
    printf("\nmountroot> ");
    cngets(line, sizeof(line));
    if (line[0] == 0)
      continue;
    if (strncmp(line, "?", sizeof(line)) == 0) {
      blockdev_publish_all();
      root_list_disks();
    } else if (strncmp(line, "panic", sizeof(line)) == 0) {
      break;
    } else if (root_try(line, 0, 0) == 0) {
      return;
    }
  }
  panic("no root filesystem");
}

uint storage_swap_device(void) {
  const char *uuid = kgetenv("boot.partuuid");
  uint part, disk;
  int n;

  if (uuid != 0)
    return (part = partition_by_uuid(uuid)) != 0 &&
                   (disk = partition_disk(part)) != 0
               ? partition_swap(disk)
               : 0;
  part = partition_labelled("fnu-swap", &n);
  return n == 1 ? part : 0;
}

void storage_mountroot(uint ramdisk) {
  const char *from = kgetenv("vfs.root.mountfrom");
  const char *uuid = kgetenv("boot.partuuid");
  int ro, nosuid, found;
  uint dev, part;

  if (getenv_int("vfs.mountroot.timeout", &found) && found >= 0)
    root_mount_timeout = found;
  if (uuid != 0)
    root_await(uuid, by_partuuid, uuid, &part);
  hibernate_resume(storage_swap_device());
  if (from != 0) {
    if (root_options(kgetenv("vfs.root.mountfrom.options"), &ro, &nosuid) == 0 &&
        root_list(from, ro, nosuid) == 0)
      return;
  } else {
    found = uuid != 0 ? root_bootdisk(uuid, &dev) : one_label(&dev);
    if (found == 0 && root_on(dev, "", 0, 0) == 0)
      return;
    if (found < 0 && ramdisk != 0 && root_on(ramdisk, "", 0, 0) == 0)
      return;
  }
  root_ask();
}

int storage_probe_ufs2(uint device, struct ufs2_volume *volume) {
  struct blockdev block_device;
  struct ufs2_inode root;
  uint dot;
  uint64_t sectors = blockdev_device_size(device);

  if (sectors == 0)
    return -1;
  block_device.device = device;
  block_device.first_lba = 0;
  block_device.sector_count = sectors;
  if (ufs2_probe(&block_device, volume) < 0 ||
      ufs2_read_inode(volume, UFS2_ROOT_INO, &root) < 0 ||
      ufs2_lookup(volume, UFS2_ROOT_INO, ".", &dot) < 0 ||
      dot != UFS2_ROOT_INO)
    return -1;
  return 0;
}

static int asked_for(const char *type, const char *name) {
  return type == 0 || type[0] == 0 || strncmp((char *)type, (char *)name,
                                              VFS_TYPELEN) == 0;
}

#define VOLUME_NAME_MAX 33

// LABELS

#define FS_BY_LABEL "fs-by-label"
#define FS_BY_UUID "fs-by-uuid"

static void clean_into(char *out, int size, const char *in, int len) {
  int n = 0, i;
  char c;

  for (i = 0; i < len && n < size - 1; i++) {
    c = in[i];
    if (c == 0)
      break;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '@' || c == '+' ||
          c == '.'))
      c = '_';
    out[n++] = c;
  }
  while (n > 0 && out[n - 1] == '_')
    n--;
  out[n] = 0;
}

static void hex_into(char *out, uint32_t value) {
  static const char hex[] = "0123456789abcdef";
  int i;

  for (i = 0; i < 8; i++)
    out[i] = hex[(value >> ((7 - i) * 4)) & 15];
  out[8] = 0;
}

// FAT
static int taste_fat32(uint dev, char *label, char *uuid) {
  const uchar *name = 0;
  struct buf *bp;
  uint id = 0;
  int ok;

  bp = bread(dev, 0);
  ok = (bp->flags & B_ERROR) == 0 && bp->data[510] == 0x55 &&
       bp->data[511] == 0xAA && fat32_taste(bp->data, &id, &name);
  if (ok) {
    clean_into(label, VOLUME_NAME_MAX, (const char *)name, 11);
    if (strncmp(label, "NO_NAME", 8) == 0)
      label[0] = 0;
    hex_into(uuid, id);
  }
  brelse(bp);
  return ok ? 0 : -1;
}

// UFS
static int taste_ufs2(uint dev, char *label, char *uuid) {
  struct blockdev volume;
  uchar name[MAXVOLLEN];
  uchar id[8];
  struct ufs2_volume probe;

  if (storage_probe_ufs2(dev, &probe) < 0)
    return -1;
  volume.device = dev;
  volume.first_lba = 0;
  volume.sector_count = blockdev_device_size(dev);
  if (blockdev_read(&volume, UFS2_SUPERBLOCK_OFFSET + UFS2_SB_volname,
                    name, sizeof(name)) == 0)
    clean_into(label, VOLUME_NAME_MAX, (const char *)name, sizeof(name));
  if (blockdev_read(&volume, UFS2_SUPERBLOCK_OFFSET + UFS2_SB_id, id,
                    sizeof(id)) == 0) {
    hex_into(uuid, (uint32_t)id[0] | ((uint32_t)id[1] << 8) |
                       ((uint32_t)id[2] << 16) | ((uint32_t)id[3] << 24));
    hex_into(uuid + 8, (uint32_t)id[4] | ((uint32_t)id[5] << 8) |
                           ((uint32_t)id[6] << 16) | ((uint32_t)id[7] << 24));
  }
  return 0;
}

void storage_publish_labels(uint dev) {
  char label[VOLUME_NAME_MAX];
  char uuid[VOLUME_NAME_MAX];

  if (blockdev_device_size(dev) == 0 ||
      devfs_has_alias(FS_BY_LABEL, (short)major(dev), (short)minor(dev)) ||
      devfs_has_alias(FS_BY_UUID, (short)major(dev), (short)minor(dev)))
    return;

  memset(label, 0, sizeof(label));
  memset(uuid, 0, sizeof(uuid));
  if (taste_fat32(dev, label, uuid) < 0 && taste_ufs2(dev, label, uuid) < 0)
    return;
  if (label[0] != 0)
    devfs_add_alias(FS_BY_LABEL, label, FNU_TYPE_DISK, (short)major(dev),
                    (short)minor(dev), 0640);
  if (uuid[0] != 0 &&
      devfs_add_alias(FS_BY_UUID, uuid, FNU_TYPE_DISK, (short)major(dev),
                      (short)minor(dev), 0640) == 0)
    printf("storage: %s/%s is on another device too; it names the first\n",
            FS_BY_UUID, uuid);
}

void storage_forget_labels(uint dev) {
  devfs_remove_aliases(FS_BY_LABEL, (short)major(dev), (short)minor(dev));
  devfs_remove_aliases(FS_BY_UUID, (short)major(dev), (short)minor(dev));
}

int storage_recognise(uint dev, const char *type, struct vfs_mount *m) {
  struct buf *bp;
  int fat;

  if (m == 0 || blockdev_device_size(dev) == 0)
    return -1;

  if (asked_for(type, "ufs2") && storage_probe_ufs2(dev, &m->ufs2) == 0) {
    m->op = &ufs2_vfsops;
    m->root_inum = UFS2_ROOT_INO;
    strlcpy(m->type, m->op->name, VFS_TYPELEN);
    return 0;
  }

  if (asked_for(type, "fat32")) {
    bp = bread(dev, 0);
    fat = (bp->flags & B_ERROR) == 0 && fat32_taste(bp->data, 0, 0);
    brelse(bp);
    if (fat) {
      fat32_init(dev);
      if (!fat32_present(dev))
        return -1;
      m->op = &fat32_vfsops;
      m->root_inum = fat32_root_cluster(dev);
      strlcpy(m->type, m->op->name, VFS_TYPELEN);
      return 0;
    }
  }

  return -1;
}
