# UFS2

UFS2 is the root filesystem, on the GPT partition `fnu-root`.
`tools/mkimage.sh` fills it with `makefs(8)` and gives it its own filesystem
id. Without `makefs` on the build host the partition stays empty and the
ramdisk on the ESP is the root.

## Kernel

`kern/ufs2.c` reads and writes FreeBSD's on-disk format. Allocation follows
`ffs_alloc.c`, the block map `ffs_balloc.c` (three levels of indirect
blocks, holes, fragments), truncation `ffs_truncate`, and directories
`ufs_lookup.c`. Short symlinks are stored in the inode.

A writable mount clears `fs_clean` and sets it again on unmount. A volume that
needs fsck, or has soft updates journaling or gjournal, mounts read-only.
Check hashes (`FS_METACKHASH`) are not kept; the flag is cleared on the first
writable mount and FreeBSD turns it back on later.

## Journal

Volumes on a real disk get a metadata journal in `/.journal`
(`kern/ufs2log.c`), created on the first writable mount. It is 1 MB per GB
of volume, between 1 MB and 64 MB. It works like NetBSD's WAPBL but the
format is our own.

- Every metadata change happens inside a transaction. Changed blocks stay in
  the cache until the transaction is in the log.
- A commit happens every second, on `fsync(2)`, on unmount, or when a
  transaction reaches half the log. Data goes home first, then the disk
  cache is flushed, the transaction is written with a CRC-32C, and the cache
  is flushed again.
- Freed blocks stay allocated until the transaction is logged. Freed
  metadata blocks are revoked so replay does not bring them back.
- Open files with no name are listed in each commit and freed after a crash.
- After a crash the next writable mount replays the log. No fsck needed.

To FreeBSD, NetBSD and OpenBSD the journal is an ordinary file (root, 0400,
`SF_IMMUTABLE`) and there is no superblock flag, since NetBSD refuses unknown
flags. A clean volume is plain UFS2 everywhere. After a crash, their fsck
repairs it without the log and marks it clean, and we then ignore the old
log. The log records `fs_mtime` to detect this.

### Format

Two 512-byte header slots (magic `PXUFSJNL`), the newer one wins. Then a
ring of 512-byte aligned records, each with a 32-byte header (magic `PXJR`):

| type | contents |
|---|---|
| `BLOCKS` | up to 30 (sector, length) pairs, then the data |
| `REVOKE` | up to 30 freed (sector, length) pairs |
| `ORPHANS` | up to 120 inode numbers |
| `COMMIT` | CRC-32C and record count of the transaction |

A transaction is valid only if all its records have the right sequence
number and the checksum matches.

### Disk caches

All disk drivers flush the write cache (FLUSH CACHE on ATA, Flush on NVMe,
SYNCHRONIZE CACHE on USB) before power off, before sleep and on `fsync(2)`.

Ramdisk volumes have no journal. An unclean volume without a journal mounts
read-only. For the root, init runs `fsck_ffs -y` and reboots.

A read-only mount over a live journal reads through it, like NetBSD's
`wapbl_replay_read`.

## Tools

- `newfs(8)` uses the layout of FreeBSD's `mkfs.c`.
- `fsck_ffs(8)` runs the five passes plus pass 1b. `fsck` picks it for UFS2.
- `fstorture` exercises the write path on a mounted volume.

## Limits

- At most 8 mounted volumes.
- All I/O goes through the buffer cache.
- Extended attributes are kept but not interpreted.
