# Third-party notices

## UFS2 on-disk format reference

The FNU/OpenProninx UFS2 reader is an independent, read-only implementation of
the on-disk layouts documented by these source files from the FreeBSD project:

- `sys/ufs/ffs/fs.h`: BSD-3-Clause;
- `sys/ufs/ufs/dinode.h`: BSD-2-Clause AND BSD-3-Clause;
- `sys/ufs/ufs/dir.h`: BSD-3-Clause.

The reference revision is `4aea6ea2eb400737837ff8d22c25688f88c7966c`.

No FreeBSD source code is compiled into the OpenProninx kernel or userland.
The UFS2 implementation in `kern/ufs2.c` / `kern/ufs2.h` was written
independently using the above specifications as documentation only.

## lwIP 2.2.1

`third_party/lwip` is an unmodified source-only import of lwIP release
`STABLE-2_2_1_RELEASE`, downloaded from the upstream lwIP project.  Its
BSD-3-Clause license is retained in `third_party/lwip/COPYING`.  The
PRONINX-specific, single-core port is in `kern/net/lwip/`.
