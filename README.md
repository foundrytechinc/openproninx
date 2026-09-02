# FNU/PRONINX

FNU/PRONINX (*Foundry is Not Unix*) is the Foundry Tech Inc. continuation of
the PRONINX software heritage from Pronin Software Distribution. The repository
currently contains the **Core 0.1.0-dev** foundation: a small x86-64 kernel,
boot path, native userspace and QEMU disk images.

It is not yet a production server operating system. In particular, it has no
network stack, SSH service, UEFI Secure Boot chain, NVMe driver,
access-control model, or signed update verifier. Its local service supervisor
is a development facility, not an enterprise-grade service manager. See
[`SECURITY.md`](SECURITY.md), [`doc/CORE_V1.md`](doc/CORE_V1.md), and
[`doc/STORAGE.md`](doc/STORAGE.md) before evaluating or deploying it.

## Build and run

Builds require a POSIX environment with GNU Make, GCC, GNU binutils, Bash and
coreutils; QEMU is required to run the result. On Windows, use a configured
WSL/Linux or CI runner rather than PowerShell directly.

```sh
make ci
make qemu
# Optional, read-only UFS2 FNU Data volume:
make qemu UFS2_DATA_IMG=/absolute/path/to/fnu-data.ufs
# UFS2 root system image, replacing legacy fs.img at IDE device 1:
make qemu UFS2_SYSTEM_IMG=/absolute/path/to/fnu-system.ufs
```

On the audited release-builder, create that image with `make system-image`.
It uses external BSD-compatible `makefs`; ordinary development and runtime do
not require FreeBSD tooling because signed system images are release artifacts.
The CI workflow produces `fnu-proninx-ufs2-system` through a FreeBSD VM and
checks it with `fsck_ffs -n`.

The build creates `obj/PRONINX.img` (boot/kernel disk) and `obj/fs.img`
(userspace filesystem). `make` does not format or otherwise alter source
files. Run `make format` only when a source-format change is intended.

## Current developer interfaces

The QEMU console starts PSH as the local development administration shell. It
retains its native utilities `ls`, `cat`, `cp`, `wc`, `top`, `mkdir`, and
filesystem/process tests, and adds the FNU commands `status`, `services`,
`health`, and `start|stop|restart health`. These are development facilities,
not an authenticated enterprise administration interface.

The supervisor command queue and the `services`/`health` snapshots are
volatile kernel state devices. They work with both the legacy image and the
immutable UFS2 system image, but are reset at boot and are not an audit log.

With a prepared UFS2 data image attached, its read-only namespace is `/data`:
PSH commands such as `cd /data`, `ls /data`, and `cat /data/<file>` use it
without modifying the legacy root image.

## Licensing and provenance

This repository is MIT-licensed; retain the notices in `LICENSE` and
`COPYRIGHT`. The latter records upstream material from xv6, JOS and
rust-osdev/bootloader.
