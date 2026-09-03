#ifndef PRONINX_X86_64_FS_H
#define PRONINX_X86_64_FS_H

// On-disk file system format.
// Both the kernel and user programs use this header file.

#include "inc/dir.h"
#include "inc/types.h"

#define ROOTINO 1 // root i-number
#define BSIZE 512 // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;       // Size of file system image (blocks)
  uint nblocks;    // Number of data blocks
  uint ninodes;    // Number of inodes.
  uint nlog;       // Number of log blocks
  uint logstart;   // Block number of first log block
  uint inodestart; // Block number of first inode block
  uint bmapstart;  // Block number of first free map block
};

#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT)

// On-disk inode structure
struct dinode {
  short type;              // File type
  short major;             // Major device number (T_DEVICE only)
  short minor;             // Minor device number (T_DEVICE only)
  short nlink;             // Number of links to inode in file system
  uint size;               // Size of file (bytes)
  uint uid;                // Owner user ID
  uint gid;                // Owner group ID
  ushort mode;             // Permission bits
  ushort pad;
  uint addrs[NDIRECT + 2]; // Data block addresses
  // Keep native on-disk inodes block-aligned after adding ownership data.
  // A 128-byte inode gives exactly four entries per 512-byte filesystem block.
  uchar reserved[52];
};

// Inodes per block.
#define IPB (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB (BSIZE * 8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) (b / BPB + sb.bmapstart)

#endif /* ifndef PRONINX_X86_64_FS_H */
