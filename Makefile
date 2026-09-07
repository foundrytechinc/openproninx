# PRONINX build file for BSD make (bmake).

OBJDIR?=obj
.OBJDIR: ${.CURDIR}

# bmake's sys.mk predefines these, with GNU binutils and a -Wno-error that
# undoes the -Werror below. never ?= or += them. llvm only: no binutils.
CC=clang
LD=ld.lld
AR=llvm-ar
OBJCOPY=llvm-objcopy
LDPE=lld-link
OBJDUMP=llvm-objdump
QEMU?=qemu-system-x86_64
GDB?=gdb
QEMU_VIDEO_OPTS?=-device virtio-vga -display gtk,zoom-to-fit=off
# The loader asks the monitor for its native size over VBE/DDC. These are only
# used when it will not answer. Set VBE_WIDTH/VBE_HEIGHT to force a size:
#   bmake VBE_WIDTH=1600 VBE_HEIGHT=900
VBE_FALLBACK_WIDTH?=1024
VBE_FALLBACK_HEIGHT?=768
VBE_FORCE=
.if defined(VBE_WIDTH) && defined(VBE_HEIGHT)
VBE_FORCE=-DVBE_WIDTH=${VBE_WIDTH} -DVBE_HEIGHT=${VBE_HEIGHT}
.endif
CPUS?=2
QEMU_SMP_OPTS?=-smp cpus=${CPUS},sockets=${CPUS}
QEMU_NET_OPTS?=-netdev user,id=proninx-net0 -device e1000,netdev=proninx-net0


CFLAGS=-fno-pic -static -fno-builtin -fno-strict-aliasing -MD -ggdb
CFLAGS+=-fno-asynchronous-unwind-tables -fno-unwind-tables
CFLAGS+=-fno-omit-frame-pointer -O1 -std=gnu11 -Wall -Wextra
CFLAGS+=-Wno-format -Wno-unused -Wno-unused-parameter -Wno-address-of-packed-member -Wno-unknown-warning-option -Wno-gnu-designator -Werror
CFLAGS+=-fno-stack-protector -mno-mmx -mno-sse -mno-sse2 -mno-sse3
CFLAGS+=-mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mfpmath=387 -fno-pie
BOOT_CFLAGS=${CFLAGS} -m32 -ffreestanding -nostdinc -I. -Iboot -DVBE_FALLBACK_WIDTH=${VBE_FALLBACK_WIDTH} -DVBE_FALLBACK_HEIGHT=${VBE_FALLBACK_HEIGHT} ${VBE_FORCE}
KERN_CFLAGS=${CFLAGS} -m64 -mcmodel=kernel -nostdinc -I.
LIB_CFLAGS=${CFLAGS} -m64 -nostdinc -I.
USER_CFLAGS=${CFLAGS} -m64 -nostdinc -I.
BOOT_LDFLAGS=-m elf_i386
# the uefi loader is a PE32+ image: clang's windows target, lld-link, no edk2
UEFI_CFLAGS=-target x86_64-unknown-windows-gnu -ffreestanding -fno-builtin
UEFI_CFLAGS+=-fno-strict-aliasing -fno-stack-protector -fno-omit-frame-pointer
UEFI_CFLAGS+=-mno-red-zone -mno-mmx -mno-sse -O1 -std=gnu11 -Wall -Wextra
UEFI_CFLAGS+=-Wno-format -Wno-unused -Wno-unused-parameter -Wno-unknown-warning-option -Werror
UEFI_CFLAGS+=-MD -nostdinc -I. -DVBE_FALLBACK_WIDTH=${VBE_FALLBACK_WIDTH} -DVBE_FALLBACK_HEIGHT=${VBE_FALLBACK_HEIGHT} ${VBE_FORCE}
UEFI_LDFLAGS=-subsystem:efi_application -entry:efi_main -nodefaultlib
KERN_LDFLAGS=-m elf_x86_64
USER_LDFLAGS=-T user/user.ld

MAKEFILE_DEP=${.CURDIR}/Makefile

MBR_ELF=${OBJDIR}/boot/mbr.elf
MBR_BIN=${OBJDIR}/boot/mbr.bin
STAGE2_ELF=${OBJDIR}/boot/stage2.elf
STAGE2_BIN=${OBJDIR}/boot/stage2.bin
KERNEL=${OBJDIR}/kern/kernel
LIBRARY=${OBJDIR}/lib/libPRONINX_x86_64.a
INITCODE=${OBJDIR}/kern/initcode
ENTRYOTHER=${OBJDIR}/kern/entryother
REALMODE=${OBJDIR}/kern/rmtramp
BLOBS=${OBJDIR}/kern/blobs.o
BOOTX64=${OBJDIR}/boot/uefi/BOOTX64.EFI
MKFS=${OBJDIR}/kern/mkfs
PRONINX_IMG=${OBJDIR}/PRONINX.img
FS_IMG=${OBJDIR}/fs.img

MBR_ASM=boot/stage_1.S
STAGE2_ASM=boot/entry.S boot/a20.S boot/realprot.S boot/bios.S
STAGE2_SRCS=boot/main.c boot/disk.c boot/fat.c boot/video.c boot/coreboot.c
UEFI_SRCS=boot/uefi/main.c boot/uefi/file.c boot/uefi/video.c boot/uefi/mem.c
KERN_SRCS!=cd ${.CURDIR} && find kern -type f -name '*.c' ! -name mkfs.c -print
KERN_ASM_SRCS=kern/entry.S kern/swtch.S kern/trapasm.S kern/vectors.S
LWIP_SRCS=third_party/lwip/src/core/def.c third_party/lwip/src/core/dns.c third_party/lwip/src/core/init.c third_party/lwip/src/core/inet_chksum.c third_party/lwip/src/core/ip.c third_party/lwip/src/core/mem.c third_party/lwip/src/core/memp.c third_party/lwip/src/core/netif.c third_party/lwip/src/core/pbuf.c third_party/lwip/src/core/raw.c third_party/lwip/src/core/stats.c third_party/lwip/src/core/sys.c third_party/lwip/src/core/tcp.c third_party/lwip/src/core/tcp_in.c third_party/lwip/src/core/tcp_out.c third_party/lwip/src/core/timeouts.c third_party/lwip/src/core/udp.c third_party/lwip/src/core/ipv4/dhcp.c third_party/lwip/src/core/ipv4/etharp.c third_party/lwip/src/core/ipv4/icmp.c third_party/lwip/src/core/ipv4/igmp.c third_party/lwip/src/core/ipv4/ip4.c third_party/lwip/src/core/ipv4/ip4_addr.c third_party/lwip/src/core/ipv4/ip4_frag.c third_party/lwip/src/netif/ethernet.c
LIB_SRCS=lib/string.c
USER_LIB_SRCS=user/usys.S user/entry.S user/printf.c user/gets.c user/stat.c
USER_SRCS!=cd ${.CURDIR} && find user -maxdepth 1 -type f -name '*.c' ! -name 'umalloc.c' ! -name 'printf.c' ! -name 'gets.c' ! -name 'stat.c' -print
MBR_OBJS=
STAGE2_OBJS=
UEFI_OBJS=
KERN_OBJS=
LWIP_OBJS=
LIB_OBJS=
USER_LIB_OBJS=
USER_BINS=
.for src in ${MBR_ASM}
MBR_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${STAGE2_ASM} ${STAGE2_SRCS}
STAGE2_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${UEFI_SRCS}
UEFI_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${KERN_SRCS} ${KERN_ASM_SRCS}
KERN_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${LWIP_SRCS}
LWIP_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${LIB_SRCS}
LIB_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${USER_LIB_SRCS}
USER_LIB_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${USER_SRCS}
USER_BINS+=${OBJDIR}/${src:R}
.endfor
IMAGES=${PRONINX_IMG} ${FS_IMG}

.SUFFIXES:
.PHONY: default all clean format ci smoke qemu qemu-uefi qemu-gdb gdb system-image si
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

.for src in ${UEFI_SRCS}
${OBJDIR}/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${UEFI_CFLAGS} -c -o ${.TARGET} ${src}
.endfor

.for src in ${MBR_ASM} ${STAGE2_ASM} ${STAGE2_SRCS}
${OBJDIR}/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${BOOT_CFLAGS} -c -o ${.TARGET} ${src}
.endfor

${LIBRARY}: ${LIB_OBJS}
	@mkdir -p ${.TARGET:H}
	@rm -f ${.TARGET}
	${AR} rcs ${.TARGET} ${.ALLSRC}
.for src in ${LIB_SRCS}
${OBJDIR}/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${LIB_CFLAGS} -c -o ${.TARGET} ${src}
.endfor

${INITCODE}: kern/initcode.S ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${CFLAGS} -m64 -fno-pic -nostdinc -I. -c -o ${.TARGET}.o kern/initcode.S
	${LD} -m elf_x86_64 --image-base=0 -N -e start -Ttext 0 -o ${.TARGET}.out ${.TARGET}.o
	${OBJCOPY} -S -O binary ${.TARGET}.out ${.TARGET}
	${OBJDUMP} -S ${.TARGET}.o > ${.TARGET}.asm

${ENTRYOTHER}: kern/entryother.S ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${CFLAGS} -m32 -nostdinc -I. -c -o ${.TARGET}.o kern/entryother.S
	${LD} -m elf_i386 --image-base=0 -N -e start -Ttext 0x7000 -o ${.TARGET}.out ${.TARGET}.o
	${OBJCOPY} -S -O binary ${.TARGET}.out ${.TARGET}
	${OBJDUMP} -S ${.TARGET}.o > ${.TARGET}.asm

${REALMODE}: kern/rmtramp.S kern/realmode.h ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${CFLAGS} -m32 -nostdinc -I. -Ikern -c -o ${.TARGET}.o kern/rmtramp.S
	${LD} -m elf_i386 --image-base=0 -N -e realmode_enter -Ttext 0xb000 -o ${.TARGET}.out ${.TARGET}.o
	${OBJCOPY} -S -O binary ${.TARGET}.out ${.TARGET}
	${OBJDUMP} -S ${.TARGET}.o > ${.TARGET}.asm

# linked from inside OBJDIR: ld -b binary bakes the path into the symbol name
${BLOBS}: ${INITCODE} ${ENTRYOTHER} ${REALMODE} ${MAKEFILE_DEP}
	cd ${OBJDIR}/kern && ${LD} -r ${KERN_LDFLAGS} -b binary -o blobs.o \
		initcode entryother rmtramp

${KERNEL}: ${KERN_OBJS} ${LWIP_OBJS} kern/kernel.ld ${BLOBS} ${LIBRARY}
	@mkdir -p ${.TARGET:H}
	${LD} ${KERN_LDFLAGS} -T kern/kernel.ld -o ${.TARGET} ${KERN_OBJS} ${LWIP_OBJS} ${BLOBS} -L${OBJDIR}/lib -lPRONINX_x86_64
	${OBJDUMP} -S ${.TARGET} > ${.TARGET}.asm
.for src in ${KERN_SRCS} ${KERN_ASM_SRCS} ${LWIP_SRCS}
${OBJDIR}/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${KERN_CFLAGS} -fno-pic -Ikern -Ikern/net -Ikern/storage -Ikern/net/lwip/port/include -Ithird_party/lwip/src/include -c -o ${.TARGET} ${src}
.endfor

kern/vectors.S: kern/vectors.sh
	tr -d '\r' < ${.ALLSRC} | sh > ${.TARGET}

${MKFS}: kern/mkfs.c kern/fs.h kern/param.h inc/dir.h inc/stat.h inc/types.h
	@mkdir -p ${.TARGET:H}
	${CC} -std=c11 -Wall -Wextra -Wno-format -Wno-unused -Wno-address-of-packed-member -Werror -I. -o ${.TARGET} kern/mkfs.c
	chmod +x ${.TARGET}

.for src in ${USER_LIB_SRCS}
${OBJDIR}/${src:R}.o: ${src} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${USER_CFLAGS} -c -o ${.TARGET} ${src}
.endfor
.for src in ${USER_SRCS}
${OBJDIR}/${src:R}: ${src} ${USER_LIB_OBJS} ${LIBRARY} ${MAKEFILE_DEP}
	@mkdir -p ${.TARGET:H}
	${CC} ${USER_CFLAGS} -c -o ${.TARGET}.o ${src}
	${LD} ${USER_LDFLAGS} -o ${.TARGET} ${.TARGET}.o ${USER_LIB_OBJS} -L${OBJDIR}/lib -lPRONINX_x86_64
	${OBJDUMP} -S ${.TARGET} > ${.TARGET}.asm
.endfor

${FS_IMG}: ${MKFS} ${USER_BINS}
	@mkdir -p ${.TARGET:H}
	${.CURDIR}/${MKFS} ${.TARGET} ${USER_BINS:S,^,${.CURDIR}/,}
${PRONINX_IMG}: ${MBR_BIN} ${MBR_ELF} ${STAGE2_BIN} ${BOOTX64} ${KERNEL} ${FS_IMG} tools/mkimage.sh
	@mkdir -p ${.TARGET:H}
	sh ${.CURDIR}/tools/mkimage.sh ${.TARGET} ${MBR_BIN} ${MBR_ELF} ${STAGE2_BIN} ${KERNEL} ${FS_IMG} ${BOOTX64}
ci: ${IMAGES}
	test -s ${PRONINX_IMG}
	test -s ${FS_IMG}
	${MAKE} -C ${.CURDIR} smoke
clean:
	rm -f .gdbinit qemu.log kern/vectors.S
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
system-image: ci
	./tools/build-ufs2-system.sh ${OBJDIR}/fnu-system.ufs
si: system-image
.gdbinit: .gdbinit.tmpl
	sed 's/localhost:1234/localhost:12345/' < ${.ALLSRC} > ${.TARGET}
gdb: .gdbinit
	${GDB} -n -x .gdbinit
qemu-gdb: ${IMAGES} .gdbinit
	${QEMU} ${QEMU_VIDEO_OPTS} ${QEMU_SMP_OPTS} ${QEMU_NET_OPTS} -drive file=${PRONINX_IMG},index=0,media=disk,format=raw ${QEMU_EXTRA_OPTS} -serial mon:stdio -gdb tcp::12345 -S

# -MD writes these. they carry rules, so they come after the default target.
.for src in ${MBR_ASM} ${STAGE2_ASM} ${STAGE2_SRCS} ${UEFI_SRCS} ${KERN_SRCS} ${KERN_ASM_SRCS} ${LWIP_SRCS} ${LIB_SRCS} ${USER_LIB_SRCS} ${USER_SRCS}
.sinclude "${OBJDIR}/${src:R}.d"
.endfor
