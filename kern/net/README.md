# PRONINX network adapter

This directory is reserved for PRONINX-owned glue between kernel facilities
and the vendored FreeBSD networking source. It must not become a copy of a
foreign boot path, scheduler, VFS or driver framework.

The first deliverable is a narrow adapter for VirtIO-net: DMA buffers, queue
notification, interrupt dispatch, monotonic timers, memory allocation and
locks. The vendor import is intentionally not part of the build until those
interfaces are defined and tested.
