# FNU/OpenProninx

FNU/OpenProninx (short name: OpenProninx) is a native x86-64 operating system
with its own boot code, kernel, userspace and QEMU disk images. The current
baseline is **Core 0.1.0-dev**.

It is not production-ready. Secure boot, signed updates, NVMe support, a
complete access-control model and hardened remote access are not implemented.
The local process supervisor and account tools currently provide only the
capabilities documented below. See
[`SECURITY.md`](SECURITY.md), [`doc/CORE_V1.md`](doc/CORE_V1.md), and
[`doc/STORAGE.md`](doc/STORAGE.md) before evaluating it.

## Build and run

Builds require a POSIX environment with `bmake`, Clang, GNU binutils, Bash and
coreutils; QEMU is required to run the result. On Fedora, install Clang with
`sudo dnf install clang binutils`. On Windows, use a configured
WSL/Linux or CI runner rather than PowerShell directly.

```sh
bmake ci
bmake qemu
# Optional, read-only UFS2 FNU Data volume:
bmake qemu UFS2_DATA_IMG=/absolute/path/to/fnu-data.ufs
# UFS2 root system image, replacing legacy fs.img at IDE device 1:
bmake qemu UFS2_SYSTEM_IMG=/absolute/path/to/fnu-system.ufs
```

On the audited release-builder, create that image with `bmake system-image`.
It uses external BSD-compatible `makefs`; ordinary development and runtime do
not require FreeBSD tooling because signed system images are release artifacts.
The CI workflow produces `fnu-proninx-ufs2-system` through a FreeBSD VM and
checks it with `fsck_ffs -n`.

The build creates `obj/PRONINX.img` (boot/kernel disk) and `obj/fs.img`
(userspace filesystem). `bmake` does not format or otherwise alter source
files. Run `bmake format` only when a source-format change is intended.

## Current capabilities

The QEMU console starts PSH, the local interactive shell. It provides native
utilities such as `ls`, `cat`, `cp`, `wc`, `top` and `mkdir`, filesystem and
process tests, plus the FNU commands `status`, `services`, `health`, and
`start|stop|restart health`.

The BIOS/QEMU path also provides a kernel-private VBE framebuffer text console
with scaled VGA-font rendering and VGA fallback. It is not yet a userspace
graphics API; see [`doc/VIDEO.md`](doc/VIDEO.md).

Local accounts can be created with `adduser [-w] NAME` (`useradd` is an
equivalent spelling). The command reads a password from the console; passwords
must contain at least eight characters, and `-w` adds the account to wheel.
Root can change file ownership with `chown UID:GID PATH` and permissions with
`chmod MODE PATH`.

The supervisor command queue and the `services`/`health` snapshots are
volatile kernel state devices. They work with both the legacy image and the
immutable UFS2 system image, but are reset at boot and are not an audit log.

With a prepared UFS2 data image attached, its namespace is `/data`. It supports
UFS2 file and empty-directory creation, writes, hard links and removal, but
does not yet provide crash recovery. PSH commands such as
`cd /data` and `ls /data` use it without modifying the legacy root image.

The repository contains a UEFI entry-point stub, but it only prints a greeting
and exits. It does not load the kernel, configure paging, implement Secure
Boot, or provide a working UEFI boot path.

## Licensing and provenance

This repository is distributed under the BSD 3-Clause License. Retain the
notices in `LICENSE` and `COPYRIGHT`.
