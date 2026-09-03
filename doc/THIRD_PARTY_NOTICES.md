# Third-party notices

## FreeBSD source references and vendored networking code

The FNU/OpenProninx UFS2 reader is an independent, read-only implementation of
the on-disk layouts documented by these FreeBSD source files:

- `sys/ufs/ffs/fs.h`: BSD-3-Clause;
- `sys/ufs/ufs/dinode.h`: BSD-2-Clause AND BSD-3-Clause;
- `sys/ufs/ufs/dir.h`: BSD-3-Clause.

The reference revision is `4aea6ea2eb400737837ff8d22c25688f88c7966c`.

`third_party/freebsd-net` is a source-only vendor import of FreeBSD's
`sys/net`, `sys/netinet`, `sys/netinet6`, `sys/netipsec`, `sys/netlink`,
`sys/netpfil` and required `sys/sys` headers from that exact revision. It
includes FreeBSD's `COPYRIGHT` file. It is not a FreeBSD kernel import:
PRONINX boot, VM, scheduler, VFS, drivers and userspace remain PRONINX code.

Local modifications to the vendor tree are forbidden. PRONINX glue belongs
under `kern/net/`; each vendor update must record its exact revision, license
notices, file selection and local adapter changes here.

## lwIP 2.2.1

`third_party/lwip` is an unmodified source-only import of lwIP release
`STABLE-2_2_1_RELEASE`, downloaded from the upstream lwIP project.  Its
BSD-3-Clause license is retained in `third_party/lwip/COPYING`.  The
PRONINX-specific, single-core port is in `kern/net/lwip/`.

The source reference checkout at `third_party/freebsd-src` remains development
material only; it is not a release dependency and is deliberately ignored by
Git.
