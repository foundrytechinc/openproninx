#include "fs.h"
#include "buf.h"
#include "defs.h"
#include "file.h"
#include "fat32.h"
#include "ufs2.h"

// fat32.c
struct fat32_dirent;
void fat32_init(int dev);
int fat32_read_cluster(int dev, uint cluster, char *buf, uint n);
int fat32_read(int dev, uint start_cluster, char *dst, uint off, uint n);
uint fat32_lookup(int dev, uint dir_cluster, char *name, struct fat32_dirent *res, uint *off);
uint fat32_root_cluster(void);
#include "inc/stat.h"
#include "param.h"
#include "proc.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
#define DEVFS_DEVICE 0xfffffff0U
#define DEVFS_CONSOLE_INO 1
#define DEVFS_FNU_COMMAND_INO 2
#define DEVFS_FNU_SERVICES_INO 3
#define DEVFS_FNU_HEALTH_INO 4
#define DEVFS_TTY_INO 8            /* 8..11 are /dev/tty1../dev/tty4 */
#define DEVFS_TTY_COUNT 4

static void itrunc(struct inode *);

// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;
uint rootdev = ROOTDEV;
int root_fs_type = FS_NATIVE;

int fs_root_readonly(void) { return root_fs_type == FS_UFS2; }

// mask: read=4, write=2, execute=1.  Root bypasses discretionary access.
int inode_access(struct inode *ip, uint uid, int mask) {
  uint bits;
  if (ip == 0 || ip->type == 0) return 0;
  if (uid == 0) return 1;
  if (uid == ip->uid) bits = (ip->mode >> 6) & 7;
  else if (myproc()->gid == ip->gid) bits = (ip->mode >> 3) & 7;
  else bits = ip->mode & 7;
  return (bits & (uint)mask) == (uint)mask;
}

// Read the super block.
void readsb(int dev, struct superblock *sb) {
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void _bzero(int dev, int bno) {
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

//
// Blocks.
//

// Allocate a zeroed disk block.
static uint balloc(uint dev) {
  uint b;
  int bi, m;
  struct buf *bp;

  bp = 0;
  for (b = 0; b < sb.size; b += BPB) {
    bp = bread(dev, BBLOCK(b, sb));
    for (bi = 0; bi < BPB && b + bi < sb.size; bi++) {
      m = 1 << (bi % 8);
      if ((bp->data[bi / 8] & m) == 0) { // Is block free?
        bp->data[bi / 8] |= m;           // Mark block in use.
        log_write(bp);
        brelse(bp);
        _bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void bfree(int dev, uint b) {
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if ((bp->data[bi / 8] & m) == 0) {
    panic("freeing free block");
  }
  bp->data[bi / 8] &= ~m;
  log_write(bp);
  brelse(bp);
}

//
// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

struct inode *iget(uint dev, uint inum);

void iinit(int dev) {
  int i = 0;

  initlock(&icache.lock, "icache");
  for (i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  if (storage_mount_root() == 0) {
    root_fs_type = FS_UFS2;
    return;
  }

  // Detect FS type
  struct buf *bp = bread(dev, 0);
  struct fat32_bpb *bpb = (struct fat32_bpb *)bp->data;
  if (bpb->boot_signature == 0x28 || bpb->boot_signature == 0x29) {
    brelse(bp);
    fat32_init(dev);
    root_fs_type = FS_FAT32;
    cprintf("FS: Detected FAT32 on device %d\n", dev);
    return;
  }
  brelse(bp);

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n",
          sb.size, sb.nblocks, sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);
}

// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode *ialloc(uint dev, short type) {
  uint inum;
  struct buf *bp;
  struct dinode *dip;

  if (storage_ufs2_volume(dev) != 0)
    return 0;
  if (root_fs_type == FS_FAT32) {
    return fat32_ialloc(dev, type);
  }

  for (inum = 1; inum < sb.ninodes; inum++) {
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode *)bp->data + inum % IPB;
    if (dip->type == 0) { // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp); // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void iupdate(struct inode *ip) {
  struct buf *bp;
  struct dinode *dip;

  if (ip->fs_type == FS_UFS2 || ip->fs_type == FS_DEVFS)
    return;
  if (ip->fs_type == FS_FAT32) {
    fat32_iupdate(ip);
    return;
  }

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode *)bp->data + ip->inum % IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  dip->uid = ip->uid;
  dip->gid = ip->gid;
  dip->mode = ip->mode;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
struct inode *iget(uint dev, uint inum) {
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++) {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) {
      // Remember empty slot.
      empty = ip;
    }
  }

  // Recycle an inode cache entry.
  if (empty == 0) {
    panic("iget: no inodes");
  }

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  // a recycled slot still carries the last tenant's filesystem. leaving it
  // makes iupdate() drop writes on the floor and directories lose their dots
  ip->fs_type = FS_NATIVE;
  release(&icache.lock);

  return ip;
}

void iupdate_inum(struct inode *ip, uint inum) {
  acquire(&icache.lock);
  ip->inum = inum;
  release(&icache.lock);
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *idup(struct inode *ip) {
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void ilock(struct inode *ip) {
  struct buf *bp;
  struct dinode *dip;
  const struct ufs2_volume *ufs_volume;

  if (ip == 0 || ip->ref < 1) {
    panic("ilock");
  }

  acquiresleep(&ip->lock);

  if (ip->valid == 0) {
    if (ip->dev == DEVFS_DEVICE && ip->inum == DEVFS_CONSOLE_INO) {
      ip->type = T_DEVICE;
      ip->major = CONSOLE;
      ip->minor = -1;   // /dev/console follows whichever terminal is in front
      ip->nlink = 1;
      ip->size = 0;
      ip->fs_type = FS_DEVFS;
      ip->valid = 1;
      return;
    }
    if (ip->dev == DEVFS_DEVICE && ip->inum >= DEVFS_TTY_INO &&
        ip->inum < DEVFS_TTY_INO + DEVFS_TTY_COUNT) {
      ip->type = T_DEVICE;
      ip->major = CONSOLE;
      ip->minor = (short)(ip->inum - DEVFS_TTY_INO);
      ip->nlink = 1;
      ip->size = 0;
      ip->fs_type = FS_DEVFS;
      ip->valid = 1;
      return;
    }
    if (ip->dev == DEVFS_DEVICE && ip->inum >= DEVFS_FNU_COMMAND_INO &&
        ip->inum <= DEVFS_FNU_HEALTH_INO) {
      ip->type = T_DEVICE;
      ip->major = FNU_STATE;
      ip->minor = ip->inum - DEVFS_FNU_COMMAND_INO + FNU_STATE_COMMAND;
      ip->nlink = 1;
      ip->size = 0;
      ip->fs_type = FS_DEVFS;
      ip->valid = 1;
      return;
    }
    ufs_volume = storage_ufs2_volume(ip->dev);
    if (ufs_volume != 0) {
      if (ufs2_read_inode(ufs_volume, ip->inum, &ip->ufs2_info) < 0)
        goto invalid_ufs2_inode;
      if ((ip->ufs2_info.mode & 0170000) == 0040000)
        ip->type = T_DIR;
      else if ((ip->ufs2_info.mode & 0170000) == 0100000)
        ip->type = T_FILE;
      else
        goto invalid_ufs2_inode;
      // The legacy inode ABI is 32-bit sized. Refuse oversized files rather
      // than truncating their size and returning incorrect content.
      if (ip->ufs2_info.size > 0xffffffffULL)
        goto invalid_ufs2_inode;
      ip->size = (uint)ip->ufs2_info.size;
      ip->nlink = ip->ufs2_info.nlink;
      ip->uid = ip->ufs2_info.uid;
      ip->gid = ip->ufs2_info.gid;
      ip->mode = ip->ufs2_info.mode & 07777;
      ip->fs_type = FS_UFS2;
      ip->valid = 1;
      return;
    }
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode *)bp->data + ip->inum % IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    ip->uid = dip->uid;
    ip->gid = dip->gid;
    ip->mode = dip->mode;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->fs_type = FS_NATIVE;
    ip->valid = 1;
    if (ip->type == 0) {
      panic("ilock: no type");
    }
  }
  return;

invalid_ufs2_inode:
  // A malformed or unsupported UFS2 object is an I/O error, not a reason to
  // take down the kernel. Callers reject type 0 just like a failed lookup.
  memset(&ip->ufs2_info, 0, sizeof(ip->ufs2_info));
  ip->type = 0;
  ip->size = 0;
  ip->major = 0;
  ip->minor = 0;
  ip->nlink = 1;
  ip->fs_type = FS_UFS2;
  ip->valid = 1;
}

// Unlock the given inode.
void iunlock(struct inode *ip) {
  if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1) {
    panic("iunlock");
  }

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be outside a transaction
// because iput() may free the inode.
void iput(struct inode *ip) {
  acquire(&icache.lock);
  if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
    // inode has no links and no other references: truncate and free.
    // icache.lock is still held.
    release(&icache.lock);
    ilock(ip);
    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
    ip->valid = 0;
    iunlock(ip);
    acquire(&icache.lock);
  }
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void iunlockput(struct inode *ip) {
  iunlock(ip);
  iput(ip);
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static void itrunc(struct inode *ip) {
  struct buf *bp;
  uint *a;

  if (ip->fs_type == FS_UFS2) {
    const struct ufs2_volume *volume = storage_ufs2_volume(ip->dev);
    if (volume != 0) {
      ufs2_truncate(volume, ip->inum, &ip->ufs2_info);
      ufs2_free_inode(volume, ip->inum);
    }
    return;
  }
  for (int i = 0; i < NDIRECT; i++) {
    if (ip->addrs[i]) {
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if (ip->addrs[NDIRECT]) {
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint *)bp->data;
    for (uint j = 0; j < NINDIRECT; j++) {
      if (a[j]) {
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  if (ip->addrs[NDIRECT + 1]) {
    bp = bread(ip->dev, ip->addrs[NDIRECT + 1]);
    a = (uint *)bp->data;
    for (uint j = 0; j < NINDIRECT; j++) {
      if (a[j]) {
        struct buf *bp2 = bread(ip->dev, a[j]);
        uint *b = (uint *)bp2->data;
        for (uint k = 0; k < NINDIRECT; k++) {
          if (b[k]) {
            bfree(ip->dev, b[k]);
          }
        }
        brelse(bp2);
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT + 1]);
    ip->addrs[NDIRECT + 1] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint bmap(struct inode *ip, uint bn) {
  uint addr, *a;
  struct buf *bp;

  if (bn < NDIRECT) {
    if ((addr = ip->addrs[bn]) == 0) {
      ip->addrs[bn] = addr = balloc(ip->dev);
    }
    return addr;
  }
  bn -= NDIRECT;

  if (bn < NINDIRECT) {
    // Load indirect block, allocating if necessary.
    if ((addr = ip->addrs[NDIRECT]) == 0) {
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    }
    bp = bread(ip->dev, addr);
    a = (uint *)bp->data;
    if ((addr = a[bn]) == 0) {
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
  bn -= NINDIRECT;

  if (bn < NINDIRECT * NINDIRECT) {
    if ((addr = ip->addrs[NDIRECT + 1]) == 0) {
      ip->addrs[NDIRECT + 1] = addr = balloc(ip->dev);
    }
    bp = bread(ip->dev, addr);
    a = (uint *)bp->data;
    if ((addr = a[bn / NINDIRECT]) == 0) {
      a[bn / NINDIRECT] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);

    bp = bread(ip->dev, addr);
    a = (uint *)bp->data;
    if ((addr = a[bn % NINDIRECT]) == 0) {
      a[bn % NINDIRECT] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void stati(struct inode *ip, struct stat *st) {
  if (ip->fs_type == FS_FAT32) {
      st->device = ip->dev;
      st->type = ip->type;
      st->size = ip->size;
      st->mode = ip->mode; st->uid = ip->uid; st->gid = ip->gid;
  } else {
      st->device = ip->dev;
      st->type = ip->type;
      st->size = ip->size;
      st->mode = ip->mode; st->uid = ip->uid; st->gid = ip->gid;
  }
}

// Read data from inode.
// Caller must hold ip->lock.
int readi(struct inode *ip, char *dst, uint off, uint n) {
  uint tot, m;
  struct buf *bp;

  if (ip->fs_type == FS_FAT32) {
      if (off > ip->size) return 0;
      if (off + n > ip->size) n = ip->size - off;
      return fat32_read(ip->dev, ip->fat32_info.cluster, dst, off, n);
  }
  if (ip->fs_type == FS_UFS2) {
    const struct ufs2_volume *volume = storage_ufs2_volume(ip->dev);
    if (volume == 0)
      return -1;
    if (ip->type == T_DIR) {
      struct dirent entry;
      uint inum;
      if (off % sizeof(entry) != 0 || n < sizeof(entry) ||
          ufs2_readdir(volume, ip->inum, off / sizeof(entry), &inum,
                       entry.name, sizeof(entry.name)) < 0)
        return 0;
      if (inum > 0xffff)
        return -1;
      entry.inum = (ushort)inum;
      memmove(dst, &entry, sizeof(entry));
      return sizeof(entry);
    }
    if (off > ip->size || off + n < off)
      return -1;
    if (off + n > ip->size)
      n = ip->size - off;
    return ufs2_read(volume, &ip->ufs2_info, off, dst, n);
  }

  if (ip->type == T_DEVICE) {
    if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read) {
      return -1;
    }
    return devsw[ip->major].read(ip, dst, n);
  }

  if (off > ip->size || off + n < off) {
    return -1;
  }
  if (off + n > ip->size) {
    n = ip->size - off;
  }

  for (tot = 0; tot < n; tot += m, off += m, dst += m) {
    bp = bread(ip->dev, bmap(ip, off / BSIZE));
    m = min(n - tot, BSIZE - off % BSIZE);
    memmove(dst, bp->data + off % BSIZE, m);
    brelse(bp);
  }
  return n;
}

// Write data to inode.
// Caller must hold ip->lock.
int writei(struct inode *ip, char *src, uint off, uint n) {
  uint tot, m;
  struct buf *bp;

  if (ip->fs_type == FS_FAT32) {
      if (off > ip->size && ip->size != 0xFFFFFFFF) return -1;
      int written = fat32_write(ip->dev, &ip->fat32_info.cluster, src, off, n);
      if (written > 0 && (off + written > ip->size || ip->size == 0xFFFFFFFF)) {
          if (ip->size != 0xFFFFFFFF) {
              ip->size = off + written;
              // Note: we don't have fat32_iupdate yet to update dirent on disk
          }
      }
      return written;
  }

  if (ip->fs_type == FS_UFS2)
    return -1;

  if (ip->type == T_DEVICE) {
    if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write) {
      return -1;
    }
    return devsw[ip->major].write(ip, src, n);
  }

  if (off > ip->size || off + n < off) {
    return -1;
  }
  if (off + n > MAXFILE * BSIZE) {
    return -1;
  }

  for (tot = 0; tot < n; tot += m, off += m, src += m) {
    bp = bread(ip->dev, bmap(ip, off / BSIZE));
    m = min(n - tot, BSIZE - off % BSIZE);
    memmove(bp->data + off % BSIZE, src, m);
    log_write(bp);
    brelse(bp);
  }

  if (n > 0 && off > ip->size) {
    ip->size = off;
    iupdate(ip);
  }
  return n;
}

//
// Directories
//

int namecmp(const char *s, const char *t) { return strncmp(s, t, DIRSIZ); }

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode *dirlookup(struct inode *dp, char *name, uint *poff) {
  uint off, inum;
  struct dirent de;

  if (dp->type != T_DIR) {
    panic("dirlookup not DIR");
  }

  if (dp->fs_type == FS_UFS2) {
    const struct ufs2_volume *volume = storage_ufs2_volume(dp->dev);
    uint inum;
    if (volume == 0 || ufs2_lookup(volume, dp->inum, name, &inum) < 0)
      return 0;
    if (poff)
      *poff = 0;
    return iget(dp->dev, inum);
  }

  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
      panic("dirlookup read");
    }
    if (de.inum == 0) {
      continue;
    }
    if (namecmp(name, de.name) == 0) {
      // entry matches path element
      if (poff) {
        *poff = off;
      }
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int dirlink(struct inode *dp, char *name, uint inum) {
  uint off;
  struct dirent de;
  struct inode *ip;

  if (dp->fs_type == FS_UFS2)
    return -1;
  if (dp->fs_type == FS_FAT32) {
    // For FAT32, we need the actual inode to get attributes and cluster.
    // Since inum is passed, we have to get the inode from cache.
    ip = iget(dp->dev, inum);
    int res = fat32_dirlink(dp, name, ip);
    
    // Update the in-memory inode's inum to something stable if it was a temp one
    if (res == 0 && (ip->inum & 0x40000000)) { // If it was a temp inum
        uint cluster = ip->fat32_info.cluster;
        uint inum;
        if (cluster == 0) {
            inum = 0x80000000 | (ip->fat32_info.parent_cluster << 12) | (ip->fat32_info.dir_off / 32);
        } else {
            inum = cluster;
        }
        iupdate_inum(ip, inum);
    }
    iput(ip);
    return res;
  }

  // Check that name is not present.
  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
      panic("dirlink read");
    }
    if (de.inum == 0) {
      break;
    }
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if (writei(dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
    panic("dirlink");
  }

  return 0;
}

//
// Paths
//

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
static char *skipelem(char *path, char *name) {
  char *s;
  int len;

  while (*path == '/') {
    path++;
  }
  if (*path == 0) {
    return 0;
  }
  s = path;
  while (*path != '/' && *path != 0) {
    path++;
  }
  len = path - s;
  if (len > MAXNAMLEN)
    return 0;
  memmove(name, s, len);
  name[len] = 0;
  while (*path == '/') {
    path++;
  }
  return path;
}

// `/data` is a VFS namespace boundary, not a legacy-root directory entry.
// Keeping it virtual lets the old boot filesystem stay unchanged while FNU
// Data is supplied by its dedicated UFS2 device.
static char *data_namespace(char *path) {
  while (*path == '/')
    path++;
  if (path[0] != 'd' || path[1] != 'a' || path[2] != 't' ||
      path[3] != 'a' || (path[4] != '/' && path[4] != '\0') ||
      storage_ufs2_volume(2) == 0)
    return 0;
  path += 4;
  while (*path == '/')
    path++;
  return path;
}

static int is_console_path(char *path) {
  if (path[0] == '/')
    path++;
  if (strncmp(path, "dev/console", 12) == 0 && path[11] == '\0')
    return 1;
  return path[0] == 'c' && path[1] == 'o' && path[2] == 'n' &&
         path[3] == 's' && path[4] == 'o' && path[5] == 'l' &&
         path[6] == 'e' && path[7] == '\0';
}

static uint devfs_path(char *path) {
  char *p;

  if (is_console_path(path))
    return DEVFS_CONSOLE_INO;
  p = path;
  if (p[0] == '/')
    p++;
  if (strncmp(p, "dev/tty", 7) == 0)
    p += 7;
  else if (strncmp(p, "tty", 3) == 0)
    p += 3;
  else
    p = 0;
  if (p != 0 && p[0] >= '1' && p[0] < '1' + DEVFS_TTY_COUNT && p[1] == '\0')
    return DEVFS_TTY_INO + (uint)(p[0] - '1');

  if (path[0] == '/')
    path++;
  if (strncmp(path, "fnusvc.command", 14) == 0 && path[14] == '\0')
    return DEVFS_FNU_COMMAND_INO;
  if (strncmp(path, "fnusvc.status", 13) == 0 && path[13] == '\0')
    return DEVFS_FNU_SERVICES_INO;
  if (strncmp(path, ".fnuhealth", 10) == 0 && path[10] == '\0')
    return DEVFS_FNU_HEALTH_INO;
  return 0;
}

// Name of the entry in dp that points at child, minus the dots.
static int dir_name_of(struct inode *dp, uint child, char *name, uint size) {
  struct dirent de;
  uint off, i;

  if (dp->type != T_DIR)
    return -1;

  if (dp->fs_type == FS_UFS2) {
    const struct ufs2_volume *volume = storage_ufs2_volume(dp->dev);
    uint inum;
    for (i = 0; volume != 0 &&
                ufs2_readdir(volume, dp->inum, i, &inum, name, size) == 0; i++)
      if (inum == child && strncmp(name, ".", 2) != 0 &&
          strncmp(name, "..", 3) != 0)
        return 0;
    return -1;
  }

  if (dp->fs_type == FS_FAT32) {
    char block[BSIZE];
    for (off = 0;; off += BSIZE) {
      int n = fat32_read(dp->dev, dp->fat32_info.cluster, block, off, BSIZE);
      if (n <= 0)
        return -1;
      for (i = 0; i + sizeof(struct fat32_dirent) <= (uint)n;
           i += sizeof(struct fat32_dirent)) {
        struct fat32_dirent *de32 = (struct fat32_dirent *)(block + i);
        uint cluster, inum, k, len = 0;
        if (de32->name[0] == 0)
          return -1;
        if (de32->name[0] == 0xe5 || de32->attr == FAT32_ATTR_LONG_NAME ||
            de32->name[0] == '.')
          continue;
        cluster = ((uint)de32->first_cluster_high << 16) |
                  de32->first_cluster_low;
        inum = cluster ? cluster
                       : (0x80000000u | (dp->fat32_info.cluster << 12) |
                          ((off + i) / 32));
        if (inum != child)
          continue;
        for (k = 0; k < 8 && de32->name[k] != ' ' && len + 2 < size; k++)
          name[len++] = de32->name[k];
        if (de32->name[8] != ' ' && len + 1 < size) {
          name[len++] = '.';
          for (k = 8; k < 11 && de32->name[k] != ' ' && len + 1 < size; k++)
            name[len++] = de32->name[k];
        }
        name[len] = 0;
        return 0;
      }
      if (n < BSIZE)
        return -1;
    }
  }

  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      return -1;
    if (de.inum != child)
      continue;
    for (i = 0; i < DIRSIZ && i + 1 < size && de.name[i]; i++)
      name[i] = de.name[i];
    name[i] = 0;
    if (strncmp(name, ".", 2) == 0 || strncmp(name, "..", 3) == 0)
      continue;
    return 0;
  }
  return -1;
}

static int path_prepend(char *buf, uint *pos, const char *s) {
  uint n = strlen(s);

  if (*pos < n + 1)
    return -1;
  *pos -= n;
  memmove(buf + *pos, s, n);
  buf[--(*pos)] = '/';
  return 0;
}

// Walk up through `..`, naming each child in its parent, the way 4.4BSD's
// getwd() did. No name cache to go stale.
int inode_path(struct inode *start, char *buf, uint size) {
  struct inode *ip, *parent;
  char name[NAMEBUFSZ];
  uint pos;
  int depth;

  if (size < 2)
    return -1;
  pos = size - 1;
  buf[pos] = 0;

  ip = idup(start);
  for (depth = 0; depth < MAXPATHDEPTH; depth++) {
    ilock(ip);
    if (ip->type != T_DIR) {
      iunlockput(ip);
      return -1;
    }
    parent = ip->fs_type == FS_DEVFS ? 0 : dirlookup(ip, "..", 0);
    if (parent == 0) {
      iunlock(ip);
      break;
    }
    if (parent->dev == ip->dev && parent->inum == ip->inum) {
      iunlock(ip);
      iput(parent);
      break;
    }
    iunlock(ip);
    ilock(parent);
    if (dir_name_of(parent, ip->inum, name, sizeof(name)) < 0) {
      iunlockput(parent);
      iput(ip);
      return -1;
    }
    iunlock(parent);
    iput(ip);
    ip = parent;
    if (path_prepend(buf, &pos, name) < 0) {
      iput(ip);
      return -1;
    }
  }
  if (depth == MAXPATHDEPTH) {
    iput(ip);
    return -1;
  }

  // the volume the walk ended on decides the prefix
  if (ip->dev != ROOTDEV && storage_ufs2_volume(ip->dev) != 0 &&
      path_prepend(buf, &pos, "data") < 0) {
    iput(ip);
    return -1;
  }
  iput(ip);

  if (buf[pos] == 0) {
    buf[--pos] = '/';
  }
  memmove(buf, buf + pos, size - pos);
  return 0;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for NAMEBUFSZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *namex(char *path, int nameiparent, char *name) {
  struct inode *ip, *next;
  char *data_path;

  // The immutable UFS2 root carries no writable device nodes. PID 1 still
  // needs its recovery console, so present it as a small devfs inode.
  if (!nameiparent && devfs_path(path) != 0)
    return iget(DEVFS_DEVICE, devfs_path(path));

  if (*path == '/') {
    data_path = data_namespace(path);
    if (data_path != 0) {
      ip = iget(2, UFS2_ROOT_INO);
      path = data_path;
    } else if (root_fs_type == FS_FAT32) {
      uint root_cluster = fat32_root_cluster();
      ip = iget(ROOTDEV, root_cluster);
      ilock(ip);
      if (!ip->valid) {
        ip->fs_type = FS_FAT32;
        ip->fat32_info.cluster = root_cluster;
        ip->size = 0xFFFFFFFF; // Directory size is unknown/large
        ip->fat32_info.attr = FAT32_ATTR_DIRECTORY;
        ip->type = T_DIR;
        ip->nlink = 1;
        ip->valid = 1;
      }
      iunlock(ip);
    } else {
      ip = iget(ROOTDEV, ROOTINO);
    }
  } else {
    ip = idup(myproc()->cwd);
  }

  while ((path = skipelem(path, name)) != 0) {
    ilock(ip);
    if (ip->type != T_DIR) {
      iunlockput(ip);
      return 0;
    }
    if (!inode_access(ip, myproc()->uid, 1)) {
      iunlockput(ip);
      return 0;
    }
    // Native directory entries have a fixed 14-byte name field and FAT32 is
    // currently exposed through its short-name adapter.  Reject an overlong
    // component there instead of letting namecmp() match a truncated prefix.
    if (ip->fs_type != FS_UFS2 && strlen(name) > DIRSIZ) {
      iunlockput(ip);
      return 0;
    }
    // `.` names the inode being traversed.  Resolve it without consulting a
    // filesystem-specific directory entry; this is also required for a child
    // process whose cwd is a newly created directory.
    if (!nameiparent && name[0] == '.' && name[1] == '\0') {
      iunlock(ip);
      continue;
    }
    if (nameiparent && *path == '\0') {
      // Stop one level early.
      iunlock(ip);
      return ip;
    }

    if (ip->fs_type == FS_FAT32) {
      struct fat32_dirent de;
      uint off;
      uint cluster = fat32_lookup(ip->dev, ip->fat32_info.cluster, name, &de, &off);
      if (cluster == 0 && de.name[0] == 0) { // Not found
        iunlockput(ip);
        return 0;
      }
      
      // Use a stable inum. If cluster is 0 (empty file), use the offset in the parent.
      // To make it unique across directories, we combine with parent cluster.
      uint inum = (cluster == 0) ? (0x80000000 | (ip->fat32_info.cluster << 12) | (off / 32)) : cluster;
      
      next = iget(ip->dev, inum);
      ilock(next);
      if (!next->valid) {
        next->fs_type = FS_FAT32;
        next->fat32_info.cluster = cluster;
        next->size = de.file_size;
        next->fat32_info.attr = de.attr;
        next->fat32_info.parent_cluster = ip->fat32_info.cluster;
        next->fat32_info.dir_off = off;
        next->nlink = 1;
        next->uid = 0;
        next->gid = 0;
        next->mode = (de.attr & FAT32_ATTR_DIRECTORY) ? 0755 : 0644;
        next->valid = 1;
        if (de.attr & FAT32_ATTR_DIRECTORY)
          next->type = T_DIR;
        else
          next->type = T_FILE;
      }
      iunlock(next);
    } else {
      if ((next = dirlookup(ip, name, 0)) == 0) {
        iunlockput(ip);
        return 0;
      }
    }
    iunlockput(ip);
    ip = next;
  }
  if (nameiparent) {
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode *namei(char *path) {
  char name[NAMEBUFSZ];
  return namex(path, 0, name);
}

struct inode *nameiparent(char *path, char *name) {
  return namex(path, 1, name);
}
