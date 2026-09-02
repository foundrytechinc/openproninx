# PRONINX networking integration

## Boundary

PRONINX retains its own boot path, memory manager, scheduler, VFS, drivers and
userspace. FreeBSD networking code is a vendored protocol component, not an
operating-system base. Its immutable source lives in `third_party/freebsd-net`;
all adaptation is PRONINX-owned code in `kern/net`.

## Source provenance

The initial vendor import is pinned to FreeBSD revision
`4aea6ea2eb400737837ff8d22c25688f88c7966c` and was copied without local
changes. See `THIRD_PARTY_NOTICES.md` for the selected directories and
licenses.

## Delivery order

1. Define the PRONINX network-interface, DMA-buffer, lock, timer and memory
   adapter APIs. No FreeBSD internal kernel subsystem is imported.
2. Implement a PRONINX VirtIO-net driver and test link, ARP, ICMP and packet
   loss in QEMU.
3. Bind the imported mbuf, interface and IPv4/UDP paths to that adapter.
4. Add TCP, IPv6, firewalling, routing and the PRONINX socket ABI with packet
   fuzzing and interoperability tests.
5. Add a PRONINX driver for one certified production NIC with MSI-X, RSS,
   multiqueue DMA and NUMA-aware queue placement.

The stack must be updated as a tracked vendor operation: pin an upstream
revision, preserve all notices, review CVEs and run protocol, fuzz, throughput
and p99-latency regression tests before release.
