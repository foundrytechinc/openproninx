#!/bin/sh
# GPT image for BIOS and UEFI:
# mkimage.sh MKIMG OUT MBR MBR_ELF STAGE2 KERNEL RAMDISK BOOTX64 [ROOTDIR [ROOT_UUID]]
set -eu

mkimg=${1:?usage: mkimage.sh MKIMG OUT MBR MBR_ELF STAGE2 KERNEL RAMDISK BOOTX64 [ROOTDIR [ROOT_UUID]]}
out=${2:?}
mbr=${3:?}
mbr_elf=${4:?}
stage2=${5:?}
kernel=${6:?}
ramdisk=${7:?}
bootx64=${8:?}
rootdir=${9:-}
root_uuid=${10:-}

ROOT_MB=${ROOT_MB:?the size of the root partition in MiB, from the Makefile}

root_img=
trap 'rm -f "$root_img"' EXIT HUP INT TERM

set -- -o "$out" -m "$mbr" -e "$mbr_elf" -s "$stage2" -k "$kernel" -b "$bootx64"
if [ -s "$ramdisk" ]; then
	set -- "$@" -r "$ramdisk"
fi
# some BIOSes want an active partition
if [ "${HYBRID_ACTIVE:-0}" != "0" ]; then
	set -- "$@" -a
fi
if [ -n "${SWAP_MB:-}" ]; then
	set -- "$@" -w "$SWAP_MB"
fi

if [ -n "$rootdir" ] && [ -d "$rootdir" ]; then
	root_img=$(mktemp)
	sh "$(dirname "$0")/mkufs.sh" "$mkimg" "$root_img" "$ROOT_MB" FNU-ROOT "$rootdir"
	set -- "$@" -u "$root_img"
	if [ -n "$root_uuid" ]; then
		set -- "$@" -R "$root_uuid"
	fi
fi

"$mkimg" "$@"
