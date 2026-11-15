#include "inc/dir.h"
#include "inc/types.h"
#include "defs.h"
#include "mmu.h"
#include "param.h"
#include "mutex.h"
#include "sleeplock.h"
#include "inc/stat.h"
#include "uio.h"
#include "fs.h"
#include "file.h"
#include "buf.h"
#include "fat32.h"
#include "malloc.h"

#define DOS_EPOCH_YEAR 1980
#define DOS_SECONDS_TO_1980 (((8 * 365) + (2 * 366)) * (24 * 60 * 60))
#define DOS_MAX_YEAR (DOS_EPOCH_YEAR + 127)

static void dos2unixtime(uint dd, uint dt, uint dh, struct fnu_timespec *out) {
  struct fnu_rtctime ymd;

  out->pad = 0;
  if (dd == 0) {
    out->sec = 0;
    out->nsec = 0;
    return;
  }
  memset(&ymd, 0, sizeof(ymd));
  ymd.year = (uint16)(((dd >> 9) & 0x7f) + DOS_EPOCH_YEAR);
  ymd.mon = (uint8)((dd >> 5) & 0x0f);
  ymd.mday = (uint8)(dd & 0x1f);
  ymd.hour = (uint8)((dt >> 11) & 0x1f);
  ymd.min = (uint8)((dt >> 5) & 0x3f);
  ymd.sec = (uint8)((dt & 0x1f) * 2);
  if (ymd.mon < 1 || ymd.mon > 12 || ymd.mday < 1 || ymd.mday > 31) {
    out->sec = 0;
    out->nsec = 0;
    return;
  }
  out->sec = clock_ymdhms_to_secs(&ymd);
  out->nsec = (int32_t)((dh % 100) * 10000000);
}

static void unix2dostime(const struct fnu_timespec *t, ushort *dd, ushort *dt,
                         uchar *dh) {
  struct fnu_rtctime ymd;
  int64_t secs = t->sec;

  *dd = 0;
  if (dt)
    *dt = 0;
  if (dh)
    *dh = 0;
  if (secs < DOS_SECONDS_TO_1980)
    return;
  secs &= ~(int64_t)1;
  clock_secs_to_ymdhms(secs, &ymd);
  if (ymd.year > DOS_MAX_YEAR)
    return;

  *dd = (ushort)((ymd.mday & 0x1f) | ((ymd.mon & 0x0f) << 5) |
                 (((ymd.year - DOS_EPOCH_YEAR) & 0x7f) << 9));
  if (dt)
    *dt = (ushort)(((ymd.sec / 2) & 0x1f) | ((ymd.min & 0x3f) << 5) |
                   ((ymd.hour & 0x1f) << 11));
  if (dh)
    *dh = (uchar)((t->sec & 1) * 100 + t->nsec / 10000000);
}

// the root has no entry: 1980-01-01
void fat32_root_times(struct inode *ip) {
  dos2unixtime(1 << 5 | 1, 0, 0, &ip->mtime);
  ip->atime = ip->ctime = ip->birthtime = ip->mtime;
}

// entry time to timespec
void fat32_dirent_times(const struct fat32_dirent *de, struct fnu_timespec *a,
                        struct fnu_timespec *m, struct fnu_timespec *b) {
  dos2unixtime(de->last_access_date, 0, 0, a);
  dos2unixtime(de->write_date, de->write_time, 0, m);
  dos2unixtime(de->create_date, de->create_time, de->create_time_tenth, b);
}

// and back
void fat32_dirent_stamp(struct fat32_dirent *de, const struct fnu_timespec *a,
                        const struct fnu_timespec *m,
                        const struct fnu_timespec *b) {
  ushort dd, dt;
  uchar dh;

  if (a != 0) {
    unix2dostime(a, &dd, 0, 0);
    de->last_access_date = dd;
  }
  if (m != 0) {
    unix2dostime(m, &dd, &dt, 0);
    de->write_date = dd;
    de->write_time = dt;
  }
  if (b != 0) {
    unix2dostime(b, &dd, &dt, &dh);
    de->create_date = dd;
    de->create_time = dt;
    de->create_time_tenth = dh;
  }
}

static uint cluster_to_sector(int dev, uint cluster);
static void set_next_cluster(int dev, uint cluster, uint next);
static uint alloc_cluster(int dev, int clear);
static int dir_entry(struct inode *dp, struct fat32_dirbuf *cache, uint off,
                     struct fat32_dirent *de);
static uint fat_entry(int dev, uint cluster);
static int write_dirslot(struct inode *dp, uint off,
                         const struct fat32_dirent *de);
static uint next_cluster(int dev, uint cluster);
static int is_eof(uint cluster);
static uint pcbmap(struct inode *ip, uint want, int extend);
static void free_cluster_chain(int dev, uint cluster);

// FAT12
#define FAT_ROOT_FIXED 1

// one per mounted device
struct fat32_volume {
    struct fat32_bpb bpb;
    uint fat_type;
    uint fat_start_sector;
    uint fat_base;
    uint fat_size;
    uint fat_copies;
    uint root_sector;
    uint root_sectors;
    uint data_start_sector;
    uint sectors_per_cluster;
    uint max_cluster;
    uint free_clusters;
    uint next_free;
    int dev;
    int valid;
    int dirty;
    struct fat32_volume *next;
};

static struct fat32_volume *fat_vols;

static struct fat32_volume *vol(int dev) {
    struct fat32_volume *v;

    for (v = fat_vols; v != 0; v = v->next)
        if (v->valid && v->dev == dev)
            return v;
    return 0;
}

int fat32_present(int dev) { return vol(dev) != 0; }

#define FAT32_CLNSHUT 0x08000000u
#define FAT16_CLNSHUT 0x8000u

static void mark_volume(struct fat32_volume *v, int clean) {
    struct buf *bp;
    uint entry, base, f;

    if (v->fat_type == 12)
        return;
    for (f = 0; f < v->fat_copies; f++) {
        base = v->fat_copies == 1 ? v->fat_start_sector
                                  : v->fat_base + f * v->fat_size;
        bp = bread(v->dev, base);
        if (bp->flags & B_ERROR) {
            brelse(bp);
            return;
        }
        if (v->fat_type == 32) {
            entry = *(uint*)(bp->data + 4);
            entry = clean ? (entry | FAT32_CLNSHUT) : (entry & ~FAT32_CLNSHUT);
            *(uint*)(bp->data + 4) = entry;
        } else {
            entry = *(ushort*)(bp->data + 2);
            entry = clean ? (entry | FAT16_CLNSHUT) : (entry & ~FAT16_CLNSHUT);
            *(ushort*)(bp->data + 2) = (ushort)entry;
        }
        bwrite(bp);
        brelse(bp);
    }
}

static void fat32_dirty(struct fat32_volume *v) {
    if (v->dirty)
        return;
    v->dirty = 1;
    mark_volume(v, 0);
}

// the FSInfo count is advisory
static void fsinfo_flush(struct fat32_volume *v) {
    struct buf *bp;

    if (v->fat_type != 32 || v->bpb.fs_info == 0 || v->bpb.fs_info == 0xFFFF)
        return;
    bp = bread(v->dev, v->bpb.fs_info);
    if (*(uint*)(bp->data) == 0x41615252 &&
        *(uint*)(bp->data + 484) == 0x61417272) {
        *(uint*)(bp->data + 488) = v->free_clusters;
        *(uint*)(bp->data + 492) = v->next_free;
        bwrite(bp);
    }
    brelse(bp);
}

void fat32_sync(int dev) {
    struct fat32_volume *v = vol(dev);

    if (v != 0)
        fsinfo_flush(v);
}

void fat32_quiesce(int dev) {
    struct fat32_volume *v = vol(dev);

    if (v != 0) {
        fsinfo_flush(v);
        if (v->dirty) {
            mark_volume(v, 1);
            v->dirty = 0;
        }
    }
}

void fat32_forget(int dev) {
    struct fat32_volume *v = vol(dev);

    if (v != 0) {
        fsinfo_flush(v);
        if (v->dirty) {
            mark_volume(v, 1);
            v->dirty = 0;
        }
        v->valid = 0;
    }
}

static uint cluster_to_sector(int dev, uint cluster) {
    struct fat32_volume *v = vol(dev);

    if (v == 0)
        return 0;
    return v->data_start_sector + (cluster - 2) * v->sectors_per_cluster;
}

static uchar fat_byte(struct fat32_volume *v, uint base, uint off) {
    struct buf *bp = bread(v->dev, base + off / BSIZE);
    uchar c = (uchar)bp->data[off % BSIZE];

    brelse(bp);
    return c;
}

static void fat_byte_set(struct fat32_volume *v, uint base, uint off, uchar c) {
    struct buf *bp = bread(v->dev, base + off / BSIZE);

    bp->data[off % BSIZE] = (char)c;
    bdirty(bp);
    brelse(bp);
}

static uint fat_get_at(struct fat32_volume *v, uint base, uint c) {
    uint off, w;

    if (v->fat_type == 32) {
        struct buf *bp = bread(v->dev, base + (c * 4) / BSIZE);
        w = *(uint *)(bp->data + (c * 4) % BSIZE) & 0x0FFFFFFF;
        brelse(bp);
        return w;
    }
    if (v->fat_type == 16) {
        off = c * 2;
        w = fat_byte(v, base, off) | (fat_byte(v, base, off + 1) << 8);
        return w >= 0xFFF7 ? 0x0FFFFFF0 | (w & 0xF) : w;
    }
    off = c + c / 2;
    w = fat_byte(v, base, off) | (fat_byte(v, base, off + 1) << 8);
    w = (c & 1) ? w >> 4 : w & 0xFFF;
    return w >= 0xFF7 ? 0x0FFFFFF0 | (w & 0xF) : w;
}

static void fat_put_at(struct fat32_volume *v, uint base, uint c, uint val) {
    uint off, w;

    if (v->fat_type == 32) {
        struct buf *bp = bread(v->dev, base + (c * 4) / BSIZE);
        uint *e = (uint *)(bp->data + (c * 4) % BSIZE);
        *e = (*e & 0xF0000000) | (val & 0x0FFFFFFF);
        bdirty(bp);
        brelse(bp);
        return;
    }
    if (v->fat_type == 16) {
        w = val >= 0x0FFFFFF7 ? 0xFFF0 | (val & 0xF) : val & 0xFFFF;
        fat_byte_set(v, base, c * 2, (uchar)w);
        fat_byte_set(v, base, c * 2 + 1, (uchar)(w >> 8));
        return;
    }
    w = val >= 0x0FFFFFF7 ? 0xFF0 | (val & 0xF) : val & 0xFFF;
    off = c + c / 2;
    if (c & 1) {
        fat_byte_set(v, base, off, (uchar)((fat_byte(v, base, off) & 0x0F) | (w << 4)));
        fat_byte_set(v, base, off + 1, (uchar)(w >> 4));
    } else {
        fat_byte_set(v, base, off, (uchar)w);
        fat_byte_set(v, base, off + 1, (uchar)((fat_byte(v, base, off + 1) & 0xF0) | (w >> 8)));
    }
}

static void set_next_cluster(int dev, uint cluster, uint next) {
    struct fat32_volume *v = vol(dev);
    uint base;

    if (v == 0 || cluster < 2 || cluster > v->max_cluster)
        return;
    fat32_dirty(v);
    for (uint f = 0; f < v->fat_copies; f++) {
        base = v->fat_copies == 1 ? v->fat_start_sector
                                  : v->fat_base + f * v->fat_size;
        fat_put_at(v, base, cluster, next);
    }
}

static uint alloc_cluster(int dev, int clear) {
    struct fat32_volume *v = vol(dev);
    uint span, c;

    if (v == 0)
        return 0;
    span = v->max_cluster - 1;
    for (uint i = 0; i < span; i++) {
        c = v->next_free + i;
        if (c > v->max_cluster)
            c -= span;
        if (fat_entry(dev, c) != 0)
            continue;
        v->next_free = c + 1 > v->max_cluster ? 2 : c + 1;
        set_next_cluster(dev, c, 0x0FFFFFFF);
        if (v->free_clusters > 0)
            v->free_clusters--;

        if (clear) {
            uint sector = cluster_to_sector(dev, c);
            for (uint k = 0; k < v->sectors_per_cluster; k++) {
                struct buf *dbp = bgetblk(dev, sector + k);
                memset(dbp->data, 0, BSIZE);
                bdirty(dbp);
                brelse(dbp);
            }
        }
        return c;
    }
    return 0;
}

static uint fat_entry(int dev, uint cluster) {
    struct fat32_volume *v = vol(dev);

    if (v == 0 || cluster < 2 || cluster > v->max_cluster)
        return 0x0FFFFFFF;
    return fat_get_at(v, v->fat_start_sector, cluster);
}

static uint next_cluster(int dev, uint cluster) {
    struct fat32_volume *v = vol(dev);
    uint next = fat_entry(dev, cluster);

    if (v == 0 || next < 2 || next > v->max_cluster)
        return 0x0FFFFFFF;
    return next;
}

static int is_eof(uint cluster) {
    return cluster >= 0x0FFFFFF8;
}

static int fixed_root(struct fat32_volume *v, struct inode *ip) {
    return v->fat_type != 32 && ip->fat32_info.cluster == FAT_ROOT_FIXED;
}

// dir size is its chain's; the entry says 0
uint fat32_dir_size(struct inode *ip) {
    struct fat32_volume *v = vol(ip->dev);
    uint per, n, most;

    if (v == 0)
        return 0;
    if (fixed_root(v, ip))
        return v->root_sectors * BSIZE;
    per = v->sectors_per_cluster * BSIZE;
    most = (65536 * sizeof(struct fat32_dirent) + per - 1) / per;
    for (n = 0; n < most && !is_eof(pcbmap(ip, n, 0)); n++)
        ;
    return n * per;
}

static int root_copy(struct fat32_volume *v, struct uio *uio, uint off, uint n,
                     int write) {
    uint end = v->root_sectors * BSIZE, tot = 0;

    if (off >= end)
        return 0;
    if (n > end - off)
        n = end - off;
    while (n > 0) {
        struct buf *bp = bread(v->dev, v->root_sector + off / BSIZE);
        uint sec_off = off % BSIZE, chunk = n > BSIZE - sec_off ? BSIZE - sec_off : n;

        if (uiomove(bp->data + sec_off, chunk, uio) != 0) {
            brelse(bp);
            return tot > 0 ? (int)tot : -1;
        }
        if (write)
            bdirty(bp);
        brelse(bp);
        off += chunk;
        n -= chunk;
        tot += chunk;
    }
    return (int)tot;
}

uint fat32_dirent_cluster(int dev, const struct fat32_dirent *de) {
    struct fat32_volume *v = vol(dev);

    if (v != 0 && v->fat_type != 32)
        return de->first_cluster_low;
    return ((uint)de->first_cluster_high << 16) | de->first_cluster_low;
}

static uint count_free_clusters(struct fat32_volume *v) {
    uint free_count = 0;

    for (uint c = 2; c <= v->max_cluster; c++)
        if (fat_get_at(v, v->fat_start_sector, c) == 0)
            free_count++;
    return free_count;
}

static int fsinfo_believe(struct fat32_volume *v, uint *free_count,
                          uint *next) {
    struct buf *bp;
    uint c, n, entry;
    int ok;

    if (v->fat_type != 32 || v->bpb.fs_info == 0 || v->bpb.fs_info == 0xFFFF)
        return -1;

    bp = bread(v->dev, v->fat_start_sector);
    ok = (bp->flags & B_ERROR) == 0;
    entry = ok ? *(uint*)(bp->data + 4) : 0;
    brelse(bp);
    if (!ok || (entry & FAT32_CLNSHUT) == 0)
        return -1;

    bp = bread(v->dev, v->bpb.fs_info);
    ok = (bp->flags & B_ERROR) == 0 &&
         *(uint*)(bp->data) == 0x41615252 &&
         *(uint*)(bp->data + 484) == 0x61417272 &&
         *(uint*)(bp->data + 508) == 0xAA550000;
    c = ok ? *(uint*)(bp->data + 488) : 0xFFFFFFFFu;
    n = ok ? *(uint*)(bp->data + 492) : 0xFFFFFFFFu;
    brelse(bp);
    if (!ok || c == 0xFFFFFFFFu || c > v->max_cluster - 1)
        return -1;
    if (n < 2 || n > v->max_cluster)
        n = 2;
    *free_count = c;
    *next = n;
    return 0;
}

void fat32_init(int dev) {
    struct buf *bp;
    struct fat32_volume *v;
    uint total_sectors, fat_entries;

    if (vol(dev) != 0)
        return;
    for (v = fat_vols; v != 0 && v->valid; v = v->next)
        ;
    if (v == 0) {
        v = malloc(sizeof(*v), M_DEVBUF, M_WAITOK | M_ZERO);
        v->next = fat_vols;
        fat_vols = v;
    }

    bp = bread(dev, 0);
    if ((bp->flags & B_ERROR) || !fat32_taste(bp->data, 0, 0)) {
        brelse(bp);
        return;
    }
    memmove(&v->bpb, bp->data, sizeof(struct fat32_bpb));
    brelse(bp);

    v->fat_size = v->bpb.fat_size_16 != 0 ? v->bpb.fat_size_16
                                          : v->bpb.fat_size_32;
    if (v->bpb.bytes_per_sector != BSIZE || v->bpb.sectors_per_cluster == 0 ||
        v->fat_size == 0 || v->bpb.fat_count == 0)
        return;

    v->fat_base = v->bpb.reserved_sectors;
    v->root_sectors = (v->bpb.root_entry_count * 32 + BSIZE - 1) / BSIZE;
    v->root_sector = v->fat_base + v->bpb.fat_count * v->fat_size;
    v->data_start_sector = v->root_sector + v->root_sectors;
    v->sectors_per_cluster = v->bpb.sectors_per_cluster;

    total_sectors = v->bpb.total_sectors_16 != 0 ? v->bpb.total_sectors_16
                                                 : v->bpb.total_sectors_32;
    if (total_sectors <= v->data_start_sector)
        return;
    v->max_cluster =
        (total_sectors - v->data_start_sector) / v->sectors_per_cluster + 1;
    v->fat_type = v->max_cluster - 1 < 4085 ? 12 : v->max_cluster - 1 < 65525 ? 16 : 32;
    if (v->fat_type == 32 && (v->bpb.fat_size_32 == 0 || v->bpb.root_entry_count != 0))
        return;
    v->fat_copies = v->fat_type == 32 && (v->bpb.ext_flags & 0x80) ? 1 : v->bpb.fat_count;
    v->fat_start_sector =
        v->fat_base + (v->fat_copies == 1 && v->bpb.fat_count > 1
                           ? ((v->bpb.ext_flags & 0x0F) % v->bpb.fat_count)
                           : 0) *
                          v->fat_size;
    fat_entries = v->fat_type == 32 ? v->fat_size * (BSIZE / 4)
                  : v->fat_type == 16 ? v->fat_size * (BSIZE / 2)
                                      : v->fat_size * BSIZE * 2 / 3;
    if (fat_entries > 0x0FFFFFF0)
        fat_entries = 0x0FFFFFF0;
    if (v->max_cluster >= fat_entries)
        v->max_cluster = fat_entries - 1;
    if (v->max_cluster < 2)
        return;

    v->dev = dev;
    v->valid = 1;
    v->dirty = 0;
    if (fsinfo_believe(v, &v->free_clusters, &v->next_free) < 0) {
        v->next_free = 2;
        v->free_clusters = count_free_clusters(v);
    }
}

int fat32_statfs(int dev, struct fnu_fsstat *st) {
    struct fat32_volume *v = vol(dev);

    if (v == 0)
        return -1;
    st->bsize = st->iosize = v->sectors_per_cluster * BSIZE;
    st->namemax = FAT32_NAME_MAX;
    st->blocks = v->max_cluster - 1;
    st->bfree = v->free_clusters;
    st->bavail = (int64_t)st->bfree;
    return 0;
}

int fat32_taste(const void *sector, uint *volume_id, const uchar **label) {
    const struct fat32_bpb *bpb = sector;
    const struct fat16_tail *t = (const struct fat16_tail *)((const char *)sector + FAT16_TAIL_OFFSET);
    int wide = bpb->fat_size_16 == 0 && bpb->fat_size_32 != 0;
    uchar sig = wide ? bpb->boot_signature : t->boot_signature;

    if (bpb->bytes_per_sector != BSIZE || bpb->sectors_per_cluster == 0 ||
        bpb->fat_count == 0 || (sig != 0x28 && sig != 0x29))
        return 0;
    if (volume_id)
        *volume_id = wide ? bpb->volume_id : t->volume_id;
    if (label)
        *label = wide ? bpb->volume_label : t->volume_label;
    return 1;
}

uint fat32_mode_of(uchar attr) {
    if (attr & FAT32_ATTR_DIRECTORY)
        return 0755;
    return (attr & FAT32_ATTR_READ_ONLY) ? 0444 : 0644;
}

uint fat32_cluster_bytes(int dev) {
    struct fat32_volume *v = vol(dev);

    return v == 0 ? 0 : v->sectors_per_cluster * BSIZE;
}

uint fat32_root_cluster(int dev) {
    struct fat32_volume *v = vol(dev);

    if (v == 0)
        return 0;
    return v->fat_type == 32 ? v->bpb.root_cluster : FAT_ROOT_FIXED;
}

int fat32_read_cluster(int dev, uint cluster, char *buf, uint n) {
    struct fat32_volume *v;

    v = vol(dev);
    if (v == 0 || !v->valid || cluster < 2 || cluster > v->max_cluster)
        return -1;

    uint sector = cluster_to_sector(dev, cluster);
    for (uint i = 0; i < v->sectors_per_cluster && n > 0; i++) {
        struct buf *bp = bread(dev, sector + i);
        uint to_read = (n > BSIZE) ? BSIZE : n;
        memmove(buf, bp->data, to_read);
        brelse(bp);
        buf += to_read;
        n -= to_read;
    }
    return 0;
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

// the long name checksum
static uchar shortname_checksum(const uchar name[11]) {
    uchar sum = 0;
    int i;

    for (i = 0; i < 11; i++)
        sum = (uchar)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name[i]);
    return sum;
}

static void lfn_chars(const struct fat32_lfn *l, ushort *out) {
    int i;

    for (i = 0; i < 5; i++)
        out[i] = (ushort)(l->name1[i * 2] | (l->name1[i * 2 + 1] << 8));
    for (i = 0; i < 6; i++)
        out[5 + i] = (ushort)(l->name2[i * 2] | (l->name2[i * 2 + 1] << 8));
    for (i = 0; i < 2; i++)
        out[11 + i] = (ushort)(l->name3[i * 2] | (l->name3[i * 2 + 1] << 8));
}

static void lfn_put(struct fat32_lfn *l, const ushort *in) {
    int i;

    for (i = 0; i < 5; i++) {
        l->name1[i * 2] = (uchar)(in[i] & 0xff);
        l->name1[i * 2 + 1] = (uchar)(in[i] >> 8);
    }
    for (i = 0; i < 6; i++) {
        l->name2[i * 2] = (uchar)(in[5 + i] & 0xff);
        l->name2[i * 2 + 1] = (uchar)(in[5 + i] >> 8);
    }
    for (i = 0; i < 2; i++) {
        l->name3[i * 2] = (uchar)(in[11 + i] & 0xff);
        l->name3[i * 2 + 1] = (uchar)(in[11 + i] >> 8);
    }
}

// UCS-2
static int ucs2_to_utf8(const ushort *src, int n, char *dst, int size) {
    int i, len = 0;

    for (i = 0; i < n; i++) {
        ushort c = src[i];

        if (c == 0)
            break;
        if (c >= 0xd800 && c < 0xe000)
            c = '?';
        if (c < 0x80) {
            if (len + 1 >= size)
                return -1;
            dst[len++] = (char)c;
        } else if (c < 0x800) {
            if (len + 2 >= size)
                return -1;
            dst[len++] = (char)(0xc0 | (c >> 6));
            dst[len++] = (char)(0x80 | (c & 0x3f));
        } else {
            if (len + 3 >= size)
                return -1;
            dst[len++] = (char)(0xe0 | (c >> 12));
            dst[len++] = (char)(0x80 | ((c >> 6) & 0x3f));
            dst[len++] = (char)(0x80 | (c & 0x3f));
        }
    }
    dst[len] = 0;
    return len;
}

static int utf8_to_ucs2(const char *src, ushort *dst, int max) {
    int n = 0;

    while (*src != 0) {
        uchar c = (uchar)*src++;
        uint cp;

        if (c < 0x80)
            cp = c;
        else if ((c & 0xe0) == 0xc0) {
            cp = (uint)(c & 0x1f) << 6;
            if ((*src & 0xc0) != 0x80)
                return -1;
            cp |= (uchar)*src++ & 0x3f;
        } else if ((c & 0xf0) == 0xe0) {
            cp = (uint)(c & 0x0f) << 12;
            if ((*src & 0xc0) != 0x80)
                return -1;
            cp |= (uint)((uchar)*src++ & 0x3f) << 6;
            if ((*src & 0xc0) != 0x80)
                return -1;
            cp |= (uchar)*src++ & 0x3f;
        } else
            return -1;
        if (cp > 0xffff)
            return -1;
        if (n >= max)
            return -1;
        dst[n++] = (ushort)cp;
    }
    return n;
}

// matched without regard to case
static int namecmp_ci(const char *a, const char *b) {
    while (*a != 0 && *b != 0) {
        int ca = to_upper(*a), cb = to_upper(*b);

        if (ca != cb)
            return ca - cb;
        a++, b++;
    }
    return to_upper(*a) - to_upper(*b);
}

static int shortname_taken(struct inode *dp, const uchar want[11]) {
    struct fat32_dirent de;
    struct fat32_dirbuf cache;
    uint off = 0;

    cache.valid = 0;
    for (;;) {
        if (!dir_entry(dp, &cache, off, &de))
            return 0;
        off += sizeof(de);
        if (de.name[0] == 0)
            return 0;
        if (de.name[0] == 0xE5 || de.attr == FAT32_ATTR_LONG_NAME)
            continue;
        if (memcmp(de.name, want, 11) == 0)
            return 1;
    }
}

#define TAILS 4096

static int short_unique(struct inode *dp, const char *name,
                        uchar out[11]) {
    const char *dot = 0, *p;
    char base[8], ext[3];
    int nb = 0, ne = 0, n;
    uint tail;

    for (p = name; *p; p++)
        if (*p == '.')
            dot = p;
    for (p = name; *p && (dot == 0 || p < dot); p++) {
        uchar c = (uchar)*p;

        if (c == ' ' || c == '.')
            continue;
        if (nb == 6)
            break;
        if (c >= 0x80 || c == '+' || c == ',' || c == ';' || c == '=' ||
            c == '[' || c == ']')
            c = '_';
        base[nb++] = (char)to_upper((char)c);
    }
    if (nb == 0)
        base[nb++] = '_';
    if (dot != 0)
        for (p = dot + 1; *p && ne < 3; p++) {
            uchar c = (uchar)*p;

            if (c >= 0x80 || c == ' ' || c == '.')
                c = '_';
            ext[ne++] = (char)to_upper((char)c);
        }

    uchar taken[TAILS / 8];
    struct fat32_dirent de;
    struct fat32_dirbuf cache;
    uint off = 0;

    memset(taken, 0, sizeof(taken));
    cache.valid = 0;
    for (;;) {
        int p, nd, i;
        uint t = 0;

        if (!dir_entry(dp, &cache, off, &de))
            break;
        off += sizeof(de);
        if (de.name[0] == 0)
            break;
        if (de.name[0] == 0xE5 || de.attr == FAT32_ATTR_LONG_NAME)
            continue;
        for (i = 0; i < 3; i++)
            if (de.name[8 + i] != (uchar)(i < ne ? ext[i] : ' '))
                break;
        if (i < 3)
            continue;
        for (p = 0; p < 8 && de.name[p] != '~'; p++)
            ;
        if (p == 8)
            continue;
        for (nd = 0, i = p + 1; i < 8 && de.name[i] != ' '; i++, nd++) {
            if (de.name[i] < '0' || de.name[i] > '9')
                break;
            t = t * 10 + (de.name[i] - '0');
        }
        if (i < 8 && de.name[i] != ' ')
            continue;
        if (nd == 0 || t == 0 || t >= TAILS)
            continue;
        n = nb;
        if (n + 1 + nd > 8)
            n = 8 - 1 - nd;
        if (n != p)
            continue;
        for (i = 0; i < n; i++)
            if (de.name[i] != (uchar)base[i])
                break;
        if (i == n)
            taken[t / 8] |= (uchar)(1 << (t % 8));
    }

    for (tail = 1; tail < 1000000; tail++) {
        char digits[8];
        int nd = 0, i;
        uint t = tail;

        if (tail < TAILS && (taken[tail / 8] & (1 << (tail % 8))) != 0)
            continue;
        while (t != 0 && nd < 6) {
            digits[nd++] = (char)('0' + t % 10);
            t /= 10;
        }
        memset(out, ' ', 11);
        n = nb;
        if (n + 1 + nd > 8)
            n = 8 - 1 - nd;
        for (i = 0; i < n; i++)
            out[i] = (uchar)base[i];
        out[n++] = '~';
        for (i = nd - 1; i >= 0; i--)
            out[n++] = (uchar)digits[i];
        for (i = 0; i < ne; i++)
            out[8 + i] = (uchar)ext[i];
        if (tail < TAILS || !shortname_taken(dp, out))
            return 0;
    }
    return -1;
}

static char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// the 8.3 name as text
static int shortname_text(const uchar raw[11], uchar ntres, char *dst,
                          int size) {
    int i, n = 0;

    for (i = 0; i < 8 && raw[i] != ' '; i++) {
        if (n + 2 >= size)
            return -1;
        dst[n] = (char)raw[i];
        if (ntres & FAT32_LCASE_BASE)
            dst[n] = to_lower(dst[n]);
        n++;
    }
    if (raw[8] != ' ') {
        if (n + 2 >= size)
            return -1;
        dst[n++] = '.';
        for (i = 8; i < 11 && raw[i] != ' '; i++) {
            if (n + 2 >= size)
                return -1;
            dst[n] = (char)raw[i];
            if (ntres & FAT32_LCASE_EXT)
                dst[n] = to_lower(dst[n]);
            n++;
        }
    }
    dst[n] = 0;
    return n;
}

static int short_form(const char *name, uchar out[11]) {
    const char *dot = 0, *p;
    int nb = 0, ne = 0, ntres = 0;
    int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;

    for (p = name; *p; p++)
        if (*p == '.') {
            if (dot != 0)
                return -1;
            dot = p;
        }
    if (name[0] == 0 || name[0] == ' ')
        return -1;
    memset(out, ' ', 11);
    for (p = name; *p && (dot == 0 || p < dot); p++) {
        uchar c = (uchar)*p;

        if (c < 0x20 || c >= 0x80 || c == ' ' || c == '+' || c == ',' ||
            c == ';' || c == '=' || c == '[' || c == ']')
            return -1;
        if (c >= 'a' && c <= 'z')
            base_lower = 1;
        else if (c >= 'A' && c <= 'Z')
            base_upper = 1;
        if (nb == 8)
            return -1;
        out[nb++] = (uchar)to_upper((char)c);
    }
    if (nb == 0)
        return -1;
    if (dot != 0)
        for (p = dot + 1; *p; p++) {
            uchar c = (uchar)*p;

            if (c < 0x20 || c >= 0x80 || c == ' ' || c == '.' || c == '+' ||
                c == ',' || c == ';' || c == '=' || c == '[' || c == ']')
                return -1;
            if (c >= 'a' && c <= 'z')
                ext_lower = 1;
            else if (c >= 'A' && c <= 'Z')
                ext_upper = 1;
            if (ne == 3)
                return -1;
            out[8 + ne++] = (uchar)to_upper((char)c);
        }
    if (base_lower && base_upper)
        return -1;
    if (ext_lower && ext_upper)
        return -1;
    if (base_lower)
        ntres |= FAT32_LCASE_BASE;
    if (ext_lower)
        ntres |= FAT32_LCASE_EXT;
    return ntres;
}

static int dir_entry(struct inode *dp, struct fat32_dirbuf *cache, uint off,
                     struct fat32_dirent *de) {
    uint blk = off / BSIZE * BSIZE;

    if (cache == 0)
        return fat32_read(dp, (char *)de, off, sizeof(*de)) == (int)sizeof(*de);
    if (!cache->valid || cache->off != blk) {
        int n = fat32_read(dp, cache->data, blk, BSIZE);

        if (n < (int)((off - blk) + sizeof(*de)))
            return 0;
        cache->off = blk;
        cache->valid = 1;
        cache->len = (uint)n;
    }
    if (off - blk + sizeof(*de) > cache->len)
        return 0;
    memmove(de, cache->data + (off - blk), sizeof(*de));
    return 1;
}

int fat32_readdir(struct inode *dp, struct fat32_dirbuf *cache, uint *at,
                  struct fat32_dirent *out, char *name, int size) {
    ushort units[FAT32_LFN_MAX * FAT32_LFN_CHARS];
    struct fat32_dirent de;
    uint off = *at;
    int have = 0, expect = 0;
    uchar want_sum = 0;

    for (;;) {
        if (!dir_entry(dp, cache, off, &de))
            return -1;
        off += sizeof(de);

        if (de.name[0] == 0)
            return 0;
        if (de.name[0] == 0xE5) {
            have = expect = 0;
            continue;
        }
        if (de.attr == FAT32_ATTR_LONG_NAME) {
            const struct fat32_lfn *l = (const struct fat32_lfn *)&de;
            int seq = l->ord & 0x1f;

            if (seq < 1 || seq > FAT32_LFN_MAX) {
                have = expect = 0;
                continue;
            }
            if (l->ord & FAT32_LFN_LAST) {
                have = seq * FAT32_LFN_CHARS;
                expect = seq;
                want_sum = l->chksum;
            } else if (seq != expect || l->chksum != want_sum) {
                have = expect = 0;
                continue;
            }
            lfn_chars(l, units + (seq - 1) * FAT32_LFN_CHARS);
            expect = seq - 1;
            continue;
        }
        if (de.attr & FAT32_ATTR_VOLUME_ID) {
            have = expect = 0;
            continue;
        }

        if (have > 0 && expect == 0 &&
            shortname_checksum(de.name) == want_sum) {
            if (ucs2_to_utf8(units, have, name, size) > 0) {
                if (out)
                    *out = de;
                *at = off;
                return 1;
            }
        }
        if (shortname_text(de.name, de.reserved, name, size) < 0)
            return -1;
        if (out)
            *out = de;
        *at = off;
        return 1;
    }
}

uint fat32_lookup(struct inode *dp, char *name, struct fat32_dirent *res, uint *off) {
    struct fat32_volume *v;
    struct fat32_dirent de;
    struct fat32_dirbuf cache;
    char found[NAMEBUFSZ];
    uint at = 0, prev;
    int r;

    v = vol(dp->dev);
    if (v == 0)
        return 0;

    cache.valid = 0;
    for (;;) {
        prev = at;
        r = fat32_readdir(dp, &cache, &at, &de, found, sizeof(found));
        if (r < 0)
            return 0;
        if (r == 0) {
            if (off)
                *off = at;
            return 0;
        }
        if (namecmp_ci(found, name) != 0)
            continue;
        if (res)
            *res = de;
        if (off)
            *off = at - sizeof(de);
        (void)prev;
        return fat32_dirent_cluster(dp->dev, &de);
    }
}

struct inode* fat32_ialloc(uint dev, short type) {
    static uint temp_inum = 0x40000000;
    struct inode *ip = iget(dev, temp_inum++);
    if (temp_inum > 0x7FFFFFFF) temp_inum = 0x40000000;
    if (ip == 0)
        return 0;
    ip->type = type;
    ip->fat32_info.cluster = 0;
    ip->fat32_info.attr = (type == FNU_TYPE_DIR) ? FAT32_ATTR_DIRECTORY : 0;
    ip->size = 0;
    ip->uid = 0;
    ip->gid = 0;
    ip->mode = fat32_mode_of(ip->fat32_info.attr);
    ip->nlink = 1;
    ip->valid = 1;
    return ip;
}

int fat32_isdirempty(struct inode *dp) {
    struct fat32_volume *v;
    int dev = dp->dev;

    v = vol(dev);
    if (v == 0)
        return 0;

    if (fixed_root(v, dp)) return 0;

    uint current_cluster = dp->fat32_info.cluster;
    uint steps = 0;
    while (!is_eof(current_cluster) && steps++ <= v->max_cluster) {
        uint sector = cluster_to_sector(dev, current_cluster);
        for (uint i = 0; i < v->sectors_per_cluster; i++) {
            struct buf *bp = bread(dp->dev, sector + i);
            struct fat32_dirent *entries = (struct fat32_dirent*)bp->data;
            for (uint j = 0; j < BSIZE / sizeof(struct fat32_dirent); j++) {
                if (entries[j].name[0] != 0 && entries[j].name[0] != 0xE5 &&
                    (entries[j].attr & FAT32_ATTR_VOLUME_ID) == 0) {
                    if (strncmp((char*)entries[j].name, ".          ", 11) == 0 ||
                        strncmp((char*)entries[j].name, "..         ", 11) == 0) {
                        continue;
                    }
                    brelse(bp);
                    return 0;
                }
            }
            brelse(bp);
        }
        current_cluster = next_cluster(dp->dev, current_cluster);
    }
    return 1;
}

static void free_cluster_chain(int dev, uint cluster) {
    struct fat32_volume *v = vol(dev);

    while (!is_eof(cluster) && cluster != 0) {
        uint next;

        if (fat_entry(dev, cluster) == 0)
            break;
        next = next_cluster(dev, cluster);
        set_next_cluster(dev, cluster, 0);
        if (v != 0) {
            if (v->free_clusters < v->max_cluster - 1)
                v->free_clusters++;
            if (cluster < v->next_free)
                v->next_free = cluster;
        }
        cluster = next;
    }
}

static int fat32_entry_remove(struct inode *dp, uint off) {
    struct fat32_volume *v;
    int dev = dp->dev;

    v = vol(dev);
    if (v == 0)
        return -1;

    uint current_cluster = dp->fat32_info.cluster;
    uint bytes_per_cluster = v->sectors_per_cluster * BSIZE;
    uint total_off = 0, sector, sec_off;

    if (fixed_root(v, dp)) {
        if (off >= v->root_sectors * BSIZE)
            return -1;
        sector = v->root_sector + off / BSIZE;
        sec_off = off % BSIZE;
    } else {
        while (off >= total_off + bytes_per_cluster) {
            current_cluster = next_cluster(dp->dev, current_cluster);
            if (is_eof(current_cluster)) return -1;
            total_off += bytes_per_cluster;
        }
        sector = cluster_to_sector(dev, current_cluster) + (off - total_off) / BSIZE;
        sec_off = (off - total_off) % BSIZE;
    }

    struct buf *bp = bread(dp->dev, sector);
    struct fat32_dirent *de = (struct fat32_dirent*)(bp->data + sec_off);

    uchar sum = shortname_checksum(de->name);
    de->name[0] = 0xE5;
    bdirty(bp);
    brelse(bp);

    {
        struct fat32_dirent slot;
        uint at = off;
        int seq;

        for (seq = 1; seq <= FAT32_LFN_MAX && at >= sizeof(slot); seq++) {
            const struct fat32_lfn *l = (const struct fat32_lfn *)&slot;
            uchar ord;

            at -= sizeof(slot);
            if (fat32_read(dp, (char *)&slot, at,
                           sizeof(slot)) != (int)sizeof(slot))
                break;
            ord = l->ord;
            if (slot.attr != FAT32_ATTR_LONG_NAME || l->chksum != sum ||
                (ord & 0x1f) != seq)
                break;
            slot.name[0] = 0xE5;
            if (write_dirslot(dp, at, &slot) < 0)
                break;
            if (ord & FAT32_LFN_LAST)
                break;
        }
    }

    return 0;
}

void fat32_truncate(struct inode *ip) {
    if (ip->fat32_info.cluster != 0)
        free_cluster_chain(ip->dev, ip->fat32_info.cluster);
    ip->fat32_info.cluster = 0;
    fat32_fc_purge(ip);
    ip->size = 0;
}

int fat32_truncate_to(struct inode *ip, uint64_t length) {
    struct fat32_volume *v = vol(ip->dev);
    uint per, keep, c, n;

    if (v == 0)
        return -FNU_ERR_IO;
    if (length > 0xFFFFFFFFull)
        return -FNU_ERR_TOOLARGE;
    if (length == 0) {
        fat32_truncate(ip);
        return 0;
    }
    if (length > ip->size) {
        char *blank = page_alloc();
        uint at = ip->size;

        if (blank == 0)
            return -FNU_ERR_NOMEMORY;
        memset(blank, 0, PGSIZE);
        while (at < (uint)length) {
            uint chunk = (uint)length - at < PGSIZE ? (uint)length - at : PGSIZE;

            if (fat32_write(ip, blank, at, chunk) != (int)chunk) {
                page_free(blank);
                return -FNU_ERR_NOSPACE;
            }
            at += chunk;
            ip->size = at;
        }
        page_free(blank);
        return 0;
    }
    per = v->sectors_per_cluster * BSIZE;
    keep = (uint)((length + per - 1) / per);
    for (c = ip->fat32_info.cluster, n = 1; n < keep && !is_eof(c) && c != 0; n++)
        c = next_cluster(ip->dev, c);
    if (!is_eof(c) && c != 0) {
        uint rest = next_cluster(ip->dev, c);
        set_next_cluster(ip->dev, c, 0x0FFFFFFF);
        fat32_fc_purge(ip);
        if (!is_eof(rest) && rest != 0)
            free_cluster_chain(ip->dev, rest);
        if ((uint)length % per != 0) {
            char *blank = page_alloc();
            uint off = (uint)length % per, left = per - off;
            if (blank == 0)
                return -FNU_ERR_NOMEMORY;
            memset(blank, 0, PGSIZE);
            while (left > 0) {
                uint chunk = left < PGSIZE ? left : PGSIZE;
                if (fat32_write(ip, blank, (uint)(keep - 1) * per + off, chunk) != (int)chunk) {
                    page_free(blank);
                    return -FNU_ERR_IO;
                }
                off += chunk;
                left -= chunk;
            }
            page_free(blank);
        }
    }
    ip->size = (uint)length;
    return 0;
}

int fat32_dirunlink(struct inode *dp, uint off) {
    return fat32_entry_remove(dp, off);
}

static int write_dirslot(struct inode *dp, uint off,
                         const struct fat32_dirent *de) {
    if (fat32_write(dp, (char *)de, off, sizeof(*de)) != (int)sizeof(*de))
        return -1;
    return 0;
}

static int find_free_run(struct inode *dp, int slots, uint *start) {
    struct fat32_dirent de;
    struct fat32_dirbuf cache;
    uint off = 0, run_start = 0;
    int run = 0;

    cache.valid = 0;
    for (;;) {
        if (!dir_entry(dp, &cache, off, &de)) {
            *start = run == 0 ? off : run_start;
            return 0;
        }
        if (de.name[0] == 0) {
            *start = run == 0 ? off : run_start;
            return 0;
        }
        if (de.name[0] == 0xE5) {
            if (run == 0)
                run_start = off;
            if (++run == slots) {
                *start = run_start;
                return 0;
            }
        } else {
            run = 0;
        }
        off += sizeof(de);
    }
}

int fat32_dirlink(struct inode *dp, char *name, struct inode *ip, int fresh) {
    struct fat32_volume *v;
    int dev = dp->dev;

    v = vol(dev);
    if (v == 0)
        return -1;

    if (ip->type == FNU_TYPE_DIR && ip->fat32_info.cluster == 0) {
        if ((ip->fat32_info.cluster = alloc_cluster(dp->dev, 1)) == 0)
            return -1;
        fat32_fc_purge(ip);
        ip->size = v->sectors_per_cluster * BSIZE;

        struct fat32_dirent de;
        memset(&de, 0, sizeof(de));
        memset(de.name, ' ', 11);
        de.name[0] = '.';
        de.attr = FAT32_ATTR_DIRECTORY;
        de.first_cluster_high = (ip->fat32_info.cluster >> 16) & 0xFFFF;
        de.first_cluster_low = ip->fat32_info.cluster & 0xFFFF;
        fat32_write(ip, (char*)&de, 0, sizeof(de));

        memset(&de, 0, sizeof(de));
        memset(de.name, ' ', 11);
        de.name[0] = '.'; de.name[1] = '.';
        de.attr = FAT32_ATTR_DIRECTORY;
        uint parent_cluster = dp->fat32_info.cluster;
        if (parent_cluster == fat32_root_cluster(dev)) parent_cluster = 0;
        de.first_cluster_high = (parent_cluster >> 16) & 0xFFFF;
        de.first_cluster_low = parent_cluster & 0xFFFF;
        fat32_write(ip, (char*)&de, sizeof(de), sizeof(de));
    }

    uchar shortname[11];
    ushort units[FAT32_LFN_MAX * FAT32_LFN_CHARS];
    int nunits = 0, slots = 1, k;

    int ntres = short_form(name, shortname);

    if (ntres < 0) {
        nunits = utf8_to_ucs2(name, units, FAT32_LFN_MAX * FAT32_LFN_CHARS);
        if (nunits <= 0)
            return -1;
        if (short_unique(dp, name, shortname) < 0)
            return -1;
        slots = 1 + (nunits + FAT32_LFN_CHARS - 1) / FAT32_LFN_CHARS;
        ntres = 0;
    } else if (shortname_taken(dp, shortname)) {
        return -1;
    }

    uint start = 0;
    if (find_free_run(dp, slots, &start) < 0)
        return -1;

    if (nunits > 0) {
        uchar sum = shortname_checksum(shortname);
        int seq;

        int total = (slots - 1) * FAT32_LFN_CHARS;
        for (k = nunits; k < total; k++)
            units[k] = (k == nunits) ? 0 : 0xffff;
        for (seq = slots - 1; seq >= 1; seq--) {
            struct fat32_lfn l;

            memset(&l, 0, sizeof(l));
            l.ord = (uchar)(seq | (seq == slots - 1 ? FAT32_LFN_LAST : 0));
            l.attr = FAT32_ATTR_LONG_NAME;
            l.chksum = sum;
            lfn_put(&l, units + (seq - 1) * FAT32_LFN_CHARS);
            if (write_dirslot(dp, start + (uint)(slots - 1 - seq) *
                                          sizeof(struct fat32_dirent),
                              (const struct fat32_dirent *)&l) < 0)
                return -1;
        }
    }

    {
        struct fat32_dirent de;
        uint at = start + (uint)(slots - 1) * sizeof(struct fat32_dirent);

        memset(&de, 0, sizeof(de));
        memmove(de.name, shortname, 11);
        de.reserved = (uchar)ntres;
        de.attr = ip->fat32_info.attr;
        de.first_cluster_high = (ushort)((ip->fat32_info.cluster >> 16) & 0xFFFF);
        de.first_cluster_low = (ushort)(ip->fat32_info.cluster & 0xFFFF);
        de.file_size = (ip->fat32_info.attr & FAT32_ATTR_DIRECTORY) ? 0 : ip->size;
        if (fresh) {
            time_now(&ip->birthtime);
            ip->atime = ip->mtime = ip->ctime = ip->birthtime;
            ip->iflag = 0;
        }
        fat32_dirent_stamp(&de, &ip->atime, &ip->mtime, &ip->birthtime);
        if (write_dirslot(dp, at, &de) < 0)
            return -1;
        ip->fat32_info.parent_cluster = dp->fat32_info.cluster;
        ip->fat32_info.dir_off = at;
    }
    return 0;
}

void fat32_iupdate(struct inode *ip) {
    struct fat32_volume *v;
    int dev = ip->dev;

    v = vol(dev);
    if (v == 0)
        return;

    if (ip->inum == fat32_root_cluster(dev)) return;
    if (ip->nlink == 0) return;

    uint current_cluster = ip->fat32_info.parent_cluster;
    uint off = ip->fat32_info.dir_off;
    uint bytes_per_cluster = v->sectors_per_cluster * BSIZE;
    uint sector, sec_off = off % BSIZE;

    if (v->fat_type != 32 && current_cluster == FAT_ROOT_FIXED) {
        if (off >= v->root_sectors * BSIZE) return;
        sector = v->root_sector + off / BSIZE;
    } else {
        while (off >= bytes_per_cluster) {
            current_cluster = next_cluster(ip->dev, current_cluster);
            if (is_eof(current_cluster)) return;
            off -= bytes_per_cluster;
        }
        sector = cluster_to_sector(dev, current_cluster) + (off / BSIZE);
    }

    struct buf *bp = bread(ip->dev, sector);
    struct fat32_dirent *de = (struct fat32_dirent*)(bp->data + sec_off);
    struct fat32_dirent was = *de;

    de->file_size = (ip->fat32_info.attr & FAT32_ATTR_DIRECTORY) ? 0 : ip->size;
    de->first_cluster_high = (ip->fat32_info.cluster >> 16) & 0xFFFF;
    de->first_cluster_low = ip->fat32_info.cluster & 0xFFFF;
    de->attr = ip->fat32_info.attr;
    itimes(ip);
    fat32_dirent_stamp(de, &ip->atime, &ip->mtime, 0);

    if (memcmp(&was, de, sizeof(was)) != 0)
        bdirty(bp);
    brelse(bp);
}

#define FC_LASTMAP 0
#define FC_LASTFC 1

void fat32_fc_purge(struct inode *ip) {
    ip->fat32_info.fc_cluster[FC_LASTMAP] = 0;
    ip->fat32_info.fc_cluster[FC_LASTFC] = 0;
}

static void fc_set(struct inode *ip, int which, uint index, uint cluster) {
    ip->fat32_info.fc_index[which] = index;
    ip->fat32_info.fc_cluster[which] = cluster;
}

static uint pcbmap(struct inode *ip, uint want, int extend) {
    struct fat32_volume *v = vol(ip->dev);
    struct buf *bp = 0;
    uint cluster = ip->fat32_info.cluster, index = 0, i, sector = 0;

    if (v == 0 || cluster == 0)
        return 0x0FFFFFFF;
    for (i = 0; i < 2; i++)
        if (ip->fat32_info.fc_cluster[i] != 0 &&
            ip->fat32_info.fc_index[i] <= want &&
            ip->fat32_info.fc_index[i] >= index) {
            index = ip->fat32_info.fc_index[i];
            cluster = ip->fat32_info.fc_cluster[i];
        }
    while (index < want) {
        uint s = v->fat_start_sector + (cluster * 4) / BSIZE, next;

        if (v->fat_type != 32)
            next = fat_get_at(v, v->fat_start_sector, cluster);
        else {
            if (bp == 0 || sector != s) {
                if (bp != 0)
                    brelse(bp);
                bp = bread(ip->dev, s);
                sector = s;
            }
            next = *(uint *)(bp->data + (cluster * 4) % BSIZE) & 0x0FFFFFFF;
        }
        if (next < 2 || next > v->max_cluster) {
            if (bp != 0)
                brelse(bp);
            bp = 0;
            fc_set(ip, FC_LASTFC, index, cluster);
            if (!extend || (next = alloc_cluster(ip->dev, ip->type == FNU_TYPE_DIR)) == 0)
                return 0x0FFFFFFF;
            set_next_cluster(ip->dev, cluster, next);
            fc_set(ip, FC_LASTFC, index + 1, next);
            if (ip->type == FNU_TYPE_DIR && ip->size < (index + 2) * v->sectors_per_cluster * BSIZE)
                ip->size = (index + 2) * v->sectors_per_cluster * BSIZE;
        }
        cluster = next;
        index++;
    }
    if (bp != 0)
        brelse(bp);
    fc_set(ip, FC_LASTMAP, index, cluster);
    return cluster;
}

int fat32_read_uio(struct inode *ip, struct uio *uio) {
    struct fat32_volume *v = vol(ip->dev);
    uint off = (uint)uio->uio_offset, n = (uint)uio->uio_resid;
    uint bytes_per_cluster, current_cluster, index, tot = 0;

    if (v == 0)
        return 0;
    if (!v->valid)
        return -1;
    if (fixed_root(v, ip))
        return root_copy(v, uio, off, n, 0);
    bytes_per_cluster = v->sectors_per_cluster * BSIZE;
    index = off / bytes_per_cluster;
    current_cluster = pcbmap(ip, index, 0);
    if (is_eof(current_cluster))
        return 0;
    off %= bytes_per_cluster;

    while (n > 0 && !is_eof(current_cluster)) {
        uint sector = cluster_to_sector(ip->dev, current_cluster);
        uint cluster_off = off;
        uint cluster_rem = bytes_per_cluster - cluster_off;
        uint to_read = (n > cluster_rem) ? cluster_rem : n;

        for (uint i = cluster_off / BSIZE; i < v->sectors_per_cluster && to_read > 0; i++) {
            struct buf *bp = bread(ip->dev, sector + i);
            uint sec_off = cluster_off % BSIZE;
            uint sec_rem = BSIZE - sec_off;
            uint chunk = (to_read > sec_rem) ? sec_rem : to_read;

            if (uiomove(bp->data + sec_off, chunk, uio) != 0) {
                brelse(bp);
                return tot > 0 ? (int)tot : -1;
            }
            if (chunk == BSIZE)
                bp->flags |= B_AGE;
            brelse(bp);

            n -= chunk;
            to_read -= chunk;
            tot += chunk;
            cluster_off = 0;
        }
        if (n > 0)
            current_cluster = pcbmap(ip, ++index, 0);
        off = 0;
    }
    return tot;
}

int fat32_read(struct inode *ip, char *dst, uint off, uint n) {
    struct kiovec iov;
    struct uio uio;

    uio_prepare(&uio, &iov, dst, n, off, UIO_SYSSPACE, UIO_READ);
    return fat32_read_uio(ip, &uio);
}

int fat32_write_uio(struct inode *ip, struct uio *uio) {
    struct fat32_volume *v = vol(ip->dev);
    uint off = (uint)uio->uio_offset, n = (uint)uio->uio_resid;
    uint bytes_per_cluster, current_cluster, index, tot = 0, pos = off;

    if (v == 0)
        return 0;
    if (!v->valid)
        return -1;
    if (fixed_root(v, ip)) {
        int r = root_copy(v, uio, off, n, 1);
        return r == (int)n ? r : -1;
    }
    if (ip->fat32_info.cluster == 0) {
        if ((ip->fat32_info.cluster = alloc_cluster(ip->dev, ip->type == FNU_TYPE_DIR)) == 0)
            return -1;
        fat32_fc_purge(ip);
    }
    bytes_per_cluster = v->sectors_per_cluster * BSIZE;
    index = off / bytes_per_cluster;
    current_cluster = pcbmap(ip, index, 1);
    if (is_eof(current_cluster))
        return -1;
    off %= bytes_per_cluster;

    while (n > 0) {
        uint sector = cluster_to_sector(ip->dev, current_cluster);
        uint cluster_off = off;
        uint cluster_rem = bytes_per_cluster - cluster_off;
        uint to_write = (n > cluster_rem) ? cluster_rem : n;

        for (uint i = cluster_off / BSIZE; i < v->sectors_per_cluster && to_write > 0; i++) {
            uint sec_off = cluster_off % BSIZE;
            uint sec_rem = BSIZE - sec_off;
            uint chunk = (to_write > sec_rem) ? sec_rem : to_write;
            struct buf *bp;

            if (chunk == BSIZE) {
                bp = bgetblk(ip->dev, sector + i);
            } else if (ip->type != FNU_TYPE_DIR && pos - sec_off >= ip->size) {
                bp = bgetblk(ip->dev, sector + i);
                memset(bp->data, 0, BSIZE);
            } else {
                bp = bread(ip->dev, sector + i);
            }
            if (uiomove(bp->data + sec_off, chunk, uio) != 0) {
                brelse(bp);
                return tot > 0 ? (int)tot : -1;
            }
            if (chunk == BSIZE)
                bp->flags |= B_AGE;
            bdirty(bp);
            brelse(bp);

            pos += chunk;
            n -= chunk;
            to_write -= chunk;
            tot += chunk;
            cluster_off = 0;
        }
        if (n > 0) {
            current_cluster = pcbmap(ip, ++index, 1);
            if (is_eof(current_cluster))
                return (int)tot;
            off = 0;
        }
    }
    return tot;
}

int fat32_write(struct inode *ip, char *src, uint off, uint n) {
    struct kiovec iov;
    struct uio uio;

    uio_prepare(&uio, &iov, src, n, off, UIO_SYSSPACE, UIO_WRITE);
    return fat32_write_uio(ip, &uio);
}
