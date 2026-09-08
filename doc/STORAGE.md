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

The kernel now provides a modular storage architecture supporting:
- **Hybrid GPT Partitioning:** Generated via `tools/mkimage.sh`, containing an ESP (FAT32 for UEFI), `fnu-boot`, and `fnu-root` partitions.
- **Hardware Block Drivers:** Full PCI auto-probing and support for IDE/ATA, AHCI SATA (with COMRESET, IDENTIFY, and SATA Gen negotiation), and NVMe (with spec 1.4 compliant shutdown).
- **RAMDisk Boot Module:** The root filesystem is passed cleanly as a boot module from the BIOS/UEFI bootloader via `bootinfo`, removing the need to embed the disk image directly inside the kernel binary.
- **VFS Enhancements:** Dedicated path buffers (`MAXPATHLEN 512`, `MAXNAMLEN 255`), `getcwd(2)` with parent traversal (`..`), and robust inode reuse in `iget()` ensuring `fs_type` state isolation across filesystem drivers.
- **UFS2 Integration:** A bounded, read-only `blockdev` adapter and UFS2 format reader derived from FreeBSD UFS2 specifications (revision `4aea6ea2eb400737837ff8d22c25688f88c7966c`). It validates superblocks, reads UFS2 inodes and direct/indirect blocks, bounds-checks offsets, and exposes valid FNU Data images at `/data`.

When booted without external data disks, OpenProninx operates as a self-contained system using the bootloader-provided root ramdisk (`DEV_RAMDISK`). When an external volume or secondary partition is attached, it is recognized and integrated into the storage hierarchy.

For QEMU, attach a prepared raw UFS2 image as the optional device:

```sh
bmake qemu UFS2_DATA_IMG=/absolute/path/to/fnu-data.ufs
```

To exercise a complete UFS2 system root instead of the default root image:

```sh
bmake qemu UFS2_SYSTEM_IMG=/absolute/path/to/fnu-system.ufs
```

`bmake system-image` stages the FNU userspace and invokes the host's audited BSD-compatible `makefs` with `version=2`. Release engineering publishes the resulting image for ordinary Linux/WSL operators.

## Required validation before writable use

- Mount and read UFS2 images produced by a supported FreeBSD release.
- Reject invalid superblocks, out-of-range block references and corrupted
  cylinder-group metadata without a kernel panic.
- Run interruption tests over create, write, rename and delete operations.
- Verify recovery using the compatible FreeBSD `fsck_ffs` tool.
- Review the imported code, build flags and SBOM before declaring writable UFS2
  support complete.
