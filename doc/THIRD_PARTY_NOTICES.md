# Third-party notices

## UFS2 on-disk format reference

The FNU/OpenProninx UFS2 filesystem is an independent implementation of
the on-disk layouts documented by these source files from the FreeBSD project:

- `sys/ufs/ffs/fs.h`: BSD-3-Clause;
- `sys/ufs/ufs/dinode.h`: BSD-2-Clause AND BSD-3-Clause;
- `sys/ufs/ufs/dir.h`: BSD-3-Clause.

The reference revision is `4aea6ea2eb400737837ff8d22c25688f88c7966c`.

No FreeBSD source code is compiled into the OpenProninx kernel or userland.
The UFS2 implementation in `kern/ufs2.c` / `kern/ufs2.h` was written
independently using the above specifications as documentation only.

## lwIP 2.2.1

`third_party/lwip` is a source-only import of lwIP release
`STABLE-2_2_1_RELEASE`, downloaded from the upstream lwIP project.  Its
BSD-3-Clause license is retained in `third_party/lwip/COPYING`.  The
PRONINX-specific, single-core port is in `kern/net/lwip/`.

One local change: `tcp_update_rcv_ann_wnd()` in `src/core/tcp.c` compares
the MSS in the window type, so a scaled `TCP_WND` above 128 KB does not
compare a `u16_t` against a constant it can never reach.

## x86emu, the real-mode emulator

`third_party/x86emu/` is OpenBSD's `sys/dev/x86emu` (`x86emu.c`,
`x86emu.h`, `x86emu_regs.h`), the realmode X86 emulator library:
Copyright (C) 1996-1999 SciTech Software, Inc., (C) David Mosberger-Tang,
(C) 1999 Egbert Eich, (C) 2007 Joerg Sonnenberger; permission to use,
copy, modify, distribute and sell it is granted without fee provided the
copyright notice and the permission notice appear in all copies. Its
`port/sys/` headers and `kern/x86bios.c`, which runs the video bios in it
after FreeBSD's `x86bios` and coreboot's YABEL, are ours.

## Spleen console fonts

`kern/font_spleen.c` carries the 8x16, 12x24, 16x32 and 32x64 faces of the
Spleen 2.2.0 release, turned from its BDF files by `tools/fontcvt`, under
their BSD-2-Clause notice (Copyright (c) 2018-2026 Frederic Cambus), which
the file keeps.

## The C library

`lib/libc/` is ours POSIX module. What it takes from elsewhere,
each file keeping its notice:

- current OpenBSD's (BSD-2/3-Clause, ISC): all of `stdio/`; in `stdlib/`
  `abort`, `abs`, `atexit`, `atof`, `bsearch`, `div`, `ldiv`, `lldiv`,
  `llabs`, `exit`, `getenv`, `setenv`, `getopt_long`, `heapsort`, `qsort`,
  `imaxabs`, `imaxdiv`, `malloc` (Otto Moerbeek, Matthew Dempsky, Damien
  Miller; Poul-Henning Kamp's design), `random`, `reallocarray`,
  `realpath`, `strtonum` and the `strto*` family; in `string/` `memchr`,
  `strcasecmp`, `strcoll`, `strcspn`, `strdup`, `strlcat`, `strmode`,
  `strncat`, `strndup`, `strnlen`, `strpbrk`, `strsep`, `strspn`,
  `strstr`, `strxfrm` and the `wcs*`/`wmem*` functions; in `gen/`
  `assert`, `basename`, `dirname`, `fnmatch` with `charclass.h` (public
  domain, Todd C. Miller), `fpclassify`, `isfinite`, `isinf`, `isnan`,
  `isnormal`, `signbit`, `frexp`, `modf`, `fts`, `getbsize`, `getgrent`,
  `getgrouplist`, `getprogname`, `setprogname`, `getusershell`,
  `initgroups`, `popen`, `pwcache`, `pw_dup`, `setmode`, `siginterrupt`,
  `strtofflags`, `syslog`, `syslog_r`, `vis` and `unvis`; `gen/killpg.c`
  from its 4.3BSD compatibility code (BSD-3-Clause, the Regents); in `net/`
  `htonl`, `htons`, `ntohl`, `ntohs`, `inet_addr` and `inet_ntoa`; in
  `arch/amd64/` the `setjmp` family, `fabs`, `flt_rounds`, the `fpget*` and
  `fpset*` calls, `infinity`, `ldexp`, `nan` and the long double
  classifiers, with `arch/DEFS.h`; `crypt/arc4random*`;
  `include/{atfork,localedef,_stdio,thread_private}.h`;
- `gdtoa/` is David M. Gay's gdtoa (the Lucent/AT&T permissive notice)
  with OpenBSD's `hdtoa.c`, `ldtoa.c` and `strtold.c`;
- `regex/` is Henry Spencer's regex as OpenBSD carries it (BSD-3-Clause,
  Henry Spencer and the Regents); `time/` is tzcode (public domain, Arthur
  David Olson) with OpenBSD's `strptime.c`;
- `locale/` is the single-byte C locale of OpenBSD 4.4: `_def_*.c`,
  `localeconv.c`, `nl_langinfo.c` (it answers `CODESET` with US-ASCII),
  `__mb_cur_max.c`, `mbrtowc_sb.c` and `multibyte_sb.c` (BSD-2-Clause,
  the Citrus Project; `mbsrtowcs`, `wcsrtombs` and `wcrtomb` are brought to
  C99 here); `setlocale.c` and `wctype.c` are ours;
- `string/strtok.c` and `memccpy.c` are 4.4BSD's (BSD-3-Clause, the
  Regents); `string/strchrnul.c` is FreeBSD's, from musl (MIT, Rich Felker
  et al.); `gen/pw_scan.c` and `pw_scan.h` are FreeBSD's (BSD-3-Clause, the
  Regents); `crypt/chacha_private.h` is D. J. Bernstein's (public domain);
  `lib/ffs.c` is OpenBSD's `sys/lib/libkern/ffs.c` (public domain, Dale
  Rahn); `include/stdatomic.h` is FreeBSD's cut to its clang branch
  (BSD-2-Clause, Ed Schouten and David Chisnall) and `include/stdnoreturn.h`
  NetBSD's (BSD-2-Clause, The NetBSD Foundation).

`sys/`, `thread/`, the other files of `gen/`, `stdlib/misc.c` and
`mktemp.c`, `string/{explicit_bzero,strerror}.c`, `devname.c` and `uname.c`
are our own. The headers under `include/` follow
OpenBSD's.

`lib/libutil/` has OpenBSD's `fmt_scaled.c`, `login_tty.c`, `logwtmp.c` and
`pty.c`, and FreeBSD's `humanize_number.c` (BSD-2-Clause, The NetBSD
Foundation and John-Mark Gurney); the rest is ours.
`lib/libcrypt/` has OpenBSD's `bcrypt.c`, `blowfish.c`, `cryptutil.c`,
`sha256.c` and `sha512.c` (ISC, BSD-3-Clause), and `helper.c` (ISC,
Poul-Henning Kamp), from which the build makes `SHA256End` and its kin.
`lib/libcrypt/arch/amd64/` has OpenBSD's `sha256_amd64_generic.S`,
`sha256_amd64_shani.S` and `sha512_amd64_generic.S` (ISC, Joel Sing);
`sha2_amd64.c` beside them is ours, after OpenBSD's
`sha256_amd64.c`.

`user/sh/` is FreeBSD 9.0's `bin/sh`, the Almquist shell (BSD-3-Clause,
Kenneth Almquist and the Regents), with `bin/kill`, `bin/test` and
`usr.bin/printf` built in as FreeBSD builds them.

## libm

`lib/libm/` is current OpenBSD's `lib/libm`: FreeBSD's msun, from Sun's
fdlibm (Copyright (C) 1993 Sun Microsystems, "freely granted" provided the
notice is kept), with the long double sources of `src/ld80/` and the amd64
assembly of `arch/amd64/` (public domain, J.T. Conklin), each file with its
notice. `include/{math,complex,fenv,ieeefp}.h` and
`include/machine/{fenv,fpu,ieeefp}.h` are OpenBSD's.

## awk, and yacc for the build

`user/awk/` is current OpenBSD's `usr.bin/awk`, the one true awk (Copyright
(C) Lucent Technologies 1997, the permissive notice in each file).
`tools/yacc/` is OpenBSD's `usr.bin/yacc`, Berkeley yacc by Robert Paul
Corbett (BSD-3-Clause, the Regents); it runs on the host during the build
to make awk's parser, and nothing of it is installed.

## Utilities from OpenBSD

These are current OpenBSD programs, each file under its own notice
(BSD-2/3-Clause or ISC; the Regents of the University of California and
the authors the files name): `user/{apply,cal,cat,chmod,col,colrm,column,
comm,csplit,date,deroff,du,echo,env,expand,expr,false,fmt,fold,getopt,
head,hostname,id,join,jot,kill,leave,ln,logger,logname,mkdir,mktemp,
nice,nl,nohup,paste,printenv,printf,pwd,readlink,realpath,renice,rev,seq,
sleep,split,stat,sync,tee,test,time,timeout,touch,true,ts,tty,uname,
unexpand,uniq,units,unvis,wc,what,which,yes}.c`, `user/arch.c` (BSD,
Th. Lockert; it names this system where the original wrote "OpenBSD."),
and the directories `user/{banner,cmp,compress,cp,diff,ed,find,grep,
hexdump,install,lam,ls,mv,patch,pr,rs,sed,tr,vis,xargs}/`.
`share/misc/units.lib` is OpenBSD's units database. `user/unifdef/` is
Tony Finch's unifdef as OpenBSD carries it (BSD-2-Clause).

## pax, tar and cpio

`user/pax/` is current OpenBSD's `bin/pax` as it stands (BSD-3-Clause,
Copyright (c) 1992 Keith Muller and the Regents of the University of
California; the notices stay in every file). It is installed as `pax` with
`tar` and `cpio` linked to it, the way OpenBSD installs it. `user/mknod.c`
is OpenBSD's `sbin/mknod/mknod.c` (BSD-3-Clause), installed as `mknod` with
`mkfifo` linked to it.

## dd

`user/dd/` is current DragonFly's `bin/dd`, unchanged (BSD-3-Clause,
Copyright (c) 1991, 1993, 1994 the Regents of the University of California;
Keith Muller and Lance Visser; the notices stay in every file). libc maps `O_DIRECT`, `O_FSYNC`, `fdatasync` and `FIODTYPE` to our own
calls.

## rm, rmdir, cut, basename and dirname

`user/rm.c`, `user/rmdir.c`, `user/cut.c`, `user/basename.c` and
`user/dirname.c` are current OpenBSD's `bin/rm`, `bin/rmdir`,
`usr.bin/cut`, `usr.bin/basename` and `usr.bin/dirname` as they stand
(BSD-3-Clause, the Regents of the University of California; `dirname.c`
ISC, Todd C. Miller; the notices stay in every file). What `rm -P` asks of
the volume comes from `fstatfs(2)`, which libc answers from the system's
table of mounted volumes.

## vi, from neatvi

`user/vi/` is Ali Gholami Rudi's neatvi (ISC license, Copyright (C)
2015-2026 Ali Gholami Rudi; the notice stays in `vi.c`), taken at commit
26e9cadbf382 of github.com/aligrudi/neatvi. It is installed as `vi` with
`ex` linked to it. What it asks of the system goes through `user/vi/sys.c`,
written here over the system's own calls; the `:rk` command, which needs a
Unix-domain socket, is left out.

## SSH primitives from OpenBSD

`user/ssh/` carries, unchanged and with their notices: `chacha.c` and
`chacha.h` (public domain, D. J. Bernstein), `poly1305.h` (public domain,
Andrew Moon), `ed25519.c` and `crypto_api.h` (ISC, the
libsodium ref10 code as OpenSSH ships it; the header by Markus Friedl,
public domain), `smult_curve25519_ref.c` (public domain, D. J. Bernstein),
`sha256.c` and `sha512.c` (ISC, Joel Sing) and `include/sha2.h`
(BSD-3-Clause, Aaron D. Gifford). `poly1305.c` is our own, written after
Moon's public-domain poly1305-donna-64 in place of the 32-bit one OpenBSD
ships, and `chacha_x4.c` runs four ChaCha blocks at once as Ted Krovetz's
public-domain vec128 does. The Realtek driver `kern/net/re.c` is
written after OpenBSD's `sys/dev/ic/re.c` (BSD-4-Clause, Bill Paul) and
keeps that notice.

## FreeBSD kbd(4), atkbd(4), kbdcontrol(1) and the vt keymaps

`kern/kbd.c` carries the keymap engine of FreeBSD's `sys/dev/kbd/kbd.c`
(Copyright (c) 1999 Kazutaka YOKOTA) and the tables of `kbdtables.h`
(Copyright (c) 1992-1998 Søren Schmidt), `kern/atkbd.c` is
`sys/dev/atkbdc/atkbd.c` (Copyright (c) 1999 Kazutaka YOKOTA), the keyboard
part of `inc/abi.h` is `sys/sys/kbio.h` under `FNU_` names, and
`user/kbdcontrol.c` is `usr.sbin/kbdcontrol`
(Copyright (c) 1994-1995 Søren Schmidt); `share/vt/keymaps/` are FreeBSD's
`share/vt/keymaps` as they stand. All two-clause BSD, the copyright lines
as the originals give them.

## teken, the terminal emulator from FreeBSD

`kern/teken.c`, `teken.h`, `teken_subr.h`, `teken_subr_compat.h`,
`teken_scs.h`, `teken_wcwidth.h`, `teken_sequences` and `teken_gensequences`
are FreeBSD's `sys/teken` (Copyright (c) 2008-2009 Ed Schouten; BSD-2-Clause),
as they stand but for three things: the include block of `teken.c` names this
kernel's headers and turns the assertions off, `teken.h` takes its types from
`inc/types.h`, and the two calls the kernel has no library for (`snprintf` for
the cursor-position reply, `strcpy` for the status replies) are small
functions of its own. `teken_state.h` is generated from `teken_sequences` by
`teken_gensequences` during the build. `kern/console.c` is the terminal
class around it, after `sys/dev/vt`.

## The 16550 serial driver from OpenBSD

`kern/uart.c` is written after OpenBSD's `sys/dev/ic/com.c` (probe, chip
detection, FIFO measurement, divisor and register handling), and
`kern/comreg.h` carries the register definitions of OpenBSD's `comreg.h`
and `ns16550reg.h` (Copyright (c) 1997-1998 Jason Downs; Copyright (c) 1991
The Regents of the University of California; two- and three-clause BSD), with
their notices and only the chips this driver knows.

## The HID report descriptor parser from OpenBSD

`kern/usb/hid.c` and `kern/usb/hid.h` are written after OpenBSD's
`sys/dev/hid/hid.c` and `hid.h` (Copyright (c) 1998 The NetBSD Foundation,
contributed by Lennart Augustsson; BSD-2-Clause): the same item walk, global
stack and usage lists, in our own code and types. `kern/usb/ums.c` and `kern/usb/ukbd.c` read their
descriptors the way `sys/dev/hid/hidms.c` and `hidkbd.c` do.

## USB device quirks from FreeBSD

The table in `kern/usb/usb_quirk.c` is the mass storage part of FreeBSD's
`sys/dev/usb/quirk/usb_quirk.c` (Copyright (c) 1998 The NetBSD Foundation,
Copyright (c) 1998 Lennart Augustsson, Copyright (c) 2008 Hans Petter
Selasky; BSD-2-Clause), the vendor and product numbers resolved from
`sys/dev/usb/usbdevs`. The `hw.usb.quirk.N` tunable takes FreeBSD's syntax.

## FreeBSD psm(4) and atkbdc(4)

`kern/psm.c`, `kern/atkbdc.c` and `kern/atkbdcreg.h` are ports of FreeBSD's
`sys/dev/atkbdc/psm.c` (Copyright (c) 1992, 1993 Erik Forsberg; 1996, 1997
Kazutaka YOKOTA), `atkbdc.c` and `atkbdcreg.h` (Copyright (c) 1996-1999
Kazutaka YOKOTA), under the BSD licences the originals carry (two-clause
for psm.c, three-clause for atkbdc.c),
reshaped to this kernel's device model and event interface.

## ACPI and the AML interpreter, after OpenBSD

`kern/acpi/` is written after OpenBSD's `sys/dev/acpi`: the interpreter
(`aml_*.c`) follows the design of `dsdt.c`, and the drivers
(`ec.c`, `button.c`, `ac.c`, `battery.c`, `thermal.c`, `prt.c`) follow
`acpiec.c`, `acpibtn.c`, `acpiac.c`, `acpibat.c`, `acpitz.c` and
`acpiprt.c`, and `user/powerd.c` behaves as FreeBSD's `usr.sbin/powerd`
(BSD-2-Clause, Colin Percival and Nate Lawson). The code is our own and does not copy those files. The originals are ISC-licensed:
Copyright (c) 2005 Jordan Hargrave (`dsdt.c`); 2005 Marco Peereboom
(`acpibat.c`, `acpiac.c`, `acpibtn.c`, `acpitz.c`); 2006 Can Erkin Acar
(`acpiec.c`, `acpitz.c`); 2006 Mark Kettenis (`acpiprt.c`). The FreeBSD
`sysctl hw.acpi.acline` and `hw.acpi.battery.*` names are theirs too.

## The scheduler, after DragonFly

`kern/sched.c` is written after DragonFly's `sys/kern/usched_dfly.c` and the
estcpu, pctcpu and load parts of `sys/kern/kern_synch.c`: per-cpu run queues,
the priority from nice and estcpu, the load of a cpu and the weights that
choose one, the idle pull, the rover and the round-robin interval
(BSD-3-Clause; Copyright (c) 1999 Peter Wemm, Copyright (c) 2012-2020 The
DragonFly Project, Matthew Dillon, Mihai Carabas). It does not copy the
file.

The `estcpu` decay follows 4.4BSD's `decay_cpu`. No code is copied.

## Threads after OpenBSD

The process and thread split in `kern/proc.c` (`struct process` and the
threads under it, `thread_fork`, `single_thread`, `thread_exit`, a stop of
the whole process) is written after OpenBSD's `sys/kern/kern_fork.c`,
`kern_exit.c` and `kern_sig.c` (BSD-3-Clause, the Regents of the University
of California). `kern/futex.c` is written after OpenBSD's
`sys/kern/sys_futex.c` (ISC, Copyright (c) 2016-2017 Martin Pieuchot). The code is our own.

## Terminals, event queues, pledge and unveil, after OpenBSD

`kern/tty.c` is written after OpenBSD's `sys/kern/tty.c` (the queues,
`ttyinput`, `ttread`, `ttwrite`, the status line), `kern/event.c` after
`sys/kern/kern_event.c`, `kern/pledge.c` after `sys/kern/kern_pledge.c` and
`kern/unveil.c` after `sys/kern/kern_unveil.c`. The originals are
BSD-3-Clause, the Regents (`tty.c`); BSD-2-Clause, Jonathan Lemon
(`kern_event.c`); ISC, Nicholas Marriott and Theo de Raadt
(`kern_pledge.c`) and Bob Beck (`kern_unveil.c`). The
console buffer `dmesg -s` shows follows OpenBSD's `consbuf`. The code is our own.

## zlib

`third_party/zlib/` is zlib 1.3.2, unchanged (the zlib license, Copyright
(C) 1995-2026 Jean-loup Gailly and Mark Adler; `LICENSE` stays beside it).
It is built as `libz` for compress(1)'s gzip and zip formats.

## LZ4, for the hibernation image

`third_party/lz4/` is `lib/lz4.c` and `lib/lz4.h` of LZ4 1.10.0, unchanged
(BSD 2-Clause, Copyright (c) 2011-2020, Yann Collet; `LICENSE` stays beside
them). The kernel builds it freestanding (`LZ4_FREESTANDING`) and calls
`LZ4_compress_fast_extState` and `LZ4_decompress_safe` only, each page on its
own as Linux's zram keeps them. `kern/hibernate.c` is written after OpenBSD's
`sys/kern/subr_hibernate.c` (piglet, chunk table, free page runs) without
copying it; the original is ISC-licensed, Copyright
(c) 2011 Ariane van der Steldt and Mike Larkin. The order of steps follows Linux's swsusp. No code from it is used.

## Intel shared e1000 code

`third_party/e1000/` is Intel's shared code for the PRO/1000 family, byte
for byte as FreeBSD `sys/dev/e1000` carries it (BSD-3-Clause, Copyright
(c) 2001-2020 Intel Corporation; each file keeps its notice). Only
`e1000_osdep.c` and `e1000_osdep.h` were rewritten for this kernel, in
`kern/net/`.

## Boot-time code from coreboot and NetBSD

`boot/coreboot.c` reads the coreboot table under coreboot's BSD-3-Clause
notice (Advanced Micro Devices, coresystems GmbH); `boot/realprot.S` is
the real-to-protected mode switch from NetBSD's boot blocks, BSD-2-Clause.
Both keep their notices.
