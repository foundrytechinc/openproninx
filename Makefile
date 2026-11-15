# PRONINX, for bmake

OBJDIR?=obj
.OBJDIR: ${.CURDIR}

# sys.mk sets these: never ?= or +=
CC=clang
LD=ld.lld
AR=llvm-ar
OBJCOPY=llvm-objcopy
LDPE=lld-link
OBJDUMP=llvm-objdump
QEMU?=qemu-system-x86_64
LLDB?=lldb
QEMU_VIDEO_OPTS?=-device virtio-vga -display gtk,zoom-to-fit=off
# bmake VBE_WIDTH=1600 VBE_HEIGHT=900
VBE_FALLBACK_WIDTH?=1024
VBE_FALLBACK_HEIGHT?=768
VBE_FORCE=
.if defined(VBE_WIDTH) && defined(VBE_HEIGHT)
VBE_FORCE=-DVBE_WIDTH=${VBE_WIDTH} -DVBE_HEIGHT=${VBE_HEIGHT}
.endif
CPUS?=2
QEMU_SMP_OPTS?=-smp cpus=${CPUS},sockets=${CPUS}
QEMU_NET_OPTS?=-netdev user,id=proninx-net0 -device e1000,netdev=proninx-net0


# no host macros
CFLAGS=-target x86_64-unknown-none-elf
CFLAGS+=-fno-pic -static -fno-builtin -fno-strict-aliasing -MD -g
CFLAGS+=-fno-asynchronous-unwind-tables -fno-unwind-tables
CFLAGS+=-fno-omit-frame-pointer -O1 -std=c11 -Wall -Wextra
CFLAGS+=-Wno-unknown-warning-option -Werror
CFLAGS+=-fno-stack-protector -fno-pie
NOFPU_CFLAGS=-mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mfpmath=387
BOOT_CFLAGS=${CFLAGS} ${NOFPU_CFLAGS} -m32 -ffreestanding -nostdinc -I. -Iboot -DVBE_FALLBACK_WIDTH=${VBE_FALLBACK_WIDTH} -DVBE_FALLBACK_HEIGHT=${VBE_FALLBACK_HEIGHT} ${VBE_FORCE}
KERN_CFLAGS=${CFLAGS} ${NOFPU_CFLAGS} -mno-red-zone -m64 -mcmodel=kernel -nostdinc -I. -D_KERNEL -Wframe-larger-than=2047
LIB_CFLAGS=${CFLAGS} ${NOFPU_CFLAGS} -m64 -nostdinc -I.
USER_CFLAGS=${CFLAGS} -m64 -nostdinc -Iinclude -I.
LIBC_CFLAGS=-Ilib/libc/include -Ilib/libc/gdtoa -Ilib/libc/arch \
	-include lib/libc/include/namespace.h -DFLOATING_POINT
DTOA_CFLAGS=-Ilib/libc/gdtoa -DINFNAN_CHECK -DNO_FENV_H -DUSE_LOCALE
BOOT_LDFLAGS=-m elf_i386
# PE32+
UEFI_CFLAGS=-target x86_64-unknown-uefi -ffreestanding -fno-builtin
UEFI_CFLAGS+=-fno-strict-aliasing -fno-stack-protector -fno-omit-frame-pointer
UEFI_CFLAGS+=-mno-red-zone -mno-mmx -mno-sse -O1 -std=c11 -Wall -Wextra
UEFI_CFLAGS+=-Wno-unknown-warning-option -Werror
UEFI_CFLAGS+=-MD -nostdinc -I. -DVBE_FALLBACK_WIDTH=${VBE_FALLBACK_WIDTH} -DVBE_FALLBACK_HEIGHT=${VBE_FALLBACK_HEIGHT} ${VBE_FORCE}
UEFI_LDFLAGS=-subsystem:efi_application -entry:efi_main -nodefaultlib
KERN_LDFLAGS=-m elf_x86_64
ASM_CFLAGS=-DLOCORE
USER_LDFLAGS=-T user/user.ld

MAKEFILE_DEP=${.CURDIR}/Makefile

MBR_ELF=${OBJDIR}/boot/mbr.elf
MBR_BIN=${OBJDIR}/boot/mbr.bin
STAGE2_ELF=${OBJDIR}/boot/stage2.elf
STAGE2_BIN=${OBJDIR}/boot/stage2.bin
KERNEL=${OBJDIR}/kern/kernel
KERNEL_BOOT=${OBJDIR}/kern/kernel.boot
LIBRARY=${OBJDIR}/lib/libPRONINX_x86_64.a
ENTRYOTHER=${OBJDIR}/kern/entryother
REALMODE=${OBJDIR}/kern/rmtramp
WAKECODE=${OBJDIR}/kern/wakecode
BLOBS=${OBJDIR}/kern/blobs.o
BOOTX64=${OBJDIR}/boot/uefi/BOOTX64.EFI
MKIMG=${OBJDIR}/tools/mkimg
FONTCVT=${OBJDIR}/tools/fontcvt
PRONINX_IMG=${OBJDIR}/PRONINX.img
FS_IMG=${OBJDIR}/fs.img

MBR_ASM=boot/stage_1.S
STAGE2_ASM=boot/entry.S boot/a20.S boot/realprot.S boot/bios.S
# both loaders
BOOTCOMMON_SRCS=boot/gpt.c boot/ufs2read.c boot/rootconf.c boot/autoboot.c
STAGE2_SRCS=boot/main.c boot/disk.c boot/fat.c boot/video.c boot/coreboot.c boot/usbhand.c ${BOOTCOMMON_SRCS}
UEFI_SRCS=boot/uefi/main.c boot/uefi/file.c boot/uefi/video.c boot/uefi/mem.c boot/uefi/pci.c
UEFI_ASM_SRCS=boot/uefi/tramp.S
KERN_SRCS!=cd ${.CURDIR} && find kern -type f -name '*.c' -print
KERN_ASM_SRCS=kern/entry.S kern/swtch.S kern/trapasm.S kern/vectors.S \
	kern/sigtramp.S kern/msrtry.S kern/copy.S kern/acpi/wakeup.S kern/setjmp.S
LWIP_SRCS=third_party/lwip/src/core/def.c third_party/lwip/src/core/dns.c third_party/lwip/src/core/init.c third_party/lwip/src/core/inet_chksum.c third_party/lwip/src/core/ip.c third_party/lwip/src/core/mem.c third_party/lwip/src/core/memp.c third_party/lwip/src/core/netif.c third_party/lwip/src/core/pbuf.c third_party/lwip/src/core/raw.c third_party/lwip/src/core/stats.c third_party/lwip/src/core/sys.c third_party/lwip/src/core/tcp.c third_party/lwip/src/core/tcp_in.c third_party/lwip/src/core/tcp_out.c third_party/lwip/src/core/timeouts.c third_party/lwip/src/core/udp.c third_party/lwip/src/core/ipv4/dhcp.c third_party/lwip/src/core/ipv4/etharp.c third_party/lwip/src/core/ipv4/icmp.c third_party/lwip/src/core/ipv4/igmp.c third_party/lwip/src/core/ipv4/ip4.c third_party/lwip/src/core/ipv4/ip4_addr.c third_party/lwip/src/core/ipv4/ip4_frag.c third_party/lwip/src/netif/ethernet.c
# zlib, Z_SOLO
ZLIB_SRCS=third_party/zlib/adler32.c third_party/zlib/deflate.c third_party/zlib/inffast.c third_party/zlib/inflate.c third_party/zlib/inftrees.c third_party/zlib/trees.c third_party/zlib/zutil.c
ZLIB_CFLAGS=-DZ_SOLO -DNO_GZIP -Ithird_party/zlib
# LZ4 for the hibernation image. the rest of it keeps 16 KB on the stack, whatever
LZ4_SRCS=third_party/lz4/lz4.c
LZ4_FRAMES=-Wno-frame-larger-than
CC_INCLUDE!=${CC} -print-resource-dir
LZ4_CFLAGS=-ffreestanding -isystem ${CC_INCLUDE}/include -DLZ4_FREESTANDING=1 -DLZ4_memcpy=__builtin_memcpy -DLZ4_memmove=__builtin_memmove -DLZ4_memset=__builtin_memset -Ithird_party/lz4
X86EMU_SRCS=third_party/x86emu/x86emu.c
X86EMU_CFLAGS=-Ithird_party/x86emu/port -Ithird_party/x86emu
# Intel shared code
E1000_SRCS!=cd ${.CURDIR} && find third_party/e1000 -type f -name '*.c' -print
LIB_SRCS=lib/string.c lib/ffs.c
USER_CRT_SRC=lib/csu/crt0.S
USER_CRT=${OBJDIR}/lib/csu/crt0.o
# no POSIX module
FNU_CRT_SRC=lib/csu/fnucrt.S
FNU_CRT=${OBJDIR}/lib/csu/fnucrt.o
FNU_ONLY=tests/fnuonly.c tests/fnuthreads.c
# LIBRARIES
LIBS=fnu c util z crypt ssh m
LIB_fnu_SRCS=lib/libfnu/usys.S lib/libfnu/table.c lib/libfnu/net.c lib/libfnu/errstr.c \
	lib/libfnu/resolve.c
# included by regexec.c
LIB_c_SRCS!=cd ${.CURDIR} && find lib/libc -type f \( -name '*.c' -o -name '*.S' \) ! -name engine.c -print
LIB_c_SRCS+=${LIB_SRCS}
LIB_c_INCS=${LIBC_CFLAGS}
# libutil
LIB_util_SRCS!=cd ${.CURDIR} && find lib/libutil -type f -name '*.c' -print
LIB_util_INCS=-Ilib/libc/gen
LIB_z_SRCS=${ZLIB_SRCS}
.for f in compress.c crc32.c gzclose.c gzlib.c gzread.c gzwrite.c infback.c uncompr.c
LIB_z_SRCS+=third_party/zlib/${f}
.endfor
LIB_z_INCS=-Ithird_party/zlib -DHAVE_UNISTD_H -DHAVE_STDARG_H
# libm
LIBM_COMMON=b_exp__D.c b_log__D.c b_tgamma.c \
	e_acos.c e_acosf.c e_acosh.c e_acoshf.c e_asin.c e_asinf.c \
	e_atan2.c e_atan2f.c e_atanh.c e_atanhf.c e_cosh.c e_coshf.c e_exp.c \
	e_expf.c e_fmod.c e_fmodf.c e_hypot.c e_hypotf.c e_j0.c e_j0f.c \
	e_j1.c e_j1f.c e_jn.c e_jnf.c e_lgamma_r.c e_lgammaf_r.c e_log.c \
	e_log10.c e_log10f.c e_log2.c e_log2f.c e_logf.c e_pow.c e_powf.c \
	e_rem_pio2.c \
	e_rem_pio2f.c e_remainder.c e_remainderf.c e_scalb.c e_scalbf.c \
	e_sinh.c e_sinhf.c e_sqrt.c e_sqrtf.c \
	fenv.c \
	k_cos.c k_cosf.c k_rem_pio2.c k_rem_pio2f.c k_sin.c k_sinf.c \
	k_tan.c k_tanf.c \
	s_lround.c s_lroundf.c s_llround.c s_llroundf.c \
	s_asinh.c s_asinhf.c s_atan.c s_atanf.c s_cabsf.c \
	s_cacosf.c s_cacoshf.c s_cargf.c \
	s_casinf.c s_casinhf.c s_catanf.c \
	s_catanhf.c s_cbrt.c s_cbrtf.c s_ccosf.c \
	s_ccoshf.c s_ceil.c \
	s_ceilf.c s_cexpf.c s_clogf.c \
	s_copysign.c s_copysignf.c s_cos.c s_cosf.c s_cpowf.c \
	s_csinf.c \
	s_csinhf.c s_csqrtf.c s_ctanf.c \
	s_ctanhf.c s_erf.c s_erff.c s_exp2.c s_exp2f.c s_expm1.c s_expm1f.c \
	s_fabsf.c s_fma.c s_fmaf.c \
	s_floor.c s_floorf.c s_frexpf.c s_ilogb.c s_ilogbf.c \
	s_log1p.c \
	s_log1pf.c s_logb.c s_logbf.c s_llrint.c s_llrintf.c s_lrint.c \
	s_lrintf.c s_modff.c s_nan.c s_nearbyint.c s_nextafter.c \
	s_nextafterf.c s_nexttowardf.c s_remquo.c s_remquof.c s_rint.c \
	s_rintf.c \
	s_scalbn.c s_scalbnf.c s_significand.c \
	s_significandf.c s_sin.c s_sinf.c s_sincos.c s_sincosf.c \
	s_tan.c s_tanf.c s_tanh.c s_tanhf.c s_trunc.c \
	s_truncf.c w_drem.c w_dremf.c w_gamma.c w_gamma_r.c w_gammaf.c \
	w_gammaf_r.c w_lgamma.c w_lgammaf.c
LIBM_LONG=e_acoshl.c e_acosl.c e_asinl.c e_atan2l.c e_atanhl.c \
	e_coshl.c s_cabsl.c s_cacoshl.c s_cacosl.c s_cargl.c \
	s_casinhl.c s_casinl.c s_catanhl.c s_catanl.c s_ccoshl.c \
	s_ccosl.c s_cexpl.c s_cimagl.c s_clogl.c s_conjl.c s_cpowl.c \
	s_cprojl.c s_creall.c s_csinhl.c s_csinl.c s_csqrtl.c \
	s_ctanhl.c s_ctanl.c e_expl.c e_fmodl.c e_hypotl.c e_lgammal.c \
	e_log10l.c e_log2l.c e_logl.c e_powl.c e_remainderl.c \
	e_sinhl.c e_sqrtl.c e_tgammal.c invtrig.c k_cosl.c k_sinl.c \
	k_tanl.c polevll.c s_asinhl.c s_atanl.c s_cbrtl.c s_ceill.c \
	s_copysignl.c s_cosl.c s_erfl.c s_exp2l.c s_expm1l.c s_fabsl.c \
	s_floorl.c s_fmal.c s_fmaxl.c s_fminl.c s_frexpl.c s_ilogbl.c \
	s_llrintl.c s_llroundl.c s_log1pl.c s_logbl.c s_lrintl.c \
	s_lroundl.c s_modfl.c s_nanl.c s_nextafterl.c s_nexttoward.c \
	s_remquol.c s_rintl.c s_roundl.c s_scalbnl.c s_sinl.c s_sincosl.c \
	s_tanhl.c s_tanl.c s_truncl.c
LIBM_PURE=s_cabs.c s_cacos.c s_cacosh.c s_carg.c s_casin.c \
	s_casinh.c s_catan.c s_catanh.c s_ccos.c s_ccosh.c s_cexp.c \
	s_cimag.c s_cimagf.c s_clog.c s_conj.c s_conjf.c s_cpow.c \
	s_creal.c s_crealf.c s_csin.c s_csinh.c s_csqrt.c s_ctan.c \
	s_ctanh.c s_fdim.c s_fmax.c s_fmaxf.c s_fmin.c s_fminf.c \
	s_cproj.c s_cprojf.c s_round.c s_roundf.c s_scalbln.c s_signgam.c 
LIB_m_SRCS!=cd ${.CURDIR}/lib/libm && for f in ${LIBM_COMMON} ${LIBM_PURE} ${LIBM_LONG}; do \
	s=$${f%.c}.S; \
	if [ -e arch/amd64/$$s ]; then echo lib/libm/arch/amd64/$$s; \
	elif [ -e arch/amd64/$$f ]; then echo lib/libm/arch/amd64/$$f; \
	elif [ -e src/$$f ]; then echo lib/libm/src/$$f; \
	else echo lib/libm/src/ld80/$$f; fi; done | sort -u
LIB_m_INCS=-Ilib/libm/hidden -include lib/libm/hidden/namespace.h -Ilib/libm/src \
	-Ilib/libm/src/ld80 -Ilib/libm/arch/amd64
LIB_m_CFLAGS=-std=gnu11
LIB_crypt_SRCS!=cd ${.CURDIR} && find lib/libcrypt -type f \( -name '*.c' -o -name '*.S' \) ! -name helper.c -print
LIB_crypt_INCS=-Ilib/libcrypt/hidden -Ilib/libcrypt -Ilib/libcrypt/arch/amd64 \
	-Ilib/libc/arch/amd64 -include lib/libc/include/namespace.h
# libssh
LIB_ssh_SRCS=user/ssh/chacha.c user/ssh/chacha_x4.c user/ssh/poly1305.c user/ssh/ed25519.c \
	user/ssh/smult_curve25519_ref.c user/ssh/keys.c user/ssh/transport.c
LIB_ssh_INCS=-Iuser/ssh
.for l in ${LIBS}
LIB_${l}=${OBJDIR}/lib/lib${l}.a
LIB_${l}_OBJS=
.for src in ${LIB_${l}_SRCS}
LIB_${l}_OBJS+=${OBJDIR}/lib${l}/${src:R}.o
.endfor
.endfor
HASH_HL=sha224 sha256 sha384 sha512 sha512_256
.for h in sha224 sha256 sha384 sha512
HASH_HL_SED_${h}=-e 's/hashinc/sha2.h/g' -e 's/HASH/${h:tu}/g' \
	-e 's/SHA[0-9][0-9][0-9]_CTX/SHA2_CTX/g'
.endfor
HASH_HL_SED_sha512_256=-e 's/hashinc/sha2.h/g' -e 's/HASH/SHA512_256/g' \
	-e 's/SHA512_256_CTX/SHA2_CTX/g'
.for h in ${HASH_HL}
LIB_crypt_OBJS+=${OBJDIR}/libcrypt/${h}hl.o
.endfor
LIBFNU=${LIB_fnu}
LIBC=${LIB_c}
USER_SRCS!=cd ${.CURDIR} && find user -maxdepth 1 -type f -name '*.c' -print
# tests
TEST_SRCS!=cd ${.CURDIR} && find tests -maxdepth 1 -type f -name '*.c' ! -name fnuonly.c ! -name fnuthreads.c ! -name sshtest.c -print
MBR_OBJS=
STAGE2_OBJS=
UEFI_OBJS=
KERN_OBJS=
LWIP_OBJS=
E1000_OBJS=
LZ4_OBJS=
X86EMU_OBJS=
LIB_OBJS=
USER_BINS=
.for src in ${MBR_ASM}
MBR_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${STAGE2_ASM} ${STAGE2_SRCS}
STAGE2_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${UEFI_SRCS} ${UEFI_ASM_SRCS}
UEFI_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${BOOTCOMMON_SRCS}
UEFI_OBJS+=${OBJDIR}/efi/${src:R}.o
.endfor
.for src in ${KERN_SRCS} ${KERN_ASM_SRCS}
KERN_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${LWIP_SRCS}
LWIP_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${LZ4_SRCS}
LZ4_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${X86EMU_SRCS}
X86EMU_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${E1000_SRCS}
E1000_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${LIB_SRCS}
LIB_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${USER_SRCS} ${TEST_SRCS}
USER_BINS+=${OBJDIR}/${src:R}
.endfor
# sh
SH_DIR=user/sh
SH_OBJ=${OBJDIR}/user/sh.d
SH_SRCS=alias.c arith_yacc.c arith_yylex.c cd.c bltin/echo.c error.c eval.c \
	histedit.c \
	exec.c expand.c input.c jobs.c kill.c mail.c main.c memalloc.c \
	miscbltin.c mystring.c options.c output.c parser.c printf.c redir.c \
	show.c test.c trap.c var.c
SH_GEN=builtins.c nodes.c syntax.c
SH_CFLAGS=-DSHELL -DNO_HISTORY -I${SH_OBJ} -I${SH_DIR}  
# host build tools
HOST_BSD=-D'__FBSDID(s)=' -D'__unused=__attribute__((__unused__))' -D'__dead2=__attribute__((__noreturn__))' -D'__printf0like(f,v)=__attribute__((__format__(__printf0__,f,v)))'
SH_OBJS=
.for src in ${SH_SRCS}
SH_OBJS+=${SH_OBJ}/${src:R}.o
.endfor
.for src in ${SH_GEN}
SH_OBJS+=${SH_OBJ}/${src:R}.o
.endfor
USER_BINS+=${OBJDIR}/user/sh
.for src in ${FNU_ONLY}
USER_BINS+=${OBJDIR}/${src:R}
.endfor
# MULTI-FILE PROGRAMS
PROGS=tr pax dd hexdump compress vi cp mv cmp xargs ls find grep sed diff awk install unifdef pr vis lam rs banner ed patch \
	ssh sshd ssh-keygen sshtest
PROG_tr_DIR=user/tr
PROG_tr_SRCS=tr.c str.c
PROG_pax_DIR=user/pax
PROG_pax_SRCS=ar_io.c ar_subs.c buf_subs.c cpio.c file_subs.c ftree.c \
	gen_subs.c getoldopt.c options.c pat_rep.c pax.c sel_subs.c tables.c \
	tar.c tty_subs.c
PROG_cp_DIR=user/cp
PROG_cp_SRCS=cp.c utils.c
PROG_mv_DIR=user/mv
PROG_mv_SRCS=mv.c cp.c rm.c
PROG_cmp_DIR=user/cmp
PROG_cmp_SRCS=cmp.c misc.c regular.c special.c
PROG_xargs_DIR=user/xargs
PROG_xargs_SRCS=xargs.c strnsubst.c
PROG_ls_DIR=user/ls
PROG_ls_SRCS=cmp.c ls.c main.c print.c util.c utf8.c
PROG_ls_LIBS=util
PROG_find_DIR=user/find
PROG_find_SRCS=find.c function.c ls.c main.c misc.c operator.c option.c
PROG_grep_DIR=user/grep
PROG_grep_SRCS=binary.c file.c grep.c mmfile.c queue.c util.c
PROG_grep_CFLAGS=-Ithird_party/zlib
PROG_grep_LIBS=z
PROG_sed_DIR=user/sed
PROG_sed_SRCS=compile.c main.c misc.c process.c
PROG_diff_DIR=user/diff
PROG_diff_SRCS=diff.c diffdir.c diffreg.c xmalloc.c
.for p in install unifdef
PROG_${p}_DIR=user/${p}
.endfor
PROG_install_SRCS=xinstall.c
PROG_unifdef_SRCS=unifdef.c
.for p in pr vis lam rs banner ed patch
PROG_${p}_DIR=user/${p}
.endfor
PROG_pr_SRCS=pr.c egetopt.c
PROG_vis_SRCS=vis.c foldit.c
PROG_lam_SRCS=lam.c utf8.c
PROG_rs_SRCS=rs.c utf8.c
PROG_banner_SRCS=banner.c chset.c
PROG_ed_SRCS=buf.c glbl.c io.c main.c re.c sub.c undo.c
PROG_patch_SRCS=patch.c pch.c inp.c util.c backupfile.c mkpath.c ed.c
# awk
PROG_awk_DIR=user/awk
PROG_awk_SRCS=lex.c b.c main.c parse.c tran.c lib.c run.c
PROG_awk_GEN=awkgram.tab.c proctab.c
PROG_awk_GENHDRS=${OBJDIR}/user/awk/awk.d/awkgram.tab.h
PROG_awk_CFLAGS=-I${OBJDIR}/user/awk/awk.d -DHAS_ISBLANK -DNDEBUG
PROG_awk_LIBS=m
PROG_dd_DIR=user/dd
PROG_dd_SRCS=args.c conv.c conv_tab.c dd.c misc.c position.c
PROG_dd_LIBS=util
PROG_hexdump_DIR=user/hexdump
PROG_hexdump_SRCS=conv.c display.c hexdump.c hexsyntax.c odsyntax.c parse.c
PROG_compress_DIR=user/compress
PROG_compress_SRCS=main.c zopen.c gzopen.c zipopen.c nullopen.c
PROG_compress_CFLAGS=-Ithird_party/zlib
PROG_compress_LIBS=z
# vi
PROG_vi_DIR=user/vi
PROG_vi_SRCS=vi.c ex.c lbuf.c mot.c sbuf.c ren.c dir.c syn.c reg.c led.c uc.c \
	term.c rset.c rstr.c regex.c cmd.c tag.c conf.c lsp.c json.c sys.c
# ssh
.for p in ssh sshd ssh-keygen sshtest
PROG_${p}_DIR=user/ssh
PROG_${p}_SRCS=${p}.c
PROG_${p}_LIBS=ssh util crypt
.endfor
PROG_sshtest_DIR=tests
PROG_sshtest_CFLAGS=-Iuser/ssh
.for p in ${PROGS}
PROG_${p}=${OBJDIR}/${PROG_${p}_DIR}/${p}
PROG_${p}_GENDIR=${OBJDIR}/${PROG_${p}_DIR}/${p}.d
PROG_${p}_OBJS=
.for src in ${PROG_${p}_SRCS} ${PROG_${p}_GEN}
PROG_${p}_OBJS+=${PROG_${p}_GENDIR}/${src:R}.o
.endfor
USER_BINS+=${PROG_${p}}
.endfor
.for p in basename chmod date df dirname du fnufetch fnuplug fnusvc groupadd ln powerd \
	ptytest rm syslogd userdel usermod wc
LIBS.${p}=util
.endfor
.for p in login passwd su doas useradd init chpass
LIBS.${p}=util crypt
.endfor
LIBS.moused=m util
LIBS.cksum=crypt
LIBS.ping=m
LIBS.jot=m
LIBS.seq=m
LIBS.faulttest=m
LIBS.mathtest=m
IMAGES=${PRONINX_IMG} ${FS_IMG}

.SUFFIXES:
.PHONY: default all clean format font ci smoke qemu qemu-uefi qemu-debug lldb
.MAIN: default
default: ${IMAGES}
all: default

${MBR_ELF}: ${MBR_OBJS}
	@mkdir -p ${.TARGET:H}
	${LD} -m elf_i386 --image-base=0 -N -Ttext 0x600 -e start -o ${.TARGET} ${MBR_OBJS}

${MBR_BIN}: ${MBR_ELF}
	${OBJCOPY} -O binary -j .stage_1 ${.ALLSRC} ${.TARGET}

${STAGE2_ELF}: ${STAGE2_OBJS} boot/stage2.ld
	@mkdir -p ${.TARGET:H}
	${LD} -m elf_i386 --image-base=0 -N -T boot/stage2.ld -o ${.TARGET} ${STAGE2_OBJS}

${STAGE2_BIN}: ${STAGE2_ELF}
	${OBJCOPY} -O binary ${.ALLSRC} ${.TARGET}

${BOOTX64}: ${UEFI_OBJS}
	@mkdir -p ${.TARGET:H}
	${LDPE} ${UEFI_LDFLAGS} -out:${.TARGET} ${UEFI_OBJS}

.for src in ${UEFI_SRCS} ${UEFI_ASM_SRCS}
${OBJDIR}/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${UEFI_CFLAGS} ${src:M*.S:C/.+/${ASM_CFLAGS}/} -c -o ${.TARGET} ${src}
.endfor
.for src in ${BOOTCOMMON_SRCS}
${OBJDIR}/efi/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${UEFI_CFLAGS} -c -o ${.TARGET} ${src}
.endfor

.for src in ${MBR_ASM} ${STAGE2_ASM} ${STAGE2_SRCS}
${OBJDIR}/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${BOOT_CFLAGS} ${src:M*.S:C/.+/${ASM_CFLAGS}/} -c -o ${.TARGET} ${src}
.endfor

${LIBRARY}: ${LIB_OBJS}
	@mkdir -p ${.TARGET:H}
	@rm -f ${.TARGET}
	${AR} rcs ${.TARGET} ${.ALLSRC}

.for h in ${HASH_HL}
${OBJDIR}/libcrypt/${h}hl.c: lib/libcrypt/helper.c ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	sed ${HASH_HL_SED_${h}} lib/libcrypt/helper.c > ${.TARGET}
${OBJDIR}/libcrypt/${h}hl.o: ${OBJDIR}/libcrypt/${h}hl.c
	${CC} ${LIB_crypt_INCS} ${USER_CFLAGS} ${LIB_crypt_CFLAGS} -c -o ${.TARGET} ${OBJDIR}/libcrypt/${h}hl.c
.endfor

.for l in ${LIBS}
${LIB_${l}}: ${LIB_${l}_OBJS}
	@mkdir -p ${.TARGET:H}
	@rm -f ${.TARGET}
	${AR} rcs ${.TARGET} ${.ALLSRC}
.for src in ${LIB_${l}_SRCS}
${OBJDIR}/lib${l}/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${LIB_${l}_INCS} ${USER_CFLAGS} ${LIB_${l}_CFLAGS} ${src:M*.S:C/.+/${ASM_CFLAGS}/} ${src:Mlib/libc/gdtoa/*:C/.+/${DTOA_CFLAGS}/} -c -o ${.TARGET} ${src}
.endfor
.endfor

${FNU_CRT}: ${FNU_CRT_SRC} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${USER_CFLAGS} ${ASM_CFLAGS} -c -o ${.TARGET} ${FNU_CRT_SRC}

.for src in ${FNU_ONLY}
${OBJDIR}/${src:R}: ${src} ${FNU_CRT} ${LIBFNU} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${CFLAGS} -m64 -nostdinc -I. -c -o ${.TARGET}.o ${src}
	${LD} ${USER_LDFLAGS} -o ${.TARGET} ${FNU_CRT} ${.TARGET}.o -L${OBJDIR}/lib -lfnu
	${OBJDUMP} -S ${.TARGET} > ${.TARGET}.asm
.endfor

${USER_CRT}: ${USER_CRT_SRC} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${USER_CFLAGS} ${ASM_CFLAGS} -c -o ${.TARGET} ${USER_CRT_SRC}
.for src in ${LIB_SRCS}
${OBJDIR}/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${LIB_CFLAGS} -c -o ${.TARGET} ${src}
.endfor

${ENTRYOTHER}: kern/entryother.S ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${CFLAGS} ${ASM_CFLAGS} -m32 -nostdinc -I. -MT ${.TARGET} -c -o ${.TARGET}.o kern/entryother.S
	${LD} -m elf_i386 --image-base=0 -N -e start -Ttext 0 -o ${.TARGET}.out ${.TARGET}.o
	${OBJCOPY} -S -O binary ${.TARGET}.out ${.TARGET}
	${OBJDUMP} -S ${.TARGET}.o > ${.TARGET}.asm

${REALMODE}: kern/rmtramp.S kern/realmode.h ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${CFLAGS} ${ASM_CFLAGS} -m32 -nostdinc -I. -Ikern -MT ${.TARGET} -c -o ${.TARGET}.o kern/rmtramp.S
	${LD} -m elf_i386 --image-base=0 -N -e realmode_enter -Ttext 0 -o ${.TARGET}.out ${.TARGET}.o
	${OBJCOPY} -S -O binary ${.TARGET}.out ${.TARGET}
	${OBJDUMP} -S ${.TARGET}.o > ${.TARGET}.asm

# from OBJDIR: ld -b binary bakes the path in
${WAKECODE}: kern/acpi/wakecode.S kern/acpi/wake.h ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${CFLAGS} ${ASM_CFLAGS} -m32 -nostdinc -I. -Ikern -Ikern/acpi -MT ${.TARGET} -c -o ${.TARGET}.o kern/acpi/wakecode.S
	${LD} -m elf_i386 --image-base=0 -N -e wakeup_start -Ttext 0 -o ${.TARGET}.out ${.TARGET}.o
	${OBJCOPY} -S -O binary ${.TARGET}.out ${.TARGET}
	${OBJDUMP} -S ${.TARGET}.o > ${.TARGET}.asm

${BLOBS}: ${ENTRYOTHER} ${REALMODE} ${WAKECODE} ${MAKEFILE_DEP}
	cd ${OBJDIR}/kern && ${LD} -r ${KERN_LDFLAGS} -b binary -o blobs.o \
		entryother rmtramp wakecode

${KERNEL}: ${KERN_OBJS} ${LWIP_OBJS} ${E1000_OBJS} ${LZ4_OBJS} ${X86EMU_OBJS} kern/kernel.ld ${BLOBS} ${LIBRARY}
	@mkdir -p ${.TARGET:H}
	${LD} ${KERN_LDFLAGS} -T kern/kernel.ld -o ${.TARGET} ${KERN_OBJS} ${LWIP_OBJS} ${E1000_OBJS} ${LZ4_OBJS} ${X86EMU_OBJS} ${BLOBS} -L${OBJDIR}/lib -lPRONINX_x86_64
	${OBJDUMP} -S ${.TARGET} > ${.TARGET}.asm
.for src in ${KERN_SRCS} ${KERN_ASM_SRCS} ${LWIP_SRCS} ${E1000_SRCS} ${LZ4_SRCS} ${X86EMU_SRCS}
${OBJDIR}/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${KERN_CFLAGS} ${src:M*.S:C/.+/${ASM_CFLAGS}/} ${src:Mthird_party/lz4/*:C/.+/${LZ4_CFLAGS} ${LZ4_FRAMES}/} ${src:Mkern/hibernate.c:C/.+/${LZ4_CFLAGS}/} ${src:Mthird_party/x86emu/*:C/.+/${X86EMU_CFLAGS}/} ${src:Mkern/x86bios.c:C/.+/${X86EMU_CFLAGS}/} -fno-pic -Ikern -I${OBJDIR}/kern -Ikern/net -Ikern/storage -Ikern/usb -Ikern/acpi -Ikern/net/lwip/port/include -Ithird_party/lwip/src/include -Ithird_party/e1000 -c -o ${.TARGET} ${src}
.endfor

# teken state table
${OBJDIR}/kern/teken_state.h: kern/teken_sequences kern/teken_gensequences ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	awk -f kern/teken_gensequences kern/teken_sequences > ${.TARGET}
${OBJDIR}/kern/teken.o: ${OBJDIR}/kern/teken_state.h

kern/vectors.S: kern/vectors.sh
	tr -d '\r' < ${.ALLSRC} | sh > ${.TARGET}

	chmod +x ${.TARGET}

# yacc on the host
BYACC=${OBJDIR}/tools/yacc/yacc
BYACC_SRCS!=cd ${.CURDIR} && find tools/yacc -name '*.c' -print
${BYACC}: ${BYACC_SRCS} tools/yacc/defs.h
	@mkdir -p ${.TARGET:H}
	${CC} -std=gnu11 -Wall -Wextra -Werror -D'pledge(p,e)=0' \
	    -D'__dead=__attribute__((__noreturn__))' -o ${.TARGET} ${BYACC_SRCS}

${MKIMG}: tools/mkimg.c
	@mkdir -p ${.TARGET:H}
	${CC} -std=c11 -Wall -Wextra -Werror -o ${.TARGET} tools/mkimg.c

${FONTCVT}: tools/fontcvt.c
	@mkdir -p ${.TARGET:H}
	${CC} -std=c11 -Wall -Wextra -Werror -o ${.TARGET} tools/fontcvt.c

# kern/font_spleen.c again, from an unpacked Spleen release: bmake font SPLEEN=dir
font: ${FONTCVT}
	@test -n "${SPLEEN}" || { echo "bmake font SPLEEN=<spleen release dir>"; exit 1; }
	{ echo "Spleen `sed -n 's/^FONT_VERSION \"\(.*\)\"/\1/p' ${SPLEEN}/spleen-8x16.bdf`, https://github.com/fcambus/spleen"; \
	  echo; cat ${SPLEEN}/LICENSE; } > ${OBJDIR}/tools/spleen-license
	${FONTCVT} -l ${OBJDIR}/tools/spleen-license \
	    -w 3840 "Spleen 32x64" ${SPLEEN}/spleen-32x64.bdf \
	    -w 1920 "Spleen 16x32" ${SPLEEN}/spleen-16x32.bdf \
	    -w 961 "Spleen 12x24" ${SPLEEN}/spleen-12x24.bdf \
	    -w 0 "Spleen 8x16" ${SPLEEN}/spleen-8x16.bdf > kern/font_spleen.c

.for src in ${USER_SRCS} ${TEST_SRCS}
${OBJDIR}/${src:R}: ${src} ${USER_CRT} ${LIBS.${src:T:R}:@l@${LIB_${l}}@} ${LIBC} ${LIBFNU} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${USER_CFLAGS} -c -o ${.TARGET}.o ${src}
	${LD} ${USER_LDFLAGS} -o ${.TARGET} ${USER_CRT} ${.TARGET}.o -L${OBJDIR}/lib ${LIBS.${src:T:R}:@l@-l${l}@} -lc -lfnu
	${OBJDUMP} -S ${.TARGET} > ${.TARGET}.asm
.endfor

SETUID_BINS=chpass doas passwd ping su
BIN_LINKS=[=test sha224=cksum sha256=cksum sha384=cksum sha512=cksum whereis=which machine=arch uptime=w groups=id whoami=id egrep=grep fgrep=grep zgrep=grep zegrep=grep zfgrep=grep chgrp=chmod chown=chmod chflags=chmod halt=reboot hibernate=zzz adduser=useradd groupdel=groupadd groupmod=groupadd chsh=chpass chfn=chpass man=fnuman poweroff=reboot health=services tar=pax cpio=pax mkfifo=mknod ex=vi od=hexdump uncompress=compress zcat=compress gzip=compress gunzip=compress gzcat=compress
INSTALL_BINS=
.for src in ${USER_SRCS}
INSTALL_BINS+=${OBJDIR}/install/${src:T:R}
.endfor
.for src in ${USER_SRCS}
${OBJDIR}/install/${src:T:R}: ${OBJDIR}/${src:R}
	@mkdir -p ${.TARGET:H}
	${OBJCOPY} --strip-debug ${.ALLSRC} ${.TARGET}
.endfor
INSTALL_BINS+=${OBJDIR}/install/sh
${OBJDIR}/install/sh: ${OBJDIR}/user/sh
	@mkdir -p ${.TARGET:H}
	${OBJCOPY} --strip-debug ${.ALLSRC} ${.TARGET}
.for p in ${PROGS:Nsshtest}
INSTALL_BINS+=${OBJDIR}/install/${p}
${OBJDIR}/install/${p}: ${PROG_${p}}
	@mkdir -p ${.TARGET:H}
	${OBJCOPY} --strip-debug ${.ALLSRC} ${.TARGET}
.endfor
INSTALL_TESTS=
.for src in ${TEST_SRCS} ${FNU_ONLY}
INSTALL_TESTS+=${OBJDIR}/install/tests/${src:T:R}
${OBJDIR}/install/tests/${src:T:R}: ${OBJDIR}/${src:R}
	@mkdir -p ${.TARGET:H}
	${OBJCOPY} --strip-debug ${.ALLSRC} ${.TARGET}
.endfor
INSTALL_TESTS+=${OBJDIR}/install/tests/sshtest
${OBJDIR}/install/tests/sshtest: ${PROG_sshtest}
	@mkdir -p ${.TARGET:H}
	${OBJCOPY} --strip-debug ${.ALLSRC} ${.TARGET}

${SH_OBJ}/mknodes ${SH_OBJ}/mksyntax: ${SH_DIR}/mknodes.c ${SH_DIR}/mksyntax.c ${SH_DIR}/parser.h
	@mkdir -p ${SH_OBJ}
	${CC} -std=c11 -Wall ${HOST_BSD} -I${SH_DIR} -o ${SH_OBJ}/mknodes ${SH_DIR}/mknodes.c
	${CC} -std=c11 -Wall ${HOST_BSD} -I${SH_DIR} -o ${SH_OBJ}/mksyntax ${SH_DIR}/mksyntax.c
${SH_OBJ}/builtins.c ${SH_OBJ}/builtins.h: ${SH_DIR}/mkbuiltins ${SH_DIR}/builtins.def
	@mkdir -p ${SH_OBJ}
	cd ${SH_OBJ} && sh ${.CURDIR}/${SH_DIR}/mkbuiltins ${.CURDIR}/${SH_DIR}
${SH_OBJ}/nodes.c ${SH_OBJ}/nodes.h: ${SH_OBJ}/mknodes ${SH_DIR}/nodetypes ${SH_DIR}/nodes.c.pat
	cd ${SH_OBJ} && ./mknodes ${.CURDIR}/${SH_DIR}/nodetypes ${.CURDIR}/${SH_DIR}/nodes.c.pat
${SH_OBJ}/syntax.c ${SH_OBJ}/syntax.h: ${SH_OBJ}/mksyntax
	cd ${SH_OBJ} && ./mksyntax
${SH_OBJ}/token.h: ${SH_DIR}/mktokens
	@mkdir -p ${SH_OBJ}
	cd ${SH_OBJ} && sh ${.CURDIR}/${SH_DIR}/mktokens
.for src in ${SH_SRCS}
${SH_OBJ}/${src:R}.o: ${SH_DIR}/${src} ${SH_OBJ}/builtins.h ${SH_OBJ}/nodes.h ${SH_OBJ}/syntax.h ${SH_OBJ}/token.h ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${USER_CFLAGS} ${SH_CFLAGS} -c -o ${.TARGET} ${SH_DIR}/${src}
.endfor
.for src in ${SH_GEN}
${SH_OBJ}/${src:R}.o: ${SH_OBJ}/${src} ${SH_OBJ}/builtins.h ${SH_OBJ}/nodes.h ${SH_OBJ}/syntax.h ${SH_OBJ}/token.h ${MAKEFILE_DEP}
	${CC} ${USER_CFLAGS} ${SH_CFLAGS} -c -o ${.TARGET} ${SH_OBJ}/${src}
.endfor
.for p in ${PROGS}
.for src in ${PROG_${p}_SRCS}
${PROG_${p}_GENDIR}/${src:R}.o: ${PROG_${p}_DIR}/${src} ${PROG_${p}_GENHDRS} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${USER_CFLAGS} -I${PROG_${p}_DIR} ${PROG_${p}_CFLAGS} -c -o ${.TARGET} ${PROG_${p}_DIR}/${src}
.endfor
.for src in ${PROG_${p}_GEN}
${PROG_${p}_GENDIR}/${src:R}.o: ${PROG_${p}_GENDIR}/${src} ${PROG_${p}_GENHDRS} ${MAKEFILE_DEP}
	${CC} ${USER_CFLAGS} -I${PROG_${p}_DIR} ${PROG_${p}_CFLAGS} -c -o ${.TARGET} ${PROG_${p}_GENDIR}/${src}
.endfor
${PROG_${p}}: ${PROG_${p}_OBJS} ${USER_CRT} ${PROG_${p}_LIBS:@l@${LIB_${l}}@} ${LIBC} ${LIBFNU}
	@mkdir -p ${.TARGET:H}
	${LD} ${USER_LDFLAGS} -o ${.TARGET} ${USER_CRT} ${PROG_${p}_OBJS} -L${OBJDIR}/lib ${PROG_${p}_LIBS:@l@-l${l}@} -lc -lfnu
	${OBJDUMP} -S ${.TARGET} > ${.TARGET}.asm
.endfor

AWK_GEN=${OBJDIR}/user/awk/awk.d
${AWK_GEN}/awkgram.tab.c ${AWK_GEN}/awkgram.tab.h: user/awk/awkgram.y ${BYACC}
	@mkdir -p ${AWK_GEN}
	${BYACC} -o ${AWK_GEN}/awkgram.tab.c -d user/awk/awkgram.y
${AWK_GEN}/maketab: ${AWK_GEN}/awkgram.tab.h user/awk/maketab.c
	${CC} -std=gnu11 -Wall -Wextra -Werror -I${AWK_GEN} -Iuser/awk \
	    -DHAS_ISBLANK -DNDEBUG -o ${.TARGET} user/awk/maketab.c
${AWK_GEN}/proctab.c: ${AWK_GEN}/maketab
	${AWK_GEN}/maketab ${AWK_GEN}/awkgram.tab.h > ${.TARGET}

${OBJDIR}/user/sh: ${SH_OBJS} ${USER_CRT} ${LIBC} ${LIBFNU}
	${LD} ${USER_LDFLAGS} -o ${.TARGET} ${USER_CRT} ${SH_OBJS} -L${OBJDIR}/lib -lc -lfnu
	${OBJDUMP} -S ${.TARGET} > ${.TARGET}.asm

# the system tree
# the first boot makes this account with useradd(8), in wheel for doas
FIRSTBOOT_USER?=admin
FIRSTBOOT_GECOS?=Administrator
FIRSTBOOT_HASH?=$$2b$$10$$CxFGI6QcwErxc9x7VLuCmeE9E.VOwPo3eylNejAfnHgoMWAoZ24Au
SKEL_FILES!=cd ${.CURDIR} && find etc/skel -type f -print
ROOT_TREE=${OBJDIR}/root
ROOT_UUID=${OBJDIR}/root.uuid
KEYMAPS!=cd ${.CURDIR} && find share/vt/keymaps -type f -print
RC_FILES!=cd ${.CURDIR} && find etc/rc.d -type f -print
${OBJDIR}/root.stamp: ${INSTALL_BINS} ${INSTALL_TESTS} tests/run ${MKIMG} ${MAKEFILE_DEP} etc/filesystems etc/master.passwd etc/passwd etc/group etc/doas.conf etc/motd etc/profile etc/shrc etc/myname etc/hosts etc/shells etc/rc.conf etc/init.conf etc/users.conf etc/sysctl.conf etc/syslog.conf ${RC_FILES} ${SKEL_FILES} ${KEYMAPS} share/misc/units.lib
	rm -rf ${ROOT_TREE}
	mkdir -p ${ROOT_TREE}/usr/bin ${ROOT_TREE}/etc ${ROOT_TREE}/dev ${ROOT_TREE}/mnt ${ROOT_TREE}/data ${ROOT_TREE}/tmp
	mkdir -p ${ROOT_TREE}/var/run ${ROOT_TREE}/var/log/svc ${ROOT_TREE}/var/tmp ${ROOT_TREE}/var/empty ${ROOT_TREE}/var/db
	mkdir -p ${ROOT_TREE}/root ${ROOT_TREE}/home ${ROOT_TREE}/etc/skel
	chmod 1777 ${ROOT_TREE}/tmp ${ROOT_TREE}/var/tmp
	chmod 0555 ${ROOT_TREE}/var/empty
	chmod 0700 ${ROOT_TREE}/root
	: > ${ROOT_TREE}/var/log/wtmp
	chmod 0644 ${ROOT_TREE}/var/log/wtmp
	for b in ${INSTALL_BINS:N*/chpass:N*/doas:N*/passwd:N*/ping:N*/su}; do install -m 0555 ${.CURDIR}/$$b ${ROOT_TREE}/usr/bin/; done
	for b in ${SETUID_BINS}; do install -m 4555 ${.CURDIR}/${OBJDIR}/install/$$b ${ROOT_TREE}/usr/bin/; done
	for l in ${BIN_LINKS}; do ln ${ROOT_TREE}/usr/bin/$${l#*=} ${ROOT_TREE}/usr/bin/$${l%=*}; done
	mkdir -p ${ROOT_TREE}/usr/tests
	for b in ${INSTALL_TESTS}; do install -m 0555 ${.CURDIR}/$$b ${ROOT_TREE}/usr/tests/; done
	install -m 0555 ${.CURDIR}/tests/run ${ROOT_TREE}/usr/tests/run
	${MKIMG} -G > ${ROOT_UUID}
	install -m 0644 ${.CURDIR}/etc/filesystems ${.CURDIR}/etc/passwd ${.CURDIR}/etc/group ${.CURDIR}/etc/motd ${.CURDIR}/etc/profile ${.CURDIR}/etc/shrc ${.CURDIR}/etc/myname ${.CURDIR}/etc/hosts ${.CURDIR}/etc/shells ${.CURDIR}/etc/rc.conf ${.CURDIR}/etc/init.conf ${.CURDIR}/etc/users.conf ${.CURDIR}/etc/sysctl.conf ${.CURDIR}/etc/syslog.conf ${ROOT_TREE}/etc/
	printf 'from=/dev/part-by-label/fnu-swap type=swap at=boot\n' >> ${ROOT_TREE}/etc/filesystems
	printf 'from=/dev/part-by-uuid/%s on=/ type=ufs2 mode=rw at=boot\n' "$$(cat ${ROOT_UUID})" >> ${ROOT_TREE}/etc/filesystems
	mkdir -p ${ROOT_TREE}/etc/rc.d
	install -m 0644 ${.CURDIR}/etc/rc.d/* ${ROOT_TREE}/etc/rc.d/
	install -m 0600 ${.CURDIR}/etc/master.passwd ${ROOT_TREE}/etc/
	install -m 0644 ${.CURDIR}/etc/skel/dot.* ${ROOT_TREE}/etc/skel/
	for f in ${.CURDIR}/etc/skel/dot.*; do install -m 0644 $$f ${ROOT_TREE}/root/.$${f##*/dot.}; done
	printf '# what the first boot does, once; the build wrote it\nuseradd -m -G wheel -c \047%s\047 -p \047%s\047 %s\n' '${FIRSTBOOT_GECOS}' '${FIRSTBOOT_HASH}' '${FIRSTBOOT_USER}' > ${ROOT_TREE}/etc/firstboot
	chmod 0600 ${ROOT_TREE}/etc/firstboot
	: > ${ROOT_TREE}/firstboot
	install -m 0640 ${.CURDIR}/etc/doas.conf ${ROOT_TREE}/etc/
	mkdir -p ${ROOT_TREE}/usr/share/vt/keymaps
	install -m 0644 ${.CURDIR}/share/vt/keymaps/*.kbd ${.CURDIR}/share/vt/keymaps/INDEX.keymaps ${ROOT_TREE}/usr/share/vt/keymaps/
	mkdir -p ${ROOT_TREE}/usr/share/misc
	install -m 0444 ${.CURDIR}/share/misc/units.lib ${ROOT_TREE}/usr/share/misc/units.lib
	touch ${OBJDIR}/root.stamp
# ramdisk and root partition
ROOT_MB=64
SWAP_MB?=256
${FS_IMG}: ${OBJDIR}/root.stamp ${MKIMG} tools/mkufs.sh ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	sh ${.CURDIR}/tools/mkufs.sh ${MKIMG} ${.TARGET} ${ROOT_MB} FNU-LIVE ${ROOT_TREE}

${KERNEL_BOOT}: ${KERNEL}
	${OBJCOPY} --strip-debug ${KERNEL} ${.TARGET}

${PRONINX_IMG}: ${MBR_BIN} ${MBR_ELF} ${STAGE2_BIN} ${BOOTX64} ${KERNEL_BOOT} ${FS_IMG} ${OBJDIR}/root.stamp ${MKIMG} tools/mkimage.sh tools/mkufs.sh
	@mkdir -p ${.TARGET:H}
	ROOT_MB=${ROOT_MB} SWAP_MB=${SWAP_MB} sh ${.CURDIR}/tools/mkimage.sh ${MKIMG} ${.TARGET} ${MBR_BIN} ${MBR_ELF} ${STAGE2_BIN} ${KERNEL_BOOT} ${FS_IMG} ${BOOTX64} ${ROOT_TREE} "$$(cat ${ROOT_UUID})"
ci: ${IMAGES}
	test -s ${PRONINX_IMG}
	test -s ${FS_IMG}
	${MAKE} -C ${.CURDIR} smoke
clean:
	rm -f qemu.log kern/vectors.S
	rm -rf ${OBJDIR}
format:
	./format.sh
QEMU_EXTRA_OPTS?=
qemu: ${IMAGES}
	${QEMU} ${QEMU_VIDEO_OPTS} ${QEMU_SMP_OPTS} ${QEMU_NET_OPTS} -drive file=${PRONINX_IMG},index=0,media=disk,format=raw ${QEMU_EXTRA_OPTS} -serial mon:stdio
qemu-uefi: ${IMAGES}
	sh ${.CURDIR}/tools/run-uefi.sh ${QEMU} ${PRONINX_IMG} ${QEMU_VIDEO_OPTS} ${QEMU_SMP_OPTS} ${QEMU_NET_OPTS} ${QEMU_EXTRA_OPTS}
smoke: ${IMAGES}
	./tools/smoke-qemu.sh ${QEMU} ${PRONINX_IMG}
qemu-debug: ${IMAGES}
	${QEMU} ${QEMU_VIDEO_OPTS} ${QEMU_SMP_OPTS} ${QEMU_NET_OPTS} -drive file=${PRONINX_IMG},index=0,media=disk,format=raw ${QEMU_EXTRA_OPTS} -serial mon:stdio -gdb tcp::12345 -S
lldb: ${KERNEL}
	${LLDB} ${KERNEL} -o 'gdb-remote localhost:12345'

.for src in ${MBR_ASM} ${STAGE2_ASM} ${STAGE2_SRCS} ${UEFI_SRCS} ${UEFI_ASM_SRCS} ${KERN_SRCS} ${KERN_ASM_SRCS} ${LWIP_SRCS} ${E1000_SRCS} ${ZLIB_SRCS} ${X86EMU_SRCS} ${LIB_SRCS} ${USER_CRT_SRC} ${USER_SRCS} ${TEST_SRCS}
.sinclude "${OBJDIR}/${src:R}.d"
.endfor
.for src in ${BOOTCOMMON_SRCS}
.sinclude "${OBJDIR}/efi/${src:R}.d"
.endfor
.for l in ${LIBS}
.for src in ${LIB_${l}_SRCS}
.sinclude "${OBJDIR}/lib${l}/${src:R}.d"
.endfor
.endfor
.for src in ${SH_SRCS} ${SH_GEN}
.sinclude "${SH_OBJ}/${src:R}.d"
.endfor
.for p in ${PROGS}
.for src in ${PROG_${p}_SRCS} ${PROG_${p}_GEN}
.sinclude "${PROG_${p}_GENDIR}/${src:R}.d"
.endfor
.endfor
.for src in ${FNU_ONLY}
.sinclude "${OBJDIR}/${src:R}.d"
.endfor
.for blob in ${ENTRYOTHER} ${REALMODE} ${WAKECODE}
.sinclude "${blob}.d"
.endfor
