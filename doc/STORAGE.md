# FNU/OpenProninx storage architecture

## Decision

FNU/OpenProninx uses **UFS2/FFS** as its writable filesystem. It keeps the
BSD licensing direction and avoids both a homemade format and the weight of
ZFS or GPLv2 lwext4. The xv6 filesystem is gone. The loader's ramdisk is a
UFS2 image made from the same tree as the root partition.

## Volume roles

| Volume | Format | Role |
|---|---|---|
| EFI System Partition | FAT32 | UEFI firmware compatibility only |
| System A / System B | signed immutable images | operating-system update and rollback slots |
| FNU Data | UFS2/FFS | service data, durable state, audit data and logs |
| Recovery | immutable image | local recovery and diagnostics |

FAT32 must not be selected as the root, service-state, log, or data volume.

## Root

The root is the GPT partition `fnu-root`, made by `tools/mkimage.sh` with
`makefs(8)`. Both loaders read the GPT of the boot disk, find `fnu-root`, read
`/etc/filesystems` from it (`boot/ufs2read.c`) and pass the `/` line to the
kernel as `vfs.root.mountfrom`, the same way FreeBSD does. The build writes
that line with the partition's own UUID.

If nothing names a disk root, the kernel uses the ramdisk. If the named root
cannot be mounted it asks at `mountroot>` (`?` lists disks). USB holds the
root mount until its first scan is done, and a missing device is waited for
up to `vfs.mountroot.timeout` seconds (3).

Other UFS2 and FAT volumes are mounted with `mount(8)` or from
`/etc/filesystems`. `kern/fat32.c` handles FAT12, FAT16 and FAT32.

## Device names

GPT partitions are devices of their own (`wd0p3`). Stable names, as in
DragonFly:

- `/dev/part-by-uuid/`, `/dev/part-by-label/` for GPT entries
- `/dev/fs-by-uuid/`, `/dev/fs-by-label/` for filesystems
- `/dev/serno/SERIAL` for a disk, `pN` appended for a partition

The first device to claim a name keeps it. Duplicates are reported.

## Buffer cache

The cache takes a sixteenth of memory and is hashed on device and block
number. Buffers sit on three queues as in 4.4BSD: free, AGE and dirty.
Requests are at most `MAXPHYS` (64 KB). Read-ahead only kicks in for
sequential reads. Full blocks are delayed writes and go out in 64 KB
clusters. The rest waits for the 30 second syncer, `sync(2)` or an unmount.

`O_DIRECT` (`FNU_OPEN_UNCACHED`) still uses the cache but does not keep the
data. `O_SYNC` (`FNU_OPEN_WRITETHROUGH`) writes at once and flushes the disk
cache. Block numbers are 64 bit all the way down.

## Disk drivers

- IDE attaches only to PCI class 01:01 controllers, in compatibility or
  native mode. It uses bus master DMA when there is one and LBA48 when
  needed.
- AHCI and NVMe use DMA. NVMe uses PRP lists, honours MDTS and keeps
  several commands in flight.
- USB mass storage uses READ(16)/WRITE(16) on big disks and 64 KB transfers
  from DMA memory below 4 GB. Devices on the quirk list
  (`kern/usb/usb_quirk.c`, or `hw.usb.quirk.N` from the loader) get no
  SYNCHRONIZE CACHE, for example the Kingston DataTraveler 3.0. Disks are
  stopped at power off and started again with START UNIT.

## FAT

FAT caches the last cluster position per file, like msdosfs, so appends do
not walk the whole chain. `chmod` keeps only the read-only bit. `chown` only
accepts the current owner. `link` and `symlink` fail. `stat` returns
`st_ino` and `st_blocks` on every filesystem.

The sticky bit on a directory limits `unlink` and `rename` to the owner of
the file, the owner of the directory, or root. `/tmp` has it.

## Kernel messages across reboots

The message buffer lives at the top of memory and survives a warm reboot. The
T520's coreboot clears memory, so at shutdown and on panic the buffer is also
written to the end of `fnu-swap`. The next boot reads it back and `dmesg`
shows it once.

## Tools

`newfs(8)`, `fsck_ffs(8)` and `fsck(8)` are included. The build needs
`makefs(8)` on the host.

## Validation

- Images made by FreeBSD's `makefs` mount, and images written here check
  clean with an independent fsck and read back byte for byte.
- Invalid superblocks, out-of-range block references and damaged cylinder
  groups are refused without a panic.
- Volumes interrupted mid-write are repaired by `fsck_ffs` and mount again.
