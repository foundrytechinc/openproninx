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

    // Extended BPB for FAT32
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
} __attribute__((packed));

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
} __attribute__((packed));

// Attributes
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LONG_NAME 0x0F

#endif
