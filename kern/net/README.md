# PRONINX network adapter

This directory contains PRONINX-owned glue between kernel facilities
and the vendored lwIP networking stack. It must not become a copy of a
foreign boot path, scheduler, VFS or driver framework.

The adapter layer provides a narrow interface for NIC drivers: DMA buffers,
queue notification, interrupt dispatch, monotonic timers, memory allocation
and locks.
