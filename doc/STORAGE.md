# FNU/PRONINX storage architecture

## Decision

FNU/PRONINX will use **UFS2/FFS** as the planned mutable
data filesystem. This preserves the BSD-family licensing direction while
avoiding an in-house filesystem format and the licensing/complexity burden of
ZFS or GPLv2 lwext4.

This decision does not make the current native filesystem production-ready.
The current xv6-derived filesystem remains the legacy boot/development
filesystem until the UFS2 path passes its defined validation gates.

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
attaches that volume to the legacy VFS and never writes it. The current image
that boots `init` and PSH remains IDE device 1 and is untouched.

When a valid FNU Data image is supplied, it is exposed read-only at `/data`.
When a valid UFS2 system image replaces IDE device 1, it becomes the read-only
root volume. The initial adapter supports regular files, directories, direct
blocks and one level of indirect UFS2 blocks. The current PRONINX pathname ABI
limits individual path components to
13 ASCII bytes; long UFS2 names, symlinks, indirect blocks, writes and metadata
updates remain deliberately unavailable until the VFS ABI is replaced.

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
- Review the imported code, build flags and SBOM before a production claim.
