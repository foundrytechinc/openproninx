/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * UFS2 on-disk offsets below are derived from FreeBSD sys/ufs/ffs/fs.h at
 * revision 4aea6ea2eb400737837ff8d22c25688f88c7966c. The source header is
 * retained under third_party/freebsd-src with its original notices.
 */
#ifndef FNU_PRONINX_UFS2_H
#define FNU_PRONINX_UFS2_H

#include "blockdev.h"

#define UFS2_SUPERBLOCK_OFFSET 65536
#define UFS2_SUPERBLOCK_SIZE 8192
#define UFS2_MAGIC 0x19540119U
#define UFS2_ROOT_INO 2
#define UFS2_DIRECT_BLOCKS 12
#define UFS2_MAX_NAME 255
#define UFS2_DIRTYPE_DIR 4
#define UFS2_DIRTYPE_REG 8

struct ufs2_volume {
  struct blockdev device;
  uint block_size;
  uint fragment_size;
  uint fragments_per_block;
  uint cylinder_groups;
  uint inodes_per_group;
  uint fragments_per_group;
  uint inodes_per_block;
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
  uint64_t direct[UFS2_DIRECT_BLOCKS];
  uint64_t indirect[3];
};

// Validate an on-disk UFS2 superblock. This does not mount the volume and
// makes no writes; directory and inode support comes in the next stage.
int ufs2_probe(const struct blockdev *device, struct ufs2_volume *volume);
void ufs2_init(void);

// The reader supports direct blocks and one level of indirection;
// double/triple indirection and symlinks remain unsupported.
int ufs2_read_inode(const struct ufs2_volume *volume, uint inode_number,
                    struct ufs2_inode *inode);
// Persist only the inode fields represented above. Callers must have already
// made corresponding allocation and directory changes recoverable.
int ufs2_write_inode(const struct ufs2_volume *volume, uint inode_number,
                     const struct ufs2_inode *inode);
int ufs2_read_direct(const struct ufs2_volume *volume,
                     const struct ufs2_inode *inode, uint logical_block,
                     void *dst, uint length);
int ufs2_lookup(const struct ufs2_volume *volume, uint directory_inode,
                const char *name, uint *inode_number);
int ufs2_read(const struct ufs2_volume *volume, const struct ufs2_inode *inode,
              uint64_t offset, void *dst, uint length);
// Overwrite bytes in blocks already assigned to an inode.  This low-level
// helper neither allocates blocks nor updates inode size or metadata.
int ufs2_write_existing(const struct ufs2_volume *volume,
                        const struct ufs2_inode *inode, uint64_t offset,
                        const void *src, uint length);
int ufs2_alloc_fragment(const struct ufs2_volume *volume, uint64_t *fragment);
int ufs2_free_fragment(const struct ufs2_volume *volume, uint64_t fragment);
int ufs2_write_growing(const struct ufs2_volume *volume, uint inode_number,
                       struct ufs2_inode *inode, uint64_t offset,
                       const void *src, uint length);
int ufs2_truncate(const struct ufs2_volume *volume, uint inode_number,
                  struct ufs2_inode *inode);
int ufs2_make_directory(const struct ufs2_volume *volume, uint inode_number,
                        uint parent_inode);
int ufs2_alloc_inode(const struct ufs2_volume *volume, uint16_t mode,
                     uint *inode_number);
int ufs2_free_inode(const struct ufs2_volume *volume, uint inode_number);
// Add an entry by splitting an existing directory record.  The directory must
// already have a data block with sufficient record slack; this function never
// grows it or allocates fragments.
int ufs2_dirlink(const struct ufs2_volume *volume, uint directory_inode,
                 const char *name, uint inode_number, uchar type);
int ufs2_dirunlink(const struct ufs2_volume *volume, uint directory_inode,
                   const char *name, uint *inode_number);
int ufs2_readdir(const struct ufs2_volume *volume, uint directory_inode,
                 uint index, uint *inode_number, char *name, uint name_size);

#endif /* FNU_PRONINX_UFS2_H */
