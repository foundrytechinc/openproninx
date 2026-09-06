# FNU/OpenProninx

FNU/OpenProninx (short name: OpenProninx) is a native 64-bit x86-64 operating system featuring Symmetrical Multiprocessing (SMP), an extensible device driver framework, a native networking subsystem, a supervisor/service architecture, and a rich userspace environment. The current baseline is **Core 0.1.5-dev**.

---

## Key Features & Capabilities

- **Symmetrical Multiprocessing (SMP):** Native 64-bit multi-core boot via APIC INIT-SIPI-SIPI, 16-bit to 64-bit trampoline (`entryother.S`), and a pre-emptive SMP scheduler across all available CPU cores.
- **Extensible Driver Framework:** Unified device and driver model with automated PCI bus enumeration, dynamic match probing, safe Memory-Mapped I/O mapping (`ioremap`), and centralized IRQ / polling dispatching. See [`doc/DRIVERS.md`](doc/DRIVERS.md).
- **Gigabit Networking Subsystem:** Native Intel E1000 Ethernet driver (`em0`) and VirtIO-Net (`vtnet0`) coupled with an integrated `lwIP` (v2.2.1) protocol stack supporting DHCP, DNS, ARP, IPv4, ICMP, UDP, and TCP. See [`doc/NETWORK.md`](doc/NETWORK.md).
- **VBE Framebuffer & Display:** Kernel-managed VBE framebuffer text console (1024x768 / up to 1920x1080) with custom VGA font rendering and automatic fallback. See [`doc/VIDEO.md`](doc/VIDEO.md).
- **Storage Subsystems:** IDE block storage supporting both legacy root filesystems and UFS2 filesystem volumes (`/data`). See [`doc/STORAGE.md`](doc/STORAGE.md).
- **PSH Interactive Shell & Userspace:** Built-in shell offering system utilities (`ls`, `cat`, `top`, `ps`, `netinfo`, `ping`, `resolve`, `adduser`, `chown`, `chmod`) and service supervisor management (`services`, `health`, `status`).

---

## Build and Run

### Prerequisites
A POSIX environment with `bmake`, Clang, GNU binutils, Bash, and QEMU (e.g. Ubuntu, Fedora, or WSL2 on Windows).

```sh
# Build kernel and disk images
bmake

# Run automated smoke test suite
bmake smoke

# Launch QEMU with SMP (2 cores) and Intel E1000 networking
bmake qemu

# Run with custom CPU core count
bmake qemu CPUS=4

# Optional: Attach UFS2 data volume
bmake qemu UFS2_DATA_IMG=/path/to/fnu-data.ufs
```

---

## Documentation

- [`doc/ABI.md`](doc/ABI.md): System call ABI reference — stable core ABI (frozen numbers and signatures) and experimental subsystems.
- [`doc/DRIVERS.md`](doc/DRIVERS.md): Driver framework architecture, lifecycle, and driver development guide.
- [`doc/SMP.md`](doc/SMP.md): 64-bit Symmetrical Multiprocessing boot sequence and topology.
- [`doc/NETWORK.md`](doc/NETWORK.md): Network stack integration, supported NICs, and socket capabilities.
- [`doc/STORAGE.md`](doc/STORAGE.md): Filesystem layouts, UFS2 volume management, and IDE drivers.
- [`doc/VIDEO.md`](doc/VIDEO.md): VBE framebuffer configuration and graphics subsystem.
- [`CHANGELOG-RELEASE.TXT`](CHANGELOG-RELEASE.TXT): Release notes and major changes overview.

---

## Licensing & Provenance

This repository is distributed under the BSD 3-Clause License. See [`LICENSE`](LICENSE) and [`COPYRIGHT`](COPYRIGHT) for details.
