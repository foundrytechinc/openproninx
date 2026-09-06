#!/bin/sh
# Verify that a freshly built development image reaches its interactive shell.
set -eu

qemu=${1:?usage: smoke-qemu.sh QEMU KERNEL_IMAGE [FS_IMAGE]}
kernel=${2:?usage: smoke-qemu.sh QEMU KERNEL_IMAGE [FS_IMAGE]}
filesystem=${3:-}
log=$(mktemp "${TMPDIR:-/tmp}/proninx-qemu.XXXXXX")

cleanup() {
  rm -f "$log"
}
trap cleanup EXIT HUP INT TERM

set +e
printf 'root\n' | timeout -s KILL 10 "$qemu" -nographic -no-reboot \
  -drive "file=$kernel,index=0,media=disk,format=raw" \
  -netdev user,id=proninx-net0 -device e1000,netdev=proninx-net0 \
  -smp cpus=2,sockets=2 \
  >"$log" 2>&1
status=$?
set -e

# timeout (124 or 137 on SIGKILL) is expected for an interactive kernel
if [ "$status" -ne 124 ] && [ "$status" -ne 137 ]; then
  cat "$log" >&2
  exit "$status"
fi
grep -F "FNU/OpenProninx service supervisor" "$log" >/dev/null
grep -E "NET: (e1000|virtio-net|interface)" "$log" >/dev/null
grep -F "requesting DHCP lease" "$log" >/dev/null
grep -F "OpenProninx login:" "$log" >/dev/null
