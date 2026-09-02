#!/bin/sh
# Verify that a freshly built development image reaches its interactive shell.
set -eu

qemu=${1:?usage: smoke-qemu.sh QEMU KERNEL_IMAGE FS_IMAGE}
kernel=${2:?usage: smoke-qemu.sh QEMU KERNEL_IMAGE FS_IMAGE}
filesystem=${3:?usage: smoke-qemu.sh QEMU KERNEL_IMAGE FS_IMAGE}
log=$(mktemp "${TMPDIR:-/tmp}/proninx-qemu.XXXXXX")

cleanup() {
  rm -f "$log"
}
trap cleanup EXIT HUP INT TERM

set +e
timeout 12 "$qemu" -nographic -no-reboot \
  -drive "file=$kernel,index=0,media=disk,format=raw" \
  -drive "file=$filesystem,if=ide,index=1,media=disk,format=raw" \
  >"$log" 2>&1
status=$?
set -e

# timeout is expected for an interactive kernel; any other QEMU failure is not.
if [ "$status" -ne 124 ]; then
  cat "$log" >&2
  exit "$status"
fi
grep -F "FNU/PRONINX service supervisor" "$log" >/dev/null
grep -F "Welcome to FNU/PRONINX" "$log" >/dev/null
