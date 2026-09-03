/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The superblock layout checks are derived from FreeBSD sys/ufs/ffs/fs.h at
 * revision 4aea6ea2eb400737837ff8d22c25688f88c7966c. See ufs2.h.
 */
#include "defs.h"
#include "spinlock.h"
#include "ufs2.h"

#define UFS2_OFFSET_NCG 44
#define UFS2_OFFSET_IBLKNO 8
#define UFS2_OFFSET_CBLKNO 12
#define UFS2_OFFSET_BSIZE 48
#define UFS2_OFFSET_FSIZE 52
#define UFS2_OFFSET_FRAG 56
#define UFS2_OFFSET_INOPB 120
#define UFS2_OFFSET_CGSIZE 160
#define UFS2_OFFSET_IPG 184
#define UFS2_OFFSET_FPG 188
#define UFS2_OFFSET_MAGIC 1372

#define UFS2_DINODE_SIZE 256
#define UFS2_DINODE_MODE 0
#define UFS2_DINODE_NLINK 2
#define UFS2_DINODE_UID 4
#define UFS2_DINODE_GID 8
#define UFS2_DINODE_SIZE_OFFSET 16
#define UFS2_DINODE_DIRECT_OFFSET 112
#define UFS2_DINODE_INDIRECT_OFFSET 208
#define UFS2_IFDIR 0040000
#define UFS2_IFREG 0100000
#define UFS2_IFMT 0170000
#define UFS2_DIR_HEADER_SIZE 8
#define UFS2_DIR_BLOCK_SIZE 512
#define UFS2_CG_MAGIC 0x00090255U
#define UFS2_CG_MAGIC_OFFSET 4
#define UFS2_CG_CGX_OFFSET 12
#define UFS2_CG_NDBLK_OFFSET 20
#define UFS2_CG_IUSEDOFF_OFFSET 92
#define UFS2_CG_FREEOFF_OFFSET 96
#define UFS2_CG_HEADER_SIZE 128

static uchar superblock[UFS2_SUPERBLOCK_SIZE];
static struct spinlock ufs2_allocation_lock;

static int ufs2_block_address(const struct ufs2_volume *volume,
                              const struct ufs2_inode *inode,
                              uint logical_block, uint64_t *address);
static uint ufs2_dir_record_size(uint name_length);

static uint read_le32(const uchar *data) {
  return (uint)data[0] | ((uint)data[1] << 8) | ((uint)data[2] << 16) |
         ((uint)data[3] << 24);
}

static uint16_t read_le16(const uchar *data) {
  return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint64_t read_le64(const uchar *data) {
  uint64_t low = read_le32(data);
  return low | ((uint64_t)read_le32(data + 4) << 32);
}

static void write_le16(uchar *data, uint16_t value) {
  data[0] = (uchar)value;
  data[1] = (uchar)(value >> 8);
}

static void write_le32(uchar *data, uint value) {
  data[0] = (uchar)value;
  data[1] = (uchar)(value >> 8);
  data[2] = (uchar)(value >> 16);
  data[3] = (uchar)(value >> 24);
}

static void write_le64(uchar *data, uint64_t value) {
  uint i;
  for (i = 0; i < 8; i++)
    data[i] = (uchar)(value >> (i * 8));
}

static int ufs2_inode_offset(const struct ufs2_volume *volume,
                             uint inode_number, uint64_t *offset) {
  uint group, index;
  uint64_t fragment;

  if (volume == 0 || offset == 0 || volume->inodes_per_group == 0)
    return -1;
  group = inode_number / volume->inodes_per_group;
  index = inode_number % volume->inodes_per_group;
  if (group >= volume->cylinder_groups)
    return -1;
  if (volume->inodes_per_block == 0 || volume->fragment_size == 0)
    return -1;
  fragment = (uint64_t)group * volume->fragments_per_group;
  if (fragment > ~(uint64_t)0 - volume->inode_table_fragment ||
      (uint64_t)(index / volume->inodes_per_block) *
              volume->fragments_per_block >
          ~(uint64_t)0 - fragment - volume->inode_table_fragment)
    return -1;
  fragment += volume->inode_table_fragment +
              (uint64_t)(index / volume->inodes_per_block) *
                  volume->fragments_per_block;
  if (fragment > ~(uint64_t)0 / volume->fragment_size ||
      (uint64_t)(index % volume->inodes_per_block) * UFS2_DINODE_SIZE >
          ~(uint64_t)0 - fragment * volume->fragment_size)
    return -1;
  *offset = fragment * volume->fragment_size +
            (uint64_t)(index % volume->inodes_per_block) * UFS2_DINODE_SIZE;
  return 0;
}

static int power_of_two(uint value) {
  return value != 0 && (value & (value - 1)) == 0;
}

void ufs2_init(void) { initlock(&ufs2_allocation_lock, "ufs2alloc"); }

static int ufs2_name_length(const char *name, uint *length) {
  uint n;

  for (n = 0; n <= UFS2_MAX_NAME; n++) {
    if (name[n] == '\0') {
      *length = n;
      return n != 0 ? 0 : -1;
    }
  }
  return -1;
}

int ufs2_probe(const struct blockdev *device, struct ufs2_volume *volume) {
  uint block_size, fragment_size, fragments, inodes_per_block;

  if (device == 0 || volume == 0)
    return -1;
  if (blockdev_read(device, UFS2_SUPERBLOCK_OFFSET, superblock,
                    sizeof(superblock)) < 0)
    return -1;
  if (read_le32(superblock + UFS2_OFFSET_MAGIC) != UFS2_MAGIC)
    return -1;

  block_size = read_le32(superblock + UFS2_OFFSET_BSIZE);
  fragment_size = read_le32(superblock + UFS2_OFFSET_FSIZE);
  fragments = read_le32(superblock + UFS2_OFFSET_FRAG);
  inodes_per_block = read_le32(superblock + UFS2_OFFSET_INOPB);
  if (!power_of_two(block_size) || block_size < 4096 || block_size > 65536 ||
      !power_of_two(fragment_size) || fragment_size < FNU_BLOCKDEV_SECTOR_SIZE ||
      fragment_size > block_size || fragments != block_size / fragment_size ||
      !power_of_two(fragments) || inodes_per_block == 0 ||
      inodes_per_block != block_size / UFS2_DINODE_SIZE ||
      read_le32(superblock + UFS2_OFFSET_NCG) == 0 ||
      read_le32(superblock + UFS2_OFFSET_IPG) == 0 ||
      read_le32(superblock + UFS2_OFFSET_FPG) == 0 ||
      read_le32(superblock + UFS2_OFFSET_CGSIZE) < UFS2_CG_HEADER_SIZE ||
      (int32_t)read_le32(superblock + UFS2_OFFSET_IBLKNO) < 0 ||
      (int32_t)read_le32(superblock + UFS2_OFFSET_CBLKNO) < 0)
    return -1;

  volume->device = *device;
  volume->block_size = block_size;
  volume->fragment_size = fragment_size;
  volume->fragments_per_block = fragments;
  volume->cylinder_groups = read_le32(superblock + UFS2_OFFSET_NCG);
  volume->inodes_per_group = read_le32(superblock + UFS2_OFFSET_IPG);
  volume->fragments_per_group = read_le32(superblock + UFS2_OFFSET_FPG);
  volume->inodes_per_block = inodes_per_block;
  volume->inode_table_fragment =
      (uint64_t)(int32_t)read_le32(superblock + UFS2_OFFSET_IBLKNO);
  volume->cylinder_group_block = read_le32(superblock + UFS2_OFFSET_CBLKNO);
  volume->cylinder_group_size = read_le32(superblock + UFS2_OFFSET_CGSIZE);
  cprintf("UFS2: validated read-only volume (%d byte blocks, %d groups)\n",
          volume->block_size, volume->cylinder_groups);
  return 0;
}

static int ufs2_cg_offset(const struct ufs2_volume *volume, uint group,
                          uint64_t *offset) {
  uint64_t fragment;

  if (volume == 0 || offset == 0 || volume->fragment_size == 0 ||
      group >= volume->cylinder_groups)
    return -1;
  fragment = (uint64_t)group * volume->fragments_per_group;
  if (fragment > ~(uint64_t)0 - volume->cylinder_group_block)
    return -1;
  fragment += volume->cylinder_group_block;
  if (fragment > ~(uint64_t)0 / volume->fragment_size)
    return -1;
  *offset = fragment * volume->fragment_size;
  return 0;
}

static int ufs2_cg_free_map(const struct ufs2_volume *volume, uint group,
                            uint64_t *map_offset, uint *entries) {
  uchar header[UFS2_CG_HEADER_SIZE];
  uint64_t cg_offset;
  uint freeoff;

  if (volume == 0 || map_offset == 0 || entries == 0 ||
      group >= volume->cylinder_groups)
    return -1;
  if (ufs2_cg_offset(volume, group, &cg_offset) < 0 ||
      blockdev_read(&volume->device, cg_offset, header, sizeof(header)) < 0 ||
      read_le32(header + UFS2_CG_MAGIC_OFFSET) != UFS2_CG_MAGIC ||
      read_le32(header + UFS2_CG_CGX_OFFSET) != group)
    return -1;
  *entries = read_le32(header + UFS2_CG_NDBLK_OFFSET);
  freeoff = read_le32(header + UFS2_CG_FREEOFF_OFFSET);
  if (*entries == 0 || *entries > volume->fragments_per_group ||
      freeoff < UFS2_CG_HEADER_SIZE || freeoff > volume->cylinder_group_size ||
      (*entries + 7) / 8 > volume->cylinder_group_size - freeoff)
    return -1;
  if (cg_offset > ~(uint64_t)0 - freeoff)
    return -1;
  *map_offset = cg_offset + freeoff;
  return 0;
}

static int ufs2_cg_inode_map(const struct ufs2_volume *volume, uint group,
                             uint64_t *map_offset) {
  uchar header[UFS2_CG_HEADER_SIZE];
  uint64_t cg_offset;
  uint iusedoff;

  if (volume == 0 || map_offset == 0 || group >= volume->cylinder_groups)
    return -1;
  if (ufs2_cg_offset(volume, group, &cg_offset) < 0 ||
      blockdev_read(&volume->device, cg_offset, header, sizeof(header)) < 0 ||
      read_le32(header + UFS2_CG_MAGIC_OFFSET) != UFS2_CG_MAGIC ||
      read_le32(header + UFS2_CG_CGX_OFFSET) != group)
    return -1;
  iusedoff = read_le32(header + UFS2_CG_IUSEDOFF_OFFSET);
  if (iusedoff < UFS2_CG_HEADER_SIZE || iusedoff > volume->cylinder_group_size ||
      (volume->inodes_per_group + 7) / 8 > volume->cylinder_group_size - iusedoff)
    return -1;
  if (cg_offset > ~(uint64_t)0 - iusedoff)
    return -1;
  *map_offset = cg_offset + iusedoff;
  return 0;
}

static int ufs2_initialize_inode(const struct ufs2_volume *volume,
                                 uint inode_number, uint16_t mode) {
  uchar raw[UFS2_DINODE_SIZE];
  uint64_t offset;

  if (ufs2_inode_offset(volume, inode_number, &offset) < 0)
    return -1;
  memset(raw, 0, sizeof(raw));
  write_le16(raw + UFS2_DINODE_MODE, mode);
  write_le16(raw + UFS2_DINODE_NLINK, 1);
  write_le32(raw + UFS2_DINODE_UID, 0);
  write_le32(raw + UFS2_DINODE_GID, 0);
  return blockdev_write(&volume->device, offset, raw, sizeof(raw));
}

// A one bit in cg_blksfree denotes a free fragment. This internal primitive
// changes only that bit; it is not yet used by namespace operations because
// cylinder-group and superblock summary counters still need transactional
// updates.
int ufs2_alloc_fragment(const struct ufs2_volume *volume, uint64_t *fragment) {
  uint group;

  if (volume == 0 || fragment == 0)
    return -1;
  acquire(&ufs2_allocation_lock);
  for (group = 0; group < volume->cylinder_groups; group++) {
    uint64_t map_offset;
    uint entries, bit;
    if (ufs2_cg_free_map(volume, group, &map_offset, &entries) < 0)
      continue;
    for (bit = 0; bit < entries; bit++) {
      uchar value;
      if (blockdev_read(&volume->device, map_offset + bit / 8, &value, 1) < 0)
        break;
      if ((value & (1 << (bit % 8))) == 0)
        continue;
      value &= ~(1 << (bit % 8));
      if (blockdev_write(&volume->device, map_offset + bit / 8, &value, 1) == 0) {
        *fragment = (uint64_t)group * volume->fragments_per_group + bit;
        release(&ufs2_allocation_lock);
        return 0;
      }
      release(&ufs2_allocation_lock);
      return -1;
    }
  }
  release(&ufs2_allocation_lock);
  return -1;
}

int ufs2_free_fragment(const struct ufs2_volume *volume, uint64_t fragment) {
  uint group, bit, entries;
  uint64_t map_offset;
  uchar value;

  if (volume == 0 || volume->fragments_per_group == 0 ||
      fragment / volume->fragments_per_group >= volume->cylinder_groups)
    return -1;
  group = fragment / volume->fragments_per_group;
  bit = fragment % volume->fragments_per_group;
  acquire(&ufs2_allocation_lock);
  if (ufs2_cg_free_map(volume, group, &map_offset, &entries) < 0 || bit >= entries ||
      blockdev_read(&volume->device, map_offset + bit / 8, &value, 1) < 0 ||
      (value & (1 << (bit % 8))) != 0) {
    release(&ufs2_allocation_lock);
    return -1;
  }
  value |= 1 << (bit % 8);
  if (blockdev_write(&volume->device, map_offset + bit / 8, &value, 1) < 0) {
    release(&ufs2_allocation_lock);
    return -1;
  }
  release(&ufs2_allocation_lock);
  return 0;
}

static int ufs2_set_fragment(const struct ufs2_volume *volume,
                             uint64_t map_offset, uint bit, int allocated) {
  uchar value;

  if (blockdev_read(&volume->device, map_offset + bit / 8, &value, 1) < 0)
    return -1;
  if (allocated)
    value &= ~(1 << (bit % 8));
  else
    value |= 1 << (bit % 8);
  return blockdev_write(&volume->device, map_offset + bit / 8, &value, 1);
}

static int ufs2_alloc_block(const struct ufs2_volume *volume,
                            uint64_t *fragment) {
  uint group;

  if (volume == 0 || fragment == 0 || volume->fragments_per_block == 0)
    return -1;
  acquire(&ufs2_allocation_lock);
  for (group = 0; group < volume->cylinder_groups; group++) {
    uint64_t map_offset;
    uint entries, bit;

    if (ufs2_cg_free_map(volume, group, &map_offset, &entries) < 0)
      continue;
    for (bit = 0; bit + volume->fragments_per_block <= entries;
         bit += volume->fragments_per_block) {
      uint i;
      for (i = 0; i < volume->fragments_per_block; i++) {
        uchar value;
        if (blockdev_read(&volume->device, map_offset + (bit + i) / 8,
                          &value, 1) < 0 ||
            (value & (1 << ((bit + i) % 8))) == 0)
          break;
      }
      if (i != volume->fragments_per_block)
        continue;
      for (i = 0; i < volume->fragments_per_block; i++) {
        if (ufs2_set_fragment(volume, map_offset, bit + i, 1) < 0) {
          while (i > 0) {
            i--;
            ufs2_set_fragment(volume, map_offset, bit + i, 0);
          }
          release(&ufs2_allocation_lock);
          return -1;
        }
      }
      *fragment = (uint64_t)group * volume->fragments_per_group + bit;
      release(&ufs2_allocation_lock);
      return 0;
    }
  }
  release(&ufs2_allocation_lock);
  return -1;
}

static int ufs2_free_block(const struct ufs2_volume *volume,
                           uint64_t fragment) {
  uint group, bit, entries, i;
  uint64_t map_offset;

  if (volume == 0 || volume->fragments_per_group == 0 ||
      volume->fragments_per_block == 0 ||
      fragment % volume->fragments_per_block != 0)
    return -1;
  group = fragment / volume->fragments_per_group;
  bit = fragment % volume->fragments_per_group;
  acquire(&ufs2_allocation_lock);
  if (group >= volume->cylinder_groups ||
      ufs2_cg_free_map(volume, group, &map_offset, &entries) < 0 ||
      bit + volume->fragments_per_block > entries) {
    release(&ufs2_allocation_lock);
    return -1;
  }
  for (i = 0; i < volume->fragments_per_block; i++)
    if (ufs2_set_fragment(volume, map_offset, bit + i, 0) < 0) {
      release(&ufs2_allocation_lock);
      return -1;
    }
  release(&ufs2_allocation_lock);
  return 0;
}

static int ufs2_zero_block(const struct ufs2_volume *volume, uint64_t fragment) {
  uchar zero[FNU_BLOCKDEV_SECTOR_SIZE];
  uint offset;

  if (fragment > ~(uint64_t)0 / volume->fragment_size)
    return -1;
  memset(zero, 0, sizeof(zero));
  for (offset = 0; offset < volume->block_size; offset += sizeof(zero))
    if (blockdev_write(&volume->device, fragment * volume->fragment_size + offset,
                       zero, sizeof(zero)) < 0)
      return -1;
  return 0;
}

// Reserve an unused inode and initialise it before returning it to a caller.
// The caller still needs to link it into a directory. A crash before linking
// leaves an unreferenced inode, which fsck can safely reclaim.
int ufs2_alloc_inode(const struct ufs2_volume *volume, uint16_t mode,
                     uint *inode_number) {
  uint group;

  if (volume == 0 || inode_number == 0 ||
      ((mode & UFS2_IFMT) != UFS2_IFREG && (mode & UFS2_IFMT) != UFS2_IFDIR))
    return -1;
  acquire(&ufs2_allocation_lock);
  for (group = 0; group < volume->cylinder_groups; group++) {
    uint64_t map_offset;
    uint bit;
    if (ufs2_cg_inode_map(volume, group, &map_offset) < 0)
      continue;
    for (bit = 0; bit < volume->inodes_per_group; bit++) {
      uchar value;
      uint number = group * volume->inodes_per_group + bit;
      if (number < UFS2_ROOT_INO)
        continue;
      if (blockdev_read(&volume->device, map_offset + bit / 8, &value, 1) < 0) {
        release(&ufs2_allocation_lock);
        return -1;
      }
      if (value & (1 << (bit % 8)))
        continue;
      value |= 1 << (bit % 8);
      if (blockdev_write(&volume->device, map_offset + bit / 8, &value, 1) < 0) {
        release(&ufs2_allocation_lock);
        return -1;
      }
      if (ufs2_initialize_inode(volume, number, mode) == 0) {
        *inode_number = number;
        release(&ufs2_allocation_lock);
        return 0;
      }
      value &= ~(1 << (bit % 8));
      blockdev_write(&volume->device, map_offset + bit / 8, &value, 1);
      release(&ufs2_allocation_lock);
      return -1;
    }
  }
  release(&ufs2_allocation_lock);
  return -1;
}

int ufs2_free_inode(const struct ufs2_volume *volume, uint inode_number) {
  uchar raw[UFS2_DINODE_SIZE], value;
  uint group, bit;
  uint64_t inode_offset, map_offset;

  if (volume == 0 || inode_number < UFS2_ROOT_INO ||
      volume->inodes_per_group == 0)
    return -1;
  group = inode_number / volume->inodes_per_group;
  bit = inode_number % volume->inodes_per_group;
  if (ufs2_inode_offset(volume, inode_number, &inode_offset) < 0 ||
      ufs2_cg_inode_map(volume, group, &map_offset) < 0)
    return -1;

  acquire(&ufs2_allocation_lock);
  if (blockdev_read(&volume->device, map_offset + bit / 8, &value, 1) < 0 ||
      (value & (1 << (bit % 8))) == 0) {
    release(&ufs2_allocation_lock);
    return -1;
  }
  memset(raw, 0, sizeof(raw));
  if (blockdev_write(&volume->device, inode_offset, raw, sizeof(raw)) < 0) {
    release(&ufs2_allocation_lock);
    return -1;
  }
  value &= ~(1 << (bit % 8));
  if (blockdev_write(&volume->device, map_offset + bit / 8, &value, 1) < 0) {
    release(&ufs2_allocation_lock);
    return -1;
  }
  release(&ufs2_allocation_lock);
  return 0;
}

int ufs2_read_inode(const struct ufs2_volume *volume, uint inode_number,
                    struct ufs2_inode *inode) {
  uchar raw[UFS2_DINODE_SIZE];
  uint64_t offset;
  uint i;

  if (inode == 0 || ufs2_inode_offset(volume, inode_number, &offset) < 0)
    return -1;
  if (blockdev_read(&volume->device, offset, raw, sizeof(raw)) < 0)
    return -1;

  inode->mode = read_le16(raw + UFS2_DINODE_MODE);
  inode->nlink = read_le16(raw + UFS2_DINODE_NLINK);
  inode->uid = read_le32(raw + UFS2_DINODE_UID);
  inode->gid = read_le32(raw + UFS2_DINODE_GID);
  inode->size = read_le64(raw + UFS2_DINODE_SIZE_OFFSET);
  for (i = 0; i < UFS2_DIRECT_BLOCKS; i++)
    inode->direct[i] = read_le64(raw + UFS2_DINODE_DIRECT_OFFSET + i * 8);
  for (i = 0; i < 3; i++)
    inode->indirect[i] = read_le64(raw + UFS2_DINODE_INDIRECT_OFFSET + i * 8);
  return 0;
}

int ufs2_write_inode(const struct ufs2_volume *volume, uint inode_number,
                     const struct ufs2_inode *inode) {
  uchar raw[UFS2_DINODE_SIZE];
  uint64_t offset;
  uint i;

  if (inode == 0 || ufs2_inode_offset(volume, inode_number, &offset) < 0 ||
      blockdev_read(&volume->device, offset, raw, sizeof(raw)) < 0)
    return -1;
  write_le16(raw + UFS2_DINODE_MODE, inode->mode);
  write_le16(raw + UFS2_DINODE_NLINK, inode->nlink);
  write_le32(raw + UFS2_DINODE_UID, inode->uid);
  write_le32(raw + UFS2_DINODE_GID, inode->gid);
  write_le64(raw + UFS2_DINODE_SIZE_OFFSET, inode->size);
  for (i = 0; i < UFS2_DIRECT_BLOCKS; i++)
    write_le64(raw + UFS2_DINODE_DIRECT_OFFSET + i * 8, inode->direct[i]);
  for (i = 0; i < 3; i++)
    write_le64(raw + UFS2_DINODE_INDIRECT_OFFSET + i * 8, inode->indirect[i]);
  return blockdev_write(&volume->device, offset, raw, sizeof(raw));
}

static int ufs2_write_range(const struct ufs2_volume *volume,
                            const struct ufs2_inode *inode, uint64_t offset,
                            const void *src, uint length) {
  const uchar *in = src;

  while (length > 0) {
    uint logical_block = (uint)(offset / volume->block_size);
    uint in_block = (uint)(offset % volume->block_size);
    uint count = volume->block_size - in_block;
    uint64_t address;

    if (count > length)
      count = length;
    if (ufs2_block_address(volume, inode, logical_block, &address) < 0 ||
        address > ~(uint64_t)0 / volume->fragment_size ||
        blockdev_write(&volume->device,
                       address * volume->fragment_size + in_block, in,
                       count) < 0)
      return -1;
    in += count;
    offset += count;
    length -= count;
  }
  return 0;
}

int ufs2_write_growing(const struct ufs2_volume *volume, uint inode_number,
                       struct ufs2_inode *inode, uint64_t offset,
                       const void *src, uint length) {
  uchar zero[FNU_BLOCKDEV_SECTOR_SIZE];
  uint64_t end, old_size;
  uint block;

  if (volume == 0 || inode == 0 || (length != 0 && src == 0) ||
      (inode->mode & UFS2_IFMT) != UFS2_IFREG ||
      offset > ~(uint64_t)0 - length)
    return -1;
  end = offset + length;
  if (end > (uint64_t)UFS2_DIRECT_BLOCKS * volume->block_size)
    return -1;
  old_size = inode->size;
  for (block = 0; block < (end + volume->block_size - 1) / volume->block_size;
       block++) {
    uint64_t fragment = 0;
    if (inode->direct[block] != 0)
      continue;
    if (ufs2_alloc_block(volume, &fragment) < 0 ||
        ufs2_zero_block(volume, fragment) < 0) {
      if (fragment != 0)
        ufs2_free_block(volume, fragment);
      return -1;
    }
    inode->direct[block] = fragment;
    if (ufs2_write_inode(volume, inode_number, inode) < 0)
      return -1;
  }
  if (end <= old_size)
    return ufs2_write_range(volume, inode, offset, src, length);

  inode->size = end;
  if (ufs2_write_inode(volume, inode_number, inode) < 0)
    return -1;
  memset(zero, 0, sizeof(zero));
  while (old_size < offset) {
    uint count = offset - old_size < sizeof(zero)
                     ? (uint)(offset - old_size)
                     : sizeof(zero);
    if (ufs2_write_range(volume, inode, old_size, zero, count) < 0)
      return -1;
    old_size += count;
  }
  return ufs2_write_range(volume, inode, offset, src, length);
}

int ufs2_truncate(const struct ufs2_volume *volume, uint inode_number,
                  struct ufs2_inode *inode) {
  uint block;

  if (volume == 0 || inode == 0 ||
      ((inode->mode & UFS2_IFMT) != UFS2_IFREG &&
       (inode->mode & UFS2_IFMT) != UFS2_IFDIR))
    return -1;
  inode->size = 0;
  if (ufs2_write_inode(volume, inode_number, inode) < 0)
    return -1;
  for (block = 0; block < UFS2_DIRECT_BLOCKS; block++) {
    uint64_t fragment = inode->direct[block];
    if (fragment == 0)
      continue;
    inode->direct[block] = 0;
    if (ufs2_write_inode(volume, inode_number, inode) < 0 ||
        ufs2_free_block(volume, fragment) < 0)
      return -1;
  }
  return 0;
}

int ufs2_make_directory(const struct ufs2_volume *volume, uint inode_number,
                        uint parent_inode) {
  struct ufs2_inode inode;
  uchar block[UFS2_DIR_BLOCK_SIZE];
  uint64_t fragment;
  uint offset;

  if (volume == 0 || ufs2_read_inode(volume, inode_number, &inode) < 0 ||
      (inode.mode & UFS2_IFMT) != UFS2_IFDIR ||
      ufs2_alloc_block(volume, &fragment) < 0 ||
      ufs2_zero_block(volume, fragment) < 0)
    return -1;
  inode.direct[0] = fragment;
  inode.size = volume->block_size;
  inode.nlink = 2;
  if (ufs2_write_inode(volume, inode_number, &inode) < 0)
    return -1;
  memset(block, 0, sizeof(block));
  write_le32(block, inode_number);
  write_le16(block + 4, ufs2_dir_record_size(1));
  block[6] = UFS2_DIRTYPE_DIR;
  block[7] = 1;
  block[8] = '.';
  write_le32(block + ufs2_dir_record_size(1), parent_inode);
  write_le16(block + ufs2_dir_record_size(1) + 4,
             UFS2_DIR_BLOCK_SIZE - ufs2_dir_record_size(1));
  block[ufs2_dir_record_size(1) + 6] = UFS2_DIRTYPE_DIR;
  block[ufs2_dir_record_size(1) + 7] = 2;
  block[ufs2_dir_record_size(1) + 8] = '.';
  block[ufs2_dir_record_size(1) + 9] = '.';
  for (offset = 0; offset < volume->block_size; offset += UFS2_DIR_BLOCK_SIZE) {
    if (offset != 0) {
      memset(block, 0, sizeof(block));
      write_le16(block + 4, UFS2_DIR_BLOCK_SIZE);
    }
    if (blockdev_write(&volume->device, fragment * volume->fragment_size + offset,
                       block, sizeof(block)) < 0)
      return -1;
  }
  return 0;
}

int ufs2_read_direct(const struct ufs2_volume *volume,
                     const struct ufs2_inode *inode, uint logical_block,
                     void *dst, uint length) {
  uint64_t offset;

  if (volume == 0 || inode == 0 || dst == 0 ||
      logical_block >= UFS2_DIRECT_BLOCKS || length > volume->block_size ||
      inode->direct[logical_block] == 0 ||
      inode->direct[logical_block] > ~(uint64_t)0 / volume->fragment_size)
    return -1;
  offset = inode->direct[logical_block] * volume->fragment_size;
  return blockdev_read(&volume->device, offset, dst, length);
}

static int ufs2_block_address(const struct ufs2_volume *volume,
                              const struct ufs2_inode *inode,
                              uint logical_block, uint64_t *address) {
  uint64_t indirect_offset;

  if (logical_block < UFS2_DIRECT_BLOCKS) {
    *address = inode->direct[logical_block];
    return *address == 0 ? -1 : 0;
  }
  logical_block -= UFS2_DIRECT_BLOCKS;
  if (logical_block >= volume->block_size / sizeof(uint64_t) ||
      inode->indirect[0] == 0 ||
      inode->indirect[0] > ~(uint64_t)0 / volume->fragment_size)
    return -1;
  indirect_offset = inode->indirect[0] * volume->fragment_size +
                    (uint64_t)logical_block * sizeof(uint64_t);
  if (blockdev_read(&volume->device, indirect_offset, address,
                    sizeof(*address)) < 0)
    return -1;
  // UFS2 is little-endian on the only currently supported x86-64 target.
  return *address == 0 ? -1 : 0;
}

int ufs2_read(const struct ufs2_volume *volume, const struct ufs2_inode *inode,
              uint64_t offset, void *dst, uint length) {
  uchar *out = dst;

  if (offset > inode->size || (uint64_t)length > inode->size - offset)
    return -1;
  while (length > 0) {
    uint logical_block = (uint)(offset / volume->block_size);
    uint in_block = (uint)(offset % volume->block_size);
    uint count = volume->block_size - in_block;
    uint64_t disk_offset, address;

    if (ufs2_block_address(volume, inode, logical_block, &address) < 0 ||
        address > ~(uint64_t)0 / volume->fragment_size)
      return -1;
    if (count > length)
      count = length;
    disk_offset = address * volume->fragment_size + in_block;
    if (blockdev_read(&volume->device, disk_offset, out, count) < 0)
      return -1;
    out += count;
    offset += count;
    length -= count;
  }
  return 0;
}

int ufs2_write_existing(const struct ufs2_volume *volume,
                        const struct ufs2_inode *inode, uint64_t offset,
                        const void *src, uint length) {
  const uchar *in = src;

  if (volume == 0 || inode == 0 || src == 0 || offset > inode->size ||
      (uint64_t)length > inode->size - offset)
    return -1;
  while (length > 0) {
    uint logical_block = (uint)(offset / volume->block_size);
    uint in_block = (uint)(offset % volume->block_size);
    uint count = volume->block_size - in_block;
    uint64_t disk_offset, address;

    if (count > length)
      count = length;
    if (ufs2_block_address(volume, inode, logical_block, &address) < 0 ||
        address > ~(uint64_t)0 / volume->fragment_size)
      return -1;
    disk_offset = address * volume->fragment_size + in_block;
    if (blockdev_write(&volume->device, disk_offset, in, count) < 0)
      return -1;
    in += count;
    offset += count;
    length -= count;
  }
  return 0;
}

static uint ufs2_dir_record_size(uint name_length) {
  return (UFS2_DIR_HEADER_SIZE + name_length + 3) & ~3U;
}

int ufs2_dirlink(const struct ufs2_volume *volume, uint directory_inode,
                 const char *name, uint inode_number, uchar type) {
  struct ufs2_inode directory;
  uchar block[UFS2_DIR_BLOCK_SIZE];
  uint entry_offset, name_length, needed;
  uint64_t file_offset;

  if (volume == 0 || inode_number < UFS2_ROOT_INO ||
      ufs2_name_length(name, &name_length) < 0 ||
      ufs2_read_inode(volume, directory_inode, &directory) < 0 ||
      (directory.mode & UFS2_IFMT) != UFS2_IFDIR ||
      (type != UFS2_DIRTYPE_REG && type != UFS2_DIRTYPE_DIR))
    return -1;
  needed = ufs2_dir_record_size(name_length);
  {
    uint existing_inode;
    if (ufs2_lookup(volume, directory_inode, name, &existing_inode) == 0)
      return -1;
  }

  for (file_offset = 0; file_offset < directory.size;
       file_offset += UFS2_DIR_BLOCK_SIZE) {
    uint to_read = directory.size - file_offset < UFS2_DIR_BLOCK_SIZE
                       ? (uint)(directory.size - file_offset)
                       : UFS2_DIR_BLOCK_SIZE;
    if (ufs2_read(volume, &directory, file_offset, block, to_read) < 0)
      return -1;
    for (entry_offset = 0; entry_offset + UFS2_DIR_HEADER_SIZE <= to_read;) {
      uint record_length = read_le16(block + entry_offset + 4);
      uint old_name_length = block[entry_offset + 7];
      uint old_size;
      uint new_offset;
      uint available;
      uint new_length;

      if (record_length < UFS2_DIR_HEADER_SIZE || record_length % 4 != 0 ||
          record_length > to_read - entry_offset ||
          old_name_length > UFS2_MAX_NAME ||
          old_name_length + UFS2_DIR_HEADER_SIZE > record_length)
        return -1;
      old_size = ufs2_dir_record_size(old_name_length);
      if (read_le32(block + entry_offset) == 0) {
        if (record_length < needed) {
          entry_offset += record_length;
          continue;
        }
        new_offset = entry_offset;
      } else {
        if (old_size > record_length || record_length - old_size < needed) {
          entry_offset += record_length;
          continue;
        }
        write_le16(block + entry_offset + 4, old_size);
        new_offset = entry_offset + old_size;
      }
      available = record_length - (new_offset - entry_offset);
      new_length = available;
      if (available > needed) {
        uint free_offset = new_offset + needed;
        memset(block + free_offset, 0, available - needed);
        write_le16(block + free_offset + 4, available - needed);
        new_length = needed;
      }
      memset(block + new_offset, 0, needed);
      write_le32(block + new_offset, inode_number);
      write_le16(block + new_offset + 4, new_length);
      block[new_offset + 6] = type;
      block[new_offset + 7] = (uchar)name_length;
      memmove(block + new_offset + UFS2_DIR_HEADER_SIZE, name, name_length);
      return ufs2_write_existing(volume, &directory, file_offset, block,
                                 to_read);
    }
  }

  // No record can be split. Add one fully initialised directory block, then
  // retry; each 512-byte DIRBLKSIZ region starts as one free record.
  if (directory.size % volume->block_size != 0 ||
      directory.size / volume->block_size >= UFS2_DIRECT_BLOCKS)
    return -1;
  {
    uint logical_block = directory.size / volume->block_size;
    uint64_t fragment = 0;
    uint offset;
    if (ufs2_alloc_block(volume, &fragment) < 0 ||
        ufs2_zero_block(volume, fragment) < 0)
      return -1;
    memset(block, 0, sizeof(block));
    write_le16(block + 4, UFS2_DIR_BLOCK_SIZE);
    for (offset = 0; offset < volume->block_size; offset += UFS2_DIR_BLOCK_SIZE)
      if (blockdev_write(&volume->device,
                         fragment * volume->fragment_size + offset, block,
                         sizeof(block)) < 0)
        return -1;
    directory.direct[logical_block] = fragment;
    directory.size += volume->block_size;
    if (ufs2_write_inode(volume, directory_inode, &directory) < 0)
      return -1;
  }
  return ufs2_dirlink(volume, directory_inode, name, inode_number, type);
}

int ufs2_dirunlink(const struct ufs2_volume *volume, uint directory_inode,
                   const char *name, uint *inode_number) {
  struct ufs2_inode directory;
  uchar block[UFS2_DIR_BLOCK_SIZE];
  uint name_length, entry_offset;
  uint64_t file_offset;

  if (volume == 0 || name == 0 || inode_number == 0 ||
      ufs2_name_length(name, &name_length) < 0 ||
      ufs2_read_inode(volume, directory_inode, &directory) < 0 ||
      (directory.mode & UFS2_IFMT) != UFS2_IFDIR)
    return -1;
  for (file_offset = 0; file_offset < directory.size;
       file_offset += UFS2_DIR_BLOCK_SIZE) {
    uint to_read = directory.size - file_offset < UFS2_DIR_BLOCK_SIZE
                       ? (uint)(directory.size - file_offset)
                       : UFS2_DIR_BLOCK_SIZE;
    if (ufs2_read(volume, &directory, file_offset, block, to_read) < 0)
      return -1;
    for (entry_offset = 0; entry_offset + UFS2_DIR_HEADER_SIZE <= to_read;) {
      uint record_length = read_le16(block + entry_offset + 4);
      uint entry_name_length = block[entry_offset + 7];
      uint entry_inode = read_le32(block + entry_offset);

      if (record_length < UFS2_DIR_HEADER_SIZE || record_length % 4 != 0 ||
          record_length > to_read - entry_offset ||
          entry_name_length > UFS2_MAX_NAME ||
          entry_name_length + UFS2_DIR_HEADER_SIZE > record_length)
        return -1;
      if (entry_inode != 0 && entry_name_length == name_length &&
          memcmp(block + entry_offset + UFS2_DIR_HEADER_SIZE, name,
                 name_length) == 0) {
        write_le32(block + entry_offset, 0);
        if (ufs2_write_existing(volume, &directory, file_offset, block,
                                to_read) < 0)
          return -1;
        *inode_number = entry_inode;
        return 0;
      }
      entry_offset += record_length;
    }
  }
  return -1;
}

int ufs2_lookup(const struct ufs2_volume *volume, uint directory_inode,
                const char *name, uint *inode_number) {
  struct ufs2_inode directory;
  uchar block[UFS2_DIR_BLOCK_SIZE];
  uint entry_offset, record_length, name_length, requested_length;
  uint64_t file_offset;

  if (volume == 0 || name == 0 || inode_number == 0 ||
      ufs2_name_length(name, &requested_length) < 0 ||
      ufs2_read_inode(volume, directory_inode, &directory) < 0 ||
      (directory.mode & 0170000) != UFS2_IFDIR)
    return -1;
  for (file_offset = 0; file_offset < directory.size;
       file_offset += UFS2_DIR_BLOCK_SIZE) {
    uint to_read = directory.size - file_offset < UFS2_DIR_BLOCK_SIZE
                       ? (uint)(directory.size - file_offset)
                       : UFS2_DIR_BLOCK_SIZE;
    if (ufs2_read(volume, &directory, file_offset, block, to_read) < 0)
      return -1;
    for (entry_offset = 0; entry_offset + UFS2_DIR_HEADER_SIZE <= to_read;
         entry_offset += record_length) {
      record_length = read_le16(block + entry_offset + 4);
      name_length = block[entry_offset + 7];
      if (record_length < UFS2_DIR_HEADER_SIZE || record_length % 4 != 0 ||
          record_length > to_read - entry_offset || name_length > UFS2_MAX_NAME ||
          name_length + UFS2_DIR_HEADER_SIZE > record_length)
        return -1;
      if (read_le32(block + entry_offset) != 0 &&
          requested_length == name_length &&
          memcmp(block + entry_offset + UFS2_DIR_HEADER_SIZE, name, name_length) == 0) {
        *inode_number = read_le32(block + entry_offset);
        return 0;
      }
    }
  }
  return -1;
}

int ufs2_readdir(const struct ufs2_volume *volume, uint directory_inode,
                 uint index, uint *inode_number, char *name, uint name_size) {
  struct ufs2_inode directory;
  uchar block[UFS2_DIR_BLOCK_SIZE];
  uint entry_offset, record_length, name_length, entry_index = 0;
  uint64_t file_offset;

  if (volume == 0 || inode_number == 0 || name == 0 || name_size == 0 ||
      ufs2_read_inode(volume, directory_inode, &directory) < 0 ||
      (directory.mode & 0170000) != UFS2_IFDIR)
    return -1;
  for (file_offset = 0; file_offset < directory.size;
       file_offset += UFS2_DIR_BLOCK_SIZE) {
    uint to_read = directory.size - file_offset < UFS2_DIR_BLOCK_SIZE
                       ? (uint)(directory.size - file_offset)
                       : UFS2_DIR_BLOCK_SIZE;
    if (ufs2_read(volume, &directory, file_offset, block, to_read) < 0)
      return -1;
    for (entry_offset = 0; entry_offset + UFS2_DIR_HEADER_SIZE <= to_read;
         entry_offset += record_length) {
      uint copy_length;
      record_length = read_le16(block + entry_offset + 4);
      name_length = block[entry_offset + 7];
      if (record_length < UFS2_DIR_HEADER_SIZE || record_length % 4 != 0 ||
          record_length > to_read - entry_offset || name_length > UFS2_MAX_NAME ||
          name_length + UFS2_DIR_HEADER_SIZE > record_length)
        return -1;
      if (read_le32(block + entry_offset) == 0)
        continue;
      if (entry_index++ != index)
        continue;
      copy_length = name_length < name_size - 1 ? name_length : name_size - 1;
      memmove(name, block + entry_offset + UFS2_DIR_HEADER_SIZE, copy_length);
      name[copy_length] = '\0';
      *inode_number = read_le32(block + entry_offset);
      return 0;
    }
  }
  return -1;
}
