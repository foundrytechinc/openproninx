/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The superblock layout checks are derived from FreeBSD sys/ufs/ffs/fs.h at
 * revision 4aea6ea2eb400737837ff8d22c25688f88c7966c. See ufs2.h.
 */
#include "defs.h"
#include "ufs2.h"

#define UFS2_OFFSET_NCG 44
#define UFS2_OFFSET_IBLKNO 8
#define UFS2_OFFSET_BSIZE 48
#define UFS2_OFFSET_FSIZE 52
#define UFS2_OFFSET_FRAG 56
#define UFS2_OFFSET_INOPB 120
#define UFS2_OFFSET_IPG 184
#define UFS2_OFFSET_FPG 188
#define UFS2_OFFSET_MAGIC 1372

#define UFS2_DINODE_SIZE 256
#define UFS2_DINODE_MODE 0
#define UFS2_DINODE_SIZE_OFFSET 16
#define UFS2_DINODE_DIRECT_OFFSET 112
#define UFS2_DINODE_INDIRECT_OFFSET 208
#define UFS2_IFDIR 0040000
#define UFS2_DIR_HEADER_SIZE 8
#define UFS2_DIR_BLOCK_SIZE 512

static uchar superblock[UFS2_SUPERBLOCK_SIZE];

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

static int power_of_two(uint value) {
  return value != 0 && (value & (value - 1)) == 0;
}

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
      (int32_t)read_le32(superblock + UFS2_OFFSET_IBLKNO) < 0)
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
  cprintf("UFS2: validated read-only volume (%d byte blocks, %d groups)\n",
          volume->block_size, volume->cylinder_groups);
  return 0;
}

int ufs2_read_inode(const struct ufs2_volume *volume, uint inode_number,
                    struct ufs2_inode *inode) {
  uchar raw[UFS2_DINODE_SIZE];
  uint group, index;
  uint64_t fragment, offset;
  uint i;

  if (volume == 0 || inode == 0 || volume->inodes_per_group == 0)
    return -1;
  group = inode_number / volume->inodes_per_group;
  index = inode_number % volume->inodes_per_group;
  if (group >= volume->cylinder_groups)
    return -1;
  fragment = (uint64_t)group * volume->fragments_per_group +
             volume->inode_table_fragment +
             (uint64_t)(index / volume->inodes_per_block) *
                 volume->fragments_per_block;
  if (fragment > ~(uint64_t)0 / volume->fragment_size)
    return -1;
  offset = fragment * volume->fragment_size +
           (uint64_t)(index % volume->inodes_per_block) * UFS2_DINODE_SIZE;
  if (blockdev_read(&volume->device, offset, raw, sizeof(raw)) < 0)
    return -1;

  inode->mode = read_le16(raw + UFS2_DINODE_MODE);
  inode->size = read_le64(raw + UFS2_DINODE_SIZE_OFFSET);
  for (i = 0; i < UFS2_DIRECT_BLOCKS; i++)
    inode->direct[i] = read_le64(raw + UFS2_DINODE_DIRECT_OFFSET + i * 8);
  for (i = 0; i < 3; i++)
    inode->indirect[i] = read_le64(raw + UFS2_DINODE_INDIRECT_OFFSET + i * 8);
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
