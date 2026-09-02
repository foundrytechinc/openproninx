# Third-party notices

## FreeBSD UFS2 on-disk format reference

The FNU/PRONINX UFS2 reader is an independent, read-only implementation of
the on-disk layouts documented by these FreeBSD source files:

- `sys/ufs/ffs/fs.h`: BSD-3-Clause;
- `sys/ufs/ufs/dinode.h`: BSD-2-Clause AND BSD-3-Clause;
- `sys/ufs/ufs/dir.h`: BSD-3-Clause.

Reference revision: `4aea6ea2eb400737837ff8d22c25688f88c7966c`.

No FreeBSD kernel code, build system, VFS layer, or FreeBSD userspace is part
of FNU/PRONINX. Before copying any upstream source into this repository, the
exact file, revision, license text and changes must be recorded here and the
original notices retained in that file.

The source reference checkout at `third_party/freebsd-src` is development
material only; it is not a release dependency and must not be committed as an
embedded Git repository. A future import must be a deliberately selected,
audited source snapshot or a documented submodule.
