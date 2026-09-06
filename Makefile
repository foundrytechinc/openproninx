# PRONINX build file for BSD make (bmake).

OBJDIR?=obj
.OBJDIR: ${.CURDIR}
CC=clang
LD?=ld
AR?=ar
OBJCOPY?=objcopy
OBJDUMP?=objdump
QEMU?=qemu-system-x86_64
GDB?=gdb
QEMU_VIDEO_OPTS?=-device virtio-vga -display gtk,zoom-to-fit=off
# A console does not benefit from a full-HD canvas: it leaves most of the
# display empty and makes the QEMU window unnecessarily large.  Callers may
# still override these limits for a different display mode.
VBE_MAX_WIDTH?=1024
VBE_MAX_HEIGHT?=768
CPUS?=2
QEMU_SMP_OPTS?=-smp cpus=${CPUS},sockets=${CPUS}
QEMU_NET_OPTS?=-netdev user,id=proninx-net0 -device e1000,netdev=proninx-net0


CFLAGS+=-fno-pic -static -fno-builtin -fno-strict-aliasing -MD -ggdb
CFLAGS+=-fno-asynchronous-unwind-tables -fno-unwind-tables
CFLAGS+=-fno-omit-frame-pointer -O1 -std=gnu11 -Wall -Wextra
CFLAGS+=-Wno-format -Wno-unused -Wno-unused-parameter -Wno-address-of-packed-member -Wno-unknown-warning-option -Wno-gnu-designator -Werror
CFLAGS+=-fno-stack-protector -mno-mmx -mno-sse -mno-sse2 -mno-sse3
CFLAGS+=-mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mfpmath=387 -fno-pie
BOOT_CFLAGS=${CFLAGS} -m32 -nostdinc -I. -DVBE_MAX_WIDTH=${VBE_MAX_WIDTH} -DVBE_MAX_HEIGHT=${VBE_MAX_HEIGHT}
KERN_CFLAGS=${CFLAGS} -m64 -mcmodel=kernel -nostdinc -I.
LIB_CFLAGS=${CFLAGS} -m64 -nostdinc -I.
USER_CFLAGS=${CFLAGS} -m64 -nostdinc -I.
BOOT_LDFLAGS=-m elf_i386
KERN_LDFLAGS=-m elf_x86_64
USER_LDFLAGS=-T user/user.ld

BOOT_BLOCK=${OBJDIR}/boot/bootblock
KERNEL=${OBJDIR}/kern/kernel
LIBRARY=${OBJDIR}/lib/libPRONINX_x86_64.a
INITCODE=${OBJDIR}/kern/initcode
ENTRYOTHER=${OBJDIR}/kern/entryother
MKFS=${OBJDIR}/kern/mkfs
PRONINX_IMG=${OBJDIR}/PRONINX.img
FS_IMG=${OBJDIR}/fs.img
KERNEL_START_SECTOR=32

BOOT_SRCS!=cd ${.CURDIR} && find boot -maxdepth 1 -type f -name '*.c' -print
BOOT_ASM_SRCS=boot/entrypgdir.S boot/stage_1.S boot/stage_3.S boot/video.S
KERN_SRCS!=cd ${.CURDIR} && find kern -type f -name '*.c' ! -name mkfs.c -print
KERN_ASM_SRCS=kern/entry.S kern/swtch.S kern/trapasm.S kern/vectors.S
LWIP_SRCS=third_party/lwip/src/core/def.c third_party/lwip/src/core/dns.c third_party/lwip/src/core/init.c third_party/lwip/src/core/inet_chksum.c third_party/lwip/src/core/ip.c third_party/lwip/src/core/mem.c third_party/lwip/src/core/memp.c third_party/lwip/src/core/netif.c third_party/lwip/src/core/pbuf.c third_party/lwip/src/core/raw.c third_party/lwip/src/core/stats.c third_party/lwip/src/core/sys.c third_party/lwip/src/core/tcp.c third_party/lwip/src/core/tcp_in.c third_party/lwip/src/core/tcp_out.c third_party/lwip/src/core/timeouts.c third_party/lwip/src/core/udp.c third_party/lwip/src/core/ipv4/dhcp.c third_party/lwip/src/core/ipv4/etharp.c third_party/lwip/src/core/ipv4/icmp.c third_party/lwip/src/core/ipv4/igmp.c third_party/lwip/src/core/ipv4/ip4.c third_party/lwip/src/core/ipv4/ip4_addr.c third_party/lwip/src/core/ipv4/ip4_frag.c third_party/lwip/src/netif/ethernet.c
LIB_SRCS=lib/string.c
USER_LIB_SRCS=user/usys.S user/entry.S user/printf.c user/gets.c user/stat.c
USER_SRCS!=cd ${.CURDIR} && find user -maxdepth 1 -type f -name '*.c' ! -name 'umalloc.c' ! -name 'printf.c' ! -name 'gets.c' ! -name 'stat.c' -print
BOOT_OBJS=
KERN_OBJS=
LWIP_OBJS=
LIB_OBJS=
USER_LIB_OBJS=
USER_BINS=
.for src in ${BOOT_SRCS} ${BOOT_ASM_SRCS}
BOOT_OBJS+=${OBJDIR}/${src:R}.o
.endfor
.for src in ${KERN_SRCS} ${KERN_ASM_SRCS}
KERN_OBJS+=${OBJDIR}/${src:R}.o
.endfor
KERN_OBJS+=${OBJDIR}/kern/ramdisk_img.o
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

.PHONY: default all clean format ci smoke qemu qemu-gdb gdb system-image si FORCE
default: ${IMAGES}
all: default

${BOOT_BLOCK}: ${BOOT_OBJS} boot/boot.ld
	@mkdir -p ${.TARGET:H}
	${LD} ${BOOT_LDFLAGS} -N -T boot/boot.ld -o ${.TARGET}.o ${BOOT_OBJS}
	${OBJDUMP} -S ${.TARGET}.o > ${.TARGET}.asm
	${OBJCOPY} -S -O binary -j .stage_1 -j .rest_of_bootloader -j .page_table ${.TARGET}.o ${.TARGET}

.for src in ${BOOT_SRCS} ${BOOT_ASM_SRCS}
${OBJDIR}/${src:R}.o: ${src}
	@mkdir -p ${.TARGET:H}
	${CC} ${BOOT_CFLAGS} -c -o ${.TARGET} ${.ALLSRC}
.endfor

${LIBRARY}: ${LIB_OBJS}
	@mkdir -p ${.TARGET:H}
	${AR} r ${.TARGET} ${.ALLSRC}
.for src in ${LIB_SRCS}
${OBJDIR}/${src:R}.o: ${src}
	@mkdir -p ${.TARGET:H}
	${CC} ${LIB_CFLAGS} -c -o ${.TARGET} ${.ALLSRC}
.endfor

${INITCODE}: kern/initcode.S
	@mkdir -p ${.TARGET:H}
	${CC} ${CFLAGS} -m64 -fno-pic -nostdinc -I. -c -o ${.TARGET}.o ${.ALLSRC}
	${LD} -m elf_x86_64 -N -e start -Ttext 0 -o ${.TARGET}.out ${.TARGET}.o
	${OBJCOPY} -S -O binary ${.TARGET}.out ${.TARGET}
	${OBJDUMP} -S ${.TARGET}.o > ${.TARGET}.asm

${ENTRYOTHER}: kern/entryother.S
	@mkdir -p ${.TARGET:H}
	${CC} ${CFLAGS} -m32 -nostdinc -I. -c -o ${.TARGET}.o ${.ALLSRC}
	${LD} -m elf_i386 -N -e start -Ttext 0x7000 -o ${.TARGET}.out ${.TARGET}.o
	${OBJCOPY} -S -O binary ${.TARGET}.out ${.TARGET}
	${OBJDUMP} -S ${.TARGET}.o > ${.TARGET}.asm

${OBJDIR}/kern/ramdisk_img.o: kern/ramdisk_img.S ${FS_IMG}
	@mkdir -p ${.TARGET:H}
	${CC} ${KERN_CFLAGS} -DFS_IMG_PATH='"${FS_IMG}"' -fno-pic -Ikern -c -o ${.TARGET} kern/ramdisk_img.S

${KERNEL}: ${KERN_OBJS} ${LWIP_OBJS} kern/kernel.ld ${INITCODE} ${ENTRYOTHER} ${LIBRARY}
	@mkdir -p ${.TARGET:H}
	${LD} ${KERN_LDFLAGS} -T kern/kernel.ld -o ${.TARGET} ${KERN_OBJS} ${LWIP_OBJS} -L${OBJDIR}/lib -lPRONINX_x86_64 -b binary ${INITCODE} ${ENTRYOTHER}
	${OBJDUMP} -S ${.TARGET} > ${.TARGET}.asm
.for src in ${KERN_SRCS} ${KERN_ASM_SRCS} ${LWIP_SRCS}
${OBJDIR}/${src:R}.o: ${src}
	@mkdir -p ${.TARGET:H}
	${CC} ${KERN_CFLAGS} -fno-pic -Ikern -Ikern/net -Ikern/storage -Ikern/net/lwip/port/include -Ithird_party/lwip/src/include -c -o ${.TARGET} ${.ALLSRC}
.endfor

kern/vectors.S: kern/vectors.sh FORCE
	tr -d '\r' < kern/vectors.sh | bash > ${.TARGET}
${MKFS}: kern/mkfs.c kern/fs.h kern/param.h inc/dir.h inc/stat.h inc/types.h
	@mkdir -p ${.TARGET:H}
	${CC} -std=c11 -Wall -Wextra -Wno-format -Wno-unused -Wno-address-of-packed-member -Werror -I. -o ${.TARGET} kern/mkfs.c
	chmod +x ${.TARGET}

.for src in ${USER_LIB_SRCS}
${OBJDIR}/${src:R}.o: ${src}
	@mkdir -p ${.TARGET:H}
	${CC} ${USER_CFLAGS} -c -o ${.TARGET} ${.ALLSRC}
.endfor
.for src in ${USER_SRCS}
${OBJDIR}/${src:R}: ${src} ${USER_LIB_OBJS} ${LIBRARY}
	@mkdir -p ${.TARGET:H}
	${CC} ${USER_CFLAGS} -c -o ${.TARGET}.o ${.ALLSRC:M*.c}
	${LD} ${USER_LDFLAGS} -o ${.TARGET} ${.TARGET}.o ${USER_LIB_OBJS} -L${OBJDIR}/lib -lPRONINX_x86_64
	${OBJDUMP} -S ${.TARGET} > ${.TARGET}.asm
.endfor

${FS_IMG}: ${MKFS} ${USER_BINS}
	@mkdir -p ${.TARGET:H}
	${.CURDIR}/${MKFS} ${.TARGET} ${USER_BINS:S,^,${.CURDIR}/,}
${PRONINX_IMG}: ${BOOT_BLOCK} ${KERNEL}
	@mkdir -p ${.TARGET:H}
	dd if=/dev/zero of=${.TARGET} count=20000
	dd if=${BOOT_BLOCK} of=${.TARGET} conv=notrunc
	dd if=${KERNEL} of=${.TARGET} seek=${KERNEL_START_SECTOR} conv=notrunc
FORCE:

ci: ${IMAGES}
	test -s ${PRONINX_IMG}
	test -s ${FS_IMG}
clean:
	rm -f .gdbinit qemu.log
	rm -rf ${OBJDIR}
format:
	./format.sh
QEMU_EXTRA_OPTS?=
qemu: ${IMAGES}
	${QEMU} ${QEMU_VIDEO_OPTS} ${QEMU_SMP_OPTS} ${QEMU_NET_OPTS} -drive file=${PRONINX_IMG},index=0,media=disk,format=raw ${QEMU_EXTRA_OPTS} -serial mon:stdio
smoke: ${IMAGES}
	./tools/smoke-qemu.sh ${QEMU} ${PRONINX_IMG}
system-image: ci
	./tools/build-ufs2-system.sh ${OBJDIR}/fnu-system.ufs
si: system-image
${.OBJDIR}/.gdbinit: .gdbinit.tmpl
	sed 's/localhost:1234/localhost:12345/' < ${.ALLSRC} > .gdbinit
gdb: .gdbinit
	${GDB} -n -x .gdbinit
qemu-gdb: ${IMAGES} .gdbinit
	${QEMU} ${QEMU_VIDEO_OPTS} ${QEMU_SMP_OPTS} ${QEMU_NET_OPTS} -drive file=${PRONINX_IMG},index=0,media=disk,format=raw ${QEMU_EXTRA_OPTS} -serial mon:stdio -gdb tcp::12345 -S
