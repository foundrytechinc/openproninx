/*-
 * SPDX-License-Identifier: BSD-3-Clause
 */

// the layout of FreeBSD fs.h and dinode.h, by byte offset
#ifndef FNU_PRONINX_UFS2_H
#define FNU_PRONINX_UFS2_H

#include "inc/abi.h"
#include "blockdev.h"
#include "sleeplock.h"

#define UFS2_SUPERBLOCK_OFFSET 65536
#define UFS2_SUPERBLOCK_SIZE 8192
#define UFS2_MAGIC 0x19540119U
#define UFS2_ROOT_INO 2
#define UFS2_DIRECT_BLOCKS 12
#define UFS2_INDIRECT_BLOCKS 3
#define UFS2_MAX_NAME 255
#define UFS2_DIRTYPE_FIFO 1
#define UFS2_DIRTYPE_DIR 4
#define UFS2_DIRTYPE_REG 8
#define UFS2_DIRTYPE_LNK 10
#define UFS2_DIRTYPE_SOCK 12
#define MAXVOLLEN 32
#define UFS2_MAXFRAG 8

/* struct fs, by byte */
#define UFS2_SB_sblkno 8
#define UFS2_SB_cblkno 12
#define UFS2_SB_iblkno 16
#define UFS2_SB_dblkno 20
#define UFS2_SB_ncg 44
#define UFS2_SB_bsize 48
#define UFS2_SB_fsize 52
#define UFS2_SB_frag 56
#define UFS2_SB_minfree 60
#define UFS2_SB_maxbpg 92
#define UFS2_SB_bshift 80
#define UFS2_SB_fshift 84
#define UFS2_SB_fragshift 96
#define UFS2_SB_fsbtodb 100
#define UFS2_SB_sbsize 104
#define UFS2_SB_nindir 116
#define UFS2_SB_optim 128
#define UFS2_SB_inopb 120
#define UFS2_SB_id 144
#define UFS2_SB_cssize 156
#define UFS2_SB_cgsize 160
#define UFS2_SB_ipg 184
#define UFS2_SB_fpg 188
#define UFS2_SB_fmod 208
#define UFS2_SB_clean 209
#define UFS2_SB_ronly 210
#define UFS2_SB_fsmnt 212
#define UFS2_SB_volname 680
#define UFS2_SB_cgrotor 724
#define UFS2_SB_metaspace 880
#define UFS2_SB_sblockloc 1000
#define UFS2_SB_cstotal 1008
#define UFS2_SB_time 1072
#define UFS2_SB_size 1080
#define UFS2_SB_dsize 1088
#define UFS2_SB_csaddr 1096
#define UFS2_SB_pendingblocks 1104
#define UFS2_SB_pendinginodes 1112
#define UFS2_SB_avgfilesize 1196
#define UFS2_SB_avgfpdir 1200
#define UFS2_SB_mtime 1208
#define UFS2_SB_ckhash 1304
#define UFS2_SB_metackhash 1308
#define UFS2_SB_flags 1312
#define UFS2_SB_contigsumsize 1316
#define UFS2_SB_maxsymlinklen 1320
#define UFS2_SB_maxfilesize 1328
#define UFS2_SB_state 1352
#define UFS2_SB_magic 1372

#define UFS2_FS_UNCLEAN 0x00000001
#define UFS2_FS_DOSOFTDEP 0x00000002
#define UFS2_FS_NEEDSFSCK 0x00000004
#define UFS2_FS_SUJ 0x00000008
#define UFS2_FS_GJOURNAL 0x00000040
#define UFS2_FS_FLAGS_UPDATED 0x00000080
#define UFS2_FS_METACKHASH 0x00000200

/* struct cg, by byte */
#define UFS2_CG_magic 4
#define UFS2_CG_old_time 8
#define UFS2_CG_cgx 12
#define UFS2_CG_ndblk 20
#define UFS2_CG_cs 24
#define UFS2_CG_rotor 40
#define UFS2_CG_frotor 44
#define UFS2_CG_irotor 48
#define UFS2_CG_frsum 52
#define UFS2_CG_iusedoff 92
#define UFS2_CG_freeoff 96
#define UFS2_CG_nextfreeoff 100
#define UFS2_CG_clustersumoff 104
#define UFS2_CG_clusteroff 108
#define UFS2_CG_nclusterblks 112
#define UFS2_CG_niblk 116
#define UFS2_CG_initediblk 120
#define UFS2_CG_unrefs 124
#define UFS2_CG_ckhash 132
#define UFS2_CG_time 136
#define UFS2_CG_HEADER_SIZE 168
#define UFS2_CG_MAGIC 0x00090255U

/* struct ufs2_dinode, by byte */
#define UFS2_DINODE_SIZE 256
#define UFS2_DINODE_MODE 0
#define UFS2_DINODE_NLINK 2
#define UFS2_DINODE_UID 4
#define UFS2_DINODE_GID 8
#define UFS2_DINODE_BLKSIZE 12
#define UFS2_DINODE_SIZE_OFFSET 16
#define UFS2_DINODE_BLOCKS 24
#define UFS2_DINODE_ATIME 32
#define UFS2_DINODE_MTIME 40
#define UFS2_DINODE_CTIME 48
#define UFS2_DINODE_BIRTHTIME 56
#define UFS2_DINODE_MTIMENSEC 64
#define UFS2_DINODE_ATIMENSEC 68
#define UFS2_DINODE_CTIMENSEC 72
#define UFS2_DINODE_BIRTHNSEC 76
#define UFS2_DINODE_GEN 80
#define UFS2_DINODE_KERNFLAGS 84
#define UFS2_DINODE_FLAGS 88
#define UFS2_DINODE_EXTSIZE 92
#define UFS2_DINODE_EXTB 96
#define UFS2_DINODE_DIRECT_OFFSET 112
#define UFS2_DINODE_INDIRECT_OFFSET 208

#define UFS2_IFIFO 0010000
#define UFS2_IFSOCK 0140000
#define UFS2_IFDIR 0040000
#define UFS2_IFREG 0100000
#define UFS2_IFLNK 0120000
#define UFS2_IFMT 0170000

#define UFS2_DIR_HEADER_SIZE 8
#define UFS2_DIR_BLOCK_SIZE 512

struct ufs2_mount;
struct uio;

struct ufs2_volume {
  struct blockdev device;
  struct ufs2_mount *m;
  uint block_size;
  uint fragment_size;
  uint fragments_per_block;
  uint cylinder_groups;
  uint inodes_per_group;
  uint fragments_per_group;
  uint inodes_per_block;
  uint64_t data_fragments;
  uint64_t free_fragments;
  uint64_t inode_table_fragment;
  uint cylinder_group_block;
  uint cylinder_group_size;
};

struct ufs2_inode {
  uint16_t mode;
  uint16_t nlink;
  uint32_t uid;
  uint32_t gid;
  uint64_t size;
  uint64_t blocks;
  struct fnu_timespec atime;
  struct fnu_timespec mtime;
  struct fnu_timespec ctime;
  struct fnu_timespec birthtime;
  uint32_t gen;
  uint32_t flags;
  uint64_t direct[UFS2_DIRECT_BLOCKS];
  uint64_t indirect[UFS2_INDIRECT_BLOCKS];
};

int ufs2_probe(const struct blockdev *device, struct ufs2_volume *volume);
int64_t ufs2_time(uint dev);
void ufs2_init(void);
int ufs2_read_uio(const struct ufs2_volume *volume, uint inode_number,
                  const struct ufs2_inode *inode, struct uio *uio);
int ufs2_write_uio(const struct ufs2_volume *volume, uint inode_number,
                   struct ufs2_inode *inode, struct uio *uio);
void ufs2_begin(const struct ufs2_volume *volume);
void ufs2_end(const struct ufs2_volume *volume);
void ufs2_logger(void *arg);
int ufs2_mount_rw(const struct ufs2_volume *volume, const char *on);
int ufs2_mount_ro(const struct ufs2_volume *volume, const char *on);
void ufs2_sync(const struct ufs2_volume *volume);
void ufs2_unmount(const struct ufs2_volume *volume);
int ufs2_upgrade(const struct ufs2_volume *volume, const char *on);
int ufs2_downgrade(const struct ufs2_volume *volume);
int ufs2_request_cleanup(const struct ufs2_volume *volume);
int ufs2_statfs(const struct ufs2_volume *volume, struct fnu_fsstat *st);

int ufs2_read_inode(const struct ufs2_volume *volume, uint inode_number,
                    struct ufs2_inode *inode);
int ufs2_write_inode(const struct ufs2_volume *volume, uint inode_number,
                     const struct ufs2_inode *inode);
int ufs2_lookup(const struct ufs2_volume *volume, uint directory_inode,
                const char *name, uint *inode_number);
int ufs2_truncate(const struct ufs2_volume *volume, uint inode_number,
                  struct ufs2_inode *inode);
int ufs2_truncate_to(const struct ufs2_volume *volume, uint inode_number,
                     struct ufs2_inode *inode, uint64_t length);
int ufs2_make_directory(const struct ufs2_volume *volume, uint inode_number,
                        uint parent_inode);
int ufs2_alloc_inode(const struct ufs2_volume *volume, uint16_t mode,
                     uint parent_inode, uint *inode_number);
int ufs2_free_inode(const struct ufs2_volume *volume, uint inode_number);
int ufs2_dirlink(const struct ufs2_volume *volume, uint directory_inode,
                 const char *name, uint inode_number, uchar type);
int ufs2_dirunlink(const struct ufs2_volume *volume, uint directory_inode,
                   const char *name, uint *inode_number);
int ufs2_readdir(const struct ufs2_volume *volume, uint directory_inode,
                 uint index, uint *inode_number, char *name, uint name_size);
int ufs2_readlink(const struct ufs2_volume *volume, uint inode_number,
                  const struct ufs2_inode *inode, char *buf, int size);
int ufs2_symlink(const struct ufs2_volume *volume, uint inode_number,
                 struct ufs2_inode *inode, const char *target);
int ufs2_dir_empty(const struct ufs2_volume *volume, uint directory_inode);

#endif
