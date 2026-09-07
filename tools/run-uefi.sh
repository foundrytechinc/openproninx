#!/bin/sh
# Boot the image interactively through OVMF.
#
#   run-uefi.sh QEMU IMAGE [extra qemu options...]
set -eu

qemu=${1:?usage: run-uefi.sh QEMU IMAGE [options...]}
image=${2:?usage: run-uefi.sh QEMU IMAGE [options...]}
shift 2

code=""
vars=""
for c in \
	/usr/share/qemu/ovmf-x86_64-4m-code.bin:/usr/share/qemu/ovmf-x86_64-4m-vars.bin \
	/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd \
	/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd \
	/usr/share/edk2/x64/OVMF_CODE.4m.fd:/usr/share/edk2/x64/OVMF_VARS.4m.fd \
	/usr/share/edk2/x64/OVMF_CODE.fd:/usr/share/edk2/x64/OVMF_VARS.fd \
	/usr/share/edk2-ovmf/x64/OVMF_CODE.fd:/usr/share/edk2-ovmf/x64/OVMF_VARS.fd
do
	c_code=${c%%:*}
	c_vars=${c##*:}
	if [ -r "$c_code" ] && [ -r "$c_vars" ]; then
		code=$c_code
		vars=$c_vars
		break
	fi
done
if [ -z "$code" ]; then
	echo "run-uefi: no OVMF firmware found" >&2
	exit 1
fi

nvram=$(mktemp "${TMPDIR:-/tmp}/proninx-vars.XXXXXX")
trap 'rm -f "$nvram"' EXIT HUP INT TERM
cp "$vars" "$nvram"

exec "$qemu" -machine q35 \
	-drive "if=pflash,format=raw,readonly=on,file=$code" \
	-drive "if=pflash,format=raw,file=$nvram" \
	-drive "file=$image,index=0,media=disk,format=raw" \
	"$@" -serial mon:stdio
