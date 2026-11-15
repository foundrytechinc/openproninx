#ifndef PRONINX_FAT32_H
#define PRONINX_FAT32_H

#include "inc/types.h"

// FAT32 Boot Sector / BPB
struct fat32_bpb {
    uchar jmp[3];
    uchar oem[8];
    ushort bytes_per_sector;
    uchar sectors_per_cluster;
    ushort reserved_sectors;
    uchar fat_count;
    ushort root_entry_count;
    ushort total_sectors_16;
    uchar media_type;
    ushort fat_size_16;
    ushort sectors_per_track;
    ushort head_count;
    uint hidden_sectors;
    uint total_sectors_32;

    uint fat_size_32;
    ushort ext_flags;
    ushort fs_version;
    uint root_cluster;
    ushort fs_info;
    ushort backup_boot_sector;
    uchar reserved[12];
    uchar drive_number;
    uchar reserved1;
    uchar boot_signature;
    uint volume_id;
    uchar volume_label[11];
    uchar fs_type[8];
} __packed;

// FAT12
struct fat16_tail {
    uchar drive_number;
    uchar reserved1;
    uchar boot_signature;
    uint volume_id;
    uchar volume_label[11];
    uchar fs_type[8];
} __packed;
#define FAT16_TAIL_OFFSET 36

// FAT32 Directory Entry
struct fat32_dirent {
    uchar name[11];
    uchar attr;
    uchar reserved;
    uchar create_time_tenth;
    ushort create_time;
    ushort create_date;
    ushort last_access_date;
    ushort first_cluster_high;
    ushort write_time;
    ushort write_date;
    ushort first_cluster_low;
    uint file_size;
} __packed;

struct fat32_lfn {
    uchar ord;
    uchar name1[10];
    uchar attr;
    uchar type;
    uchar chksum;
    uchar name2[12];
    ushort first_cluster_low;
    uchar name3[4];
} __packed;

#define FAT32_LCASE_BASE 0x08
#define FAT32_LCASE_EXT 0x10

#define FAT32_LFN_LAST 0x40
#define FAT32_LFN_CHARS 13
#define FAT32_LFN_MAX 20
#define FAT32_NAME_MAX 255 // WIN_MAXLEN: the longest name

struct inode;
struct fat32_dirbuf {
  int valid;
  uint off, len;
  char data[512];
};
int fat32_dirunlink(struct inode *dp, uint off);
int fat32_readdir(struct inode *dp, struct fat32_dirbuf *cache, uint *at,
                  struct fat32_dirent *out, char *name, int size);

struct fnu_timespec;
void fat32_dirent_times(const struct fat32_dirent *de, struct fnu_timespec *a,
                        struct fnu_timespec *m, struct fnu_timespec *b);
void fat32_dirent_stamp(struct fat32_dirent *de, const struct fnu_timespec *a,
                        const struct fnu_timespec *m,
                        const struct fnu_timespec *b);

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LONG_NAME 0x0F

struct uio;
struct fnu_fsstat;
void fat32_init(int dev);
uint fat32_root_cluster(int dev);
int fat32_taste(const void *sector, uint *volume_id, const uchar **label);
uint fat32_dirent_cluster(int dev, const struct fat32_dirent *de);
uint fat32_cluster_bytes(int dev);
uint fat32_dir_size(struct inode *ip);
void fat32_root_times(struct inode *ip);
uint fat32_mode_of(uchar attr);
int fat32_present(int dev);
void fat32_forget(int dev);
void fat32_quiesce(int dev);
void fat32_truncate(struct inode *ip);
int fat32_truncate_to(struct inode *ip, uint64_t length);
void fat32_renumber(struct inode *ip);
void fat32_sync(int dev);
int fat32_statfs(int dev, struct fnu_fsstat *st);
int fat32_read_cluster(int dev, uint cluster, char *buf, uint n);
int fat32_read(struct inode *ip, char *dst, uint off, uint n);
int fat32_write(struct inode *ip, char *src, uint off, uint n);
int fat32_read_uio(struct inode *ip, struct uio *uio);
int fat32_write_uio(struct inode *ip, struct uio *uio);
void fat32_fc_purge(struct inode *ip);
void fat32_iupdate(struct inode *ip);
uint fat32_lookup(struct inode *dp, char *name, struct fat32_dirent *res,
                  uint *off);
int fat32_dirlink(struct inode *dp, char *name, struct inode *ip, int fresh);
int fat32_isdirempty(struct inode *dp);
struct inode *fat32_ialloc(uint dev, short type);

#endif
