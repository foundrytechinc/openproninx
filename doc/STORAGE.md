# FNU/OpenProninx storage architecture

## Decision

FNU/OpenProninx will use **UFS2/FFS** as the planned mutable
data filesystem. This preserves the BSD-family licensing direction while
avoiding an in-house filesystem format and the licensing/complexity burden of
ZFS or GPLv2 lwext4.

This decision does not change the current native filesystem implementation.
The xv6-derived filesystem remains the legacy boot filesystem until the UFS2
path passes its defined validation gates.

## Volume roles

| Volume | Format | Role |
|---|---|---|
| EFI System Partition | FAT32 | UEFI firmware compatibility only |
| System A / System B | signed immutable images | operating-system update and rollback slots |
| FNU Data | UFS2/FFS | service data, durable state, audit data and logs |
| Recovery | immutable image | local recovery and diagnostics |

FAT32 must not be selected as the root, service-state, log, or data volume.

## Porting boundary

The UFS2 port must be isolated behind an FNU block-device and VFS adapter. It
must not expose FreeBSD kernel internals across the rest of PRONINX. Every
imported file must retain its original license notice and be recorded with its
upstream revision in an SBOM. The initial on-disk-format attribution is in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

The first milestone is read-only mounting of a known-good UFS2 image in QEMU.
Write support is blocked until file reads, directory traversal, corrupt-image
rejection and offline repair with FreeBSD tooling are demonstrated.

## Current implementation

The kernel now has a bounded, read-only `blockdev` adapter and UFS2 format
reader derived from the FreeBSD UFS2 format at revision
`4aea6ea2eb400737837ff8d22c25688f88c7966c`. It validates a superblock, reads
UFS2 inodes, reads direct data blocks, and can resolve a directory entry.
All offsets are bounds-checked and corrupt directory records are rejected.

The kernel now discovers an optional dedicated IDE device 2, validates it as a
UFS2 volume and verifies the root inode and `.` directory record. It never
attaches that volume to the legacy VFS and never writes it. When no external
IDE device 1 is attached, OpenProninx boots as a fully self-contained LiveCD
using its built-in in-memory root filesystem ramdisk (`DEV_RAMDISK`). When an
external volume is attached to IDE device 1, it takes priority and can serve as
either an external native root filesystem or a UFS2 system image.

When a valid FNU Data image is supplied, it is exposed at `/data`.
When a valid UFS2 system image replaces IDE device 1, it becomes the root
volume. System images provide conventional top-level directories `/etc`,
`/mount`, `/dev`, `/tmp`, `/usr` and `/home`; user programs live in `/usr/bin`.
The initial adapter supports regular files, directories,
direct blocks and one level of indirect UFS2 blocks. UFS2 names of up to 255
bytes traverse the VFS and are returned through `getdents`; the legacy xv6
filesystem retains its 13-byte component limit. The current UFS2 writer can
create files and directories, write direct blocks, make and remove hard links,
and remove empty directories. Symlinks, double/triple indirection and
crash-safe metadata updates remain unavailable.

For QEMU, attach a prepared raw UFS2 image as the optional device:

```sh
make qemu UFS2_DATA_IMG=/absolute/path/to/fnu-data.ufs
```

To exercise a complete UFS2 system root instead of the legacy `fs.img`:

```sh
make qemu UFS2_SYSTEM_IMG=/absolute/path/to/fnu-system.ufs
```

`make system-image` stages the FNU userspace and invokes the host's audited
BSD-compatible `makefs` with `version=2`. It intentionally fails when that
tool is unavailable: PRONINX does not contain a second, homemade UFS formatter.
Release engineering publishes the resulting image for ordinary Linux/WSL
operators, who only need `make qemu UFS2_SYSTEM_IMG=...`.

The GitHub Actions release path builds the kernel and userspace on Ubuntu,
then creates and checks `fnu-system.ufs` inside a FreeBSD VM. This keeps the
UFS implementation tooling out of the target OS and out of normal developer
workstations while preserving FreeBSD-format compatibility.

There is no FNU `mkfs`: UFS2 volumes are provisioned by installer or
image-building tooling using a compatible, audited implementation. The next
storage milestone is a partition manager and VFS attachment of this dedicated
data device.

## Required validation before writable use

- Mount and read UFS2 images produced by a supported FreeBSD release.
- Reject invalid superblocks, out-of-range block references and corrupted
  cylinder-group metadata without a kernel panic.
- Run interruption tests over create, write, rename and delete operations.
- Verify recovery using the compatible FreeBSD `fsck_ffs` tool.
- Review the imported code, build flags and SBOM before declaring writable UFS2
  support complete.
