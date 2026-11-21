#!/bin/sh
# guest-fnupkg-pin.sh QEMU DISK_IMAGE
# local update (no pin) + http update without sha256 + file:// path
set -eu
qemu=${1:?usage: guest-fnupkg-pin.sh QEMU DISK_IMAGE}
disk=${2:?usage: guest-fnupkg-pin.sh QEMU DISK_IMAGE}
work=$(mktemp -d "${TMPDIR:-/tmp}/proninx-fnupkg-pin.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP TERM
cp "$disk" "$work/disk.img"
: > "$work/log"
rm -f "$work/in"
mkfifo "$work/in"

wait_for() {
  n=0
  until grep -q -- "$1" "$work/log" 2>/dev/null; do
    n=$((n + 1))
    if [ "$n" -ge $(($2 * 5)) ]; then
      echo "timeout: $1" >&2
      tail -20 "$work/log" >&2
      return 1
    fi
    sleep 0.2
  done
}
say() { printf '%s\n' "$1" >&3; sleep 0.25; }

timeout -s KILL 360 "$qemu" -nographic -no-reboot \
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

  # 1) default local repo, no pin
  say "fnupkg repo list"
  wait_for "(local)" 10 || ok=0
  say "fnupkg update"
  wait_for "updated repo 0 (/usr/share/fnupkg/repo) index=.* (no pin)" 30 || ok=0

  # 2) http without pin — must work
  say "printf '%s\\n' 'http://foundry-repo.522368.xyz' > /etc/fnupkg.conf"
  say "fnupkg repo list"
  wait_for "no pin — optional for http://" 15 || ok=0
  say "fnupkg update"
  wait_for "updated repo 0 (http://foundry-repo.522368.xyz) index=.* (no pin)" 45 || ok=0

  # 3) file:// without pin
  say "printf '%s\\n' 'file:///usr/share/fnupkg/repo' > /etc/fnupkg.conf"
  say "fnupkg update"
  wait_for "updated repo 0 (/usr/share/fnupkg/repo) index=.* (no pin)" 30 || ok=0

  # 4) http WITH pin still verifies
  say "printf '%s\\n' 'http://foundry-repo.522368.xyz sha256=0489909e96e2bdcc190fb6894cdbc97c6eac2be960619258e2480fbabcc6e364' > /etc/fnupkg.conf"
  say "fnupkg update"
  wait_for "(pin ok)" 45 || ok=0

  # 5) wrong pin must fail
  say "printf '%s\\n' 'http://foundry-repo.522368.xyz sha256=0000000000000000000000000000000000000000000000000000000000000000' > /etc/fnupkg.conf"
  say "fnupkg update; echo PINFAIL_RC=\$?"
  wait_for "index sha256 mismatch" 30 || ok=0
  wait_for "PINFAIL_RC=1" 15 || ok=0

  say "poweroff"
  wait_for "Powering system off" 60 || ok=0
else
  ok=0
fi
exec 3>&-
wait "$qemupid" 2>/dev/null || true

grep -F "(no pin)" "$work/log" >/dev/null || ok=0
grep -F "(pin ok)" "$work/log" >/dev/null || ok=0
grep -F "index sha256 mismatch" "$work/log" >/dev/null || ok=0
grep -F "file:///usr/share/fnupkg/repo" "$work/log" >/dev/null || ok=0
if grep -E "panic|FAILED" "$work/log" >/dev/null; then
  ok=0
fi

if [ "$ok" -ne 1 ]; then
  echo "guest-fnupkg-pin: FAILED" >&2
  tail -60 "$work/log" >&2
  exit 1
fi
echo "guest-fnupkg-pin: ok"
