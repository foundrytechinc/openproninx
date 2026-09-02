# Third-party notices

## FreeBSD source references and vendored networking code

The FNU/PRONINX UFS2 reader is an independent, read-only implementation of
the on-disk layouts documented by these FreeBSD source files:

- `sys/ufs/ffs/fs.h`: BSD-3-Clause;
- `sys/ufs/ufs/dinode.h`: BSD-2-Clause AND BSD-3-Clause;
- `sys/ufs/ufs/dir.h`: BSD-3-Clause.

The reference revision is `4aea6ea2eb400737837ff8d22c25688f88c7966c`.

The selected networking source is FreeBSD's `sys/net`, `sys/netinet`,
`sys/netinet6`, `sys/netipsec`, `sys/netlink`, `sys/netpfil` and required
`sys/sys` headers from that exact revision. When the vendor import is
available, it will live in `third_party/freebsd-net` with FreeBSD's
`COPYRIGHT` file. It is not a FreeBSD kernel import: PRONINX boot, VM,
scheduler, VFS, drivers and userspace remain PRONINX code.

Local modifications to the future vendor tree are forbidden. PRONINX glue
belongs under `kern/net/`; each vendor import or update must record its exact
revision, license notices, file selection and local adapter changes here.

The source reference checkout at `third_party/freebsd-src` remains development
material only; it is not a release dependency and is deliberately ignored by
Git.
