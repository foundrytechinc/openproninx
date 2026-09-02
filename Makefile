# Try to infer the correct QEMU
ifndef QEMU
	QEMU = qemu-system-x86_64
	# QEMU = qemu-system-i386
endif

OBJDIR := obj

CC = gcc
AS = gas
LD = ld
OBJCOPY = objcopy
OBJDUMP = objdump
CP = cp
DD = dd
MKDIR = mkdir
GDB = gdb
AR = ar

CFLAGS = -fno-pic -static -fno-builtin -fno-strict-aliasing -MD -ggdb -fno-omit-frame-pointer
# CFLAGS += -O2 -std=c11 -Wall -Wextra -Wno-format -Wno-unused -Wno-address-of-packed-member -Werror
CFLAGS += -O1 -std=c11 -Wall -Wextra -Wno-format -Wno-unused -Wno-address-of-packed-member -Werror
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)
# Prevent gcc from generating MMX and SSE instructions
CFLAGS += -mno-mmx -mno-sse -mno-sse2 -mno-sse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mfpmath=387
# ASFLAGS = -m32 -gdwarf-2 -Wa,-divide
# LDFLAGS += -m elf_x86_64
# LDFLAGS += -m elf_i386
LDFLAGS :=

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
	CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
	CFLAGS += -fno-pie -nopie
endif

GDBPORT	:= 12345
CPUS ?= 1
ifneq ($(CPUS),1)
$(error SMP is not implemented: build and run with CPUS=1)
endif

PRONINX_IMG := $(OBJDIR)/PRONINX.img
FS_IMG := $(OBJDIR)/fs.img
UFS2_SYSTEM_IMG ?=
UFS2_DATA_IMG ?=
IMAGES := $(PRONINX_IMG) $(FS_IMG)
UOBJS :=

default: $(IMAGES)

.PHONY: clean default format ci smoke system-image FORCE

FORCE:

include boot/module.mk
include lib/module.mk
include user/module.mk
include kern/module.mk

# Disc sector start no where kernel image is loaded
KERNEL_START_SECTOR := 32

# Formatting source files must be an explicit developer action. A build must
# never rewrite its inputs: that keeps images reproducible and CI clean.
$(PRONINX_IMG): $(OBJDIR)/$(BOOT_BLOCK) $(OBJDIR)/$(KERNEL)
	dd if=/dev/zero of=$@ count=10000
	dd if=$(OBJDIR)/$(BOOT_BLOCK) of=$@ conv=notrunc
	dd if=$(OBJDIR)/$(KERNEL) of=$@ seek=$(KERNEL_START_SECTOR) conv=notrunc

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o

# Enter QEMU monitor by 'Ctrl+a then c' if -serial mon:stdio is specified
# ref. https://kashyapc.wordpress.com/2016/02/11/qemu-command-line-behavior-of-serial-stdio-vs-serial-monstdio/
QEMUOPTS := $(QEMUOPTS)
QEMU_SYSTEM_IMG := $(if $(strip $(UFS2_SYSTEM_IMG)),$(UFS2_SYSTEM_IMG),$(FS_IMG))
QEMUOPTS += -drive file=$(PRONINX_IMG),index=0,media=disk,format=raw \
			-drive file=$(QEMU_SYSTEM_IMG),if=ide,index=1,media=disk,format=raw \
			-netdev user,id=proninx-net0 -device virtio-net-pci,netdev=proninx-net0 \
			-serial mon:stdio -gdb tcp::$(GDBPORT) -smp $(CPUS)
ifneq ($(strip $(UFS2_DATA_IMG)),)
QEMUOPTS += -drive file=$(UFS2_DATA_IMG),if=ide,index=2,media=disk,format=raw
endif
QEMUOPTS += $(shell if $(QEMU) -nographic -help | grep -q '^-D '; then echo '-D qemu.log'; fi)

.gdbinit: .gdbinit.tmpl
	sed "s/localhost:1234/localhost:$(GDBPORT)/" < $^ > $@

gdb:
	$(GDB) -n -x .gdbinit

# qemu: $(IMAGES) pre-qemu
qemu: $(IMAGES)
	$(QEMU) $(QEMUOPTS)

qemu-gdb: $(IMAGES) .gdbinit
	$(QEMU) $(QEMUOPTS) -S

# Minimal non-interactive gate used by CI and release builders.
ci: $(IMAGES)
	test -s $(PRONINX_IMG)
	test -s $(FS_IMG)

# Boot the two development images and require PID 1 to reach the local shell.
# This intentionally uses a timeout: the kernel is an interactive OS and
# does not terminate on its own.
smoke: $(IMAGES)
	./tools/smoke-qemu.sh $(QEMU) $(PRONINX_IMG) $(FS_IMG)

# Host-side release tooling. It uses a BSD-compatible makefs and never puts a
# formatter into the PRONINX kernel or the target system.
system-image: ci
	./tools/build-ufs2-system.sh $(OBJDIR)/fnu-system.ufs

format:
	./format.sh

clean:
	rm -f *.o *.d *.asm *.bin .gdbinit qemu.log
	rm -rf $(OBJDIR)
