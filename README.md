# OpenProninx

**OpenProninx** is a free and independent operating system distributed under the **BSD 3-Clause License**. It is developed by **Foundry Tech Inc.**, founded by Vyacheslav Pronin.

OpenProninx originated from **PRONINX**, an earlier operating system created by the same developer as part of Pronin Software Distribution. PRONINX was originally based on the **xv6** kernel, specifically its **x86_64 port**.

OpenProninx is no longer an educational operating system based on xv6. It has evolved into a standalone operating system with its own kernel subsystems, userspace, drivers, networking, storage, and graphics (framebuffer) support.

---

## FNU — Foundry Not Unix

**FNU (Foundry Not Unix)** is a collection of free, **GPL-free userspace software** developed specifically for OpenProninx.

It includes system software such as:

* `init`
* **Proninx Shell**
* other userspace utilities and components

FNU provides the software required by OpenProninx rather than attempting to reproduce an existing Unix userspace.

---

## What FNU Is Not

### FNU is not an operating system

The operating system is **OpenProninx**.

FNU is the userspace software developed for it.

### FNU is not a GNU replacement

FNU is not intended to reimplement:

* `bash`
* `coreutils`
* `glibc`
* or the GNU userspace as a whole

The goal is to provide the software specifically required by OpenProninx.

### FNU is not a compatibility layer

FNU software is written for **OpenProninx**. It is not intended to run on Linux or other operating systems.

---

## Licensing

All FNU components are distributed under the **BSD 3-Clause License**, the same license used by OpenProninx.

No FNU component will depend on code distributed under the **GPL** or **LGPL**.

This licensing policy is intentional and applies to the project as a whole.

---

## Documentation

* [`doc/ABI.md`](doc/ABI.md): System call ABI reference — stable core ABI (frozen numbers and signatures) and experimental subsystems.
* [`doc/DRIVERS.md`](doc/DRIVERS.md): Driver framework architecture, lifecycle, and driver development guide.
* [`doc/SMP.md`](doc/SMP.md): 64-bit Symmetrical Multiprocessing boot sequence and topology.
* [`doc/NETWORK.md`](doc/NETWORK.md): Network stack integration, supported NICs, and socket capabilities.
* [`doc/STORAGE.md`](doc/STORAGE.md): Filesystem layouts, UFS2 volume management, and IDE drivers.
* [`doc/VIDEO.md`](doc/VIDEO.md): VBE framebuffer configuration and graphics subsystem.
* [`doc/UFS2_WRITE.md`](doc/UFS2_WRITE.md): UFS2 write support.
* [`doc/THIRD_PARTY_NOTICES.md`](doc/THIRD_PARTY_NOTICES.md): Third-party software and license notices.

---

## Quick Start

### Build an image

```sh
bmake
```

### Run with QEMU

To build and automatically launch OpenProninx in QEMU:

```sh
bmake qemu
```

### Requirements

* `bmake`
* `clang`
* QEMU (for `bmake qemu`)

---

## License

OpenProninx and FNU are distributed under the **BSD 3-Clause License**.

Copyright © 2026 **Foundry Tech Inc.**
