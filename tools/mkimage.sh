#!/bin/sh
# Build a GPT image that boots on both BIOS and UEFI.
#
#   mkimage.sh OUT MBR MBR_ELF STAGE2 KERNEL RAMDISK BOOTX64
set -eu

# sgdisk lives in sbin
PATH=$PATH:/usr/sbin:/sbin
export PATH
for t in sgdisk mformat mmd mcopy; do
	command -v "$t" >/dev/null || { echo "mkimage: $t not found" >&2; exit 1; }
done

out=${1:?usage: mkimage.sh OUT MBR MBR_ELF STAGE2 KERNEL RAMDISK BOOTX64}
mbr=${2:?}
mbr_elf=${3:?}
stage2=${4:?}
kernel=${5:?}
ramdisk=${6:?}
bootx64=${7:?}

ESP_MB=64
BOOT_MB=1
ROOT_MB=32

esp_sectors=$((ESP_MB * 2048))
boot_sectors=$((BOOT_MB * 2048))
root_sectors=$((ROOT_MB * 2048))

esp_start=2048
boot_start=$((esp_start + esp_sectors))
root_start=$((boot_start + boot_sectors))
total=$((root_start + root_sectors + 2048))

FNUBOOT_GUID=7B589DA3-BDC2-4F6C-8028-FC43D46C5953

stage2_bytes=$(wc -c < "$stage2")
stage2_secs=$(( (stage2_bytes + 511) / 512 ))
if [ "$stage2_secs" -gt "$boot_sectors" ]; then
	echo "mkimage: stage2 is larger than the fnu-boot partition" >&2
	exit 1
fi

rm -f "$out"
dd if=/dev/zero of="$out" bs=512 count="$total" status=none

sgdisk \
	--new=1:$esp_start:+${ESP_MB}M --typecode=1:EF00 --change-name=1:"EFI System" \
	--new=2:$boot_start:+${BOOT_MB}M --typecode=2:$FNUBOOT_GUID --change-name=2:"fnu-boot" \
	--new=3:$root_start:+${ROOT_MB}M --typecode=3:8300 --change-name=3:"fnu-root" \
	--attributes=2:set:2 \
	"$out" > /dev/null

# some legacy BIOSes refuse a disk with no active MBR partition
if [ "${HYBRID_ACTIVE:-0}" != "0" ]; then
	printf '\200' | dd of="$out" bs=1 seek=446 count=1 conv=notrunc status=none
fi

# ESP holds the kernel and the ramdisk for both boot paths
esp_img=$(mktemp)
trap 'rm -f "$esp_img"' EXIT HUP INT TERM
dd if=/dev/zero of="$esp_img" bs=512 count="$esp_sectors" status=none
mformat -i "$esp_img" -F -v FNUESP ::
mmd -i "$esp_img" ::/EFI ::/EFI/BOOT ::/fnu
mcopy -i "$esp_img" "$kernel" ::/fnu/kernel
mcopy -i "$esp_img" "$bootx64" ::/EFI/BOOT/BOOTX64.EFI
if [ -s "$ramdisk" ]; then
	mcopy -i "$esp_img" "$ramdisk" ::/fnu/ramdisk
fi
dd if="$esp_img" of="$out" bs=512 seek="$esp_start" conv=notrunc status=none

dd if="$stage2" of="$out" bs=512 seek="$boot_start" conv=notrunc status=none

# patch the sector count stage1 must load, then keep sgdisk's protective table
nsects_addr=$(llvm-nm "$mbr_elf" 2>/dev/null | awk '$3 == "nsects" { print $1 }')
if [ -z "$nsects_addr" ]; then
	echo "mkimage: nsects symbol not found in $mbr_elf" >&2
	exit 1
fi
mbr_tmp=$(mktemp)
trap 'rm -f "$esp_img" "$mbr_tmp"' EXIT HUP INT TERM
cp "$mbr" "$mbr_tmp"
python3 - "$mbr_tmp" "0x$nsects_addr" "$stage2_secs" <<'EOF'
import sys
path, addr, n = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
off = addr - 0x600
d = bytearray(open(path, 'rb').read())
if not 0 <= off < 438:
    raise SystemExit('mkimage: nsects at unexpected offset %d' % off)
d[off:off + 2] = n.to_bytes(2, 'little')
open(path, 'wb').write(d)
EOF
dd if="$mbr_tmp" of="$out" bs=1 count=440 conv=notrunc status=none

echo "$out: esp@$esp_start fnu-boot@$boot_start ($stage2_secs sectors) root@$root_start"
