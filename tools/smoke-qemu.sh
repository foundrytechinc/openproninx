#!/bin/sh
# boot the image, log in, torture both filesystems, shut down; TCG and KVM
set -eu

qemu=${1:?usage: smoke-qemu.sh QEMU DISK_IMAGE}
disk=${2:?usage: smoke-qemu.sh QEMU DISK_IMAGE}
work=$(mktemp -d "${TMPDIR:-/tmp}/proninx-smoke.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

wait_for() {
  # $1 pattern, $2 seconds
  n=0
  until grep -q -- "$1" "$work/log" 2>/dev/null; do
    n=$((n + 1))
    if [ "$n" -ge $(($2 * 5)) ]; then
      echo "smoke: timed out waiting for: $1" >&2
      return 1
    fi
    sleep 0.2
  done
}

say() {
  printf '%s\n' "$1" >&3
  sleep 0.3
}

# $1: a name for the run, the rest: accelerator arguments
run() {
  name=$1
  shift
  cp "$disk" "$work/disk.img"
  : > "$work/log"
  rm -f "$work/in"
  mkfifo "$work/in"
  timeout -s KILL 600 "$qemu" -nographic -no-reboot "$@" \
    -drive "file=$work/disk.img,index=0,media=disk,format=raw" \
    -netdev user,id=proninx-net0 -device e1000,netdev=proninx-net0 \
    -smp cpus=2,sockets=2 \
    <"$work/in" >"$work/log" 2>&1 &
  qemupid=$!
  exec 3>"$work/in"
  ok=1
  if wait_for "OpenProninx login:" 120; then
    say root
    wait_for "assword" 30 && say root
    wait_for "em0: inet .*leased" 60 || ok=0
    say services
    wait_for "sshd  *running" 30 || ok=0
    say "/usr/tests/fstorture /tmp 8"
    wait_for "fstorture: /tmp: " 300 || ok=0
    say "mount /dev/wd0p1 /mnt && /usr/tests/fstorture /mnt 8 && umount /mnt"
    wait_for "fstorture: /mnt: " 300 || ok=0
    say "sync"
    say "poweroff"
    wait_for "Powering system off" 60 || ok=0
  else
    ok=0
  fi
  exec 3>&-
  wait "$qemupid" 2>/dev/null || true

  grep -E "sshd +running" "$work/log" >/dev/null || ok=0
  grep -E "^em0: at pci" "$work/log" >/dev/null || ok=0
  grep -F "fstorture: /tmp: ok" "$work/log" >/dev/null || ok=0
  grep -F "fstorture: /mnt: ok" "$work/log" >/dev/null || ok=0
  if grep -E "panic|FAILED|stray interrupt" "$work/log" >/dev/null; then
    ok=0
  fi
  if [ "$ok" -ne 1 ]; then
    echo "smoke ($name): FAILED" >&2
    tail -40 "$work/log" >&2
    return 1
  fi
  echo "smoke ($name): ok"
}

run tcg
if [ -w /dev/kvm ]; then
  run kvm -accel kvm -cpu host
fi
