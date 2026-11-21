#!/bin/sh
# guest-fnupkg-remote.sh QEMU DISK_IMAGE — boot, fnupkg update from remote, install one pkg
set -eu
qemu=${1:?usage: guest-fnupkg-remote.sh QEMU DISK_IMAGE}
disk=${2:?usage: guest-fnupkg-remote.sh QEMU DISK_IMAGE}
work=$(mktemp -d "${TMPDIR:-/tmp}/proninx-fnupkg-remote.XXXXXX")
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
      echo "timeout waiting for: $1" >&2
      tail -30 "$work/log" >&2
      return 1
    fi
    sleep 0.2
  done
}

say() {
  printf '%s\n' "$1" >&3
  sleep 0.25
}

PIN=0489909e96e2bdcc190fb6894cdbc97c6eac2be960619258e2480fbabcc6e364

timeout -s KILL 420 "$qemu" -nographic -no-reboot \
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
  # wait for DHCP so DNS/routing works
  wait_for "em0: inet .*leased" 60 || ok=0

  # point conf at remote only (comment out local) + pin
  say "printf '%s\\n' '# remote only test' 'http://foundry-repo.522368.xyz sha256=${PIN}' > /etc/fnupkg.conf"
  say "fnupkg repo list"
  wait_for "foundry-repo.522368.xyz" 15 || ok=0
  wait_for "pin=sha256:${PIN}" 15 || ok=0

  say "fnupkg update"
  wait_for "updated repo 0 (http://foundry-repo.522368.xyz) index=" 45 || ok=0

  # pin must match — wrong pin would fail update; also search remote index
  say "fnupkg search nginx"
  wait_for "nginx-1.27.4" 15 || ok=0

  say "fnupkg install tsort"
  wait_for "installed tsort-1.0.0" 60 || ok=0

  say "tsort <<EOF"
  say "b a"
  say "a c"
  say "c"
  say "EOF"
  wait_for "^b$" 10 || true

  say "poweroff"
  wait_for "Powering system off" 60 || ok=0
else
  ok=0
fi
exec 3>&-
wait "$qemupid" 2>/dev/null || true

# assertions
grep -F "updated repo 0 (http://foundry-repo.522368.xyz)" "$work/log" >/dev/null || ok=0
grep -F "installed tsort-1.0.0" "$work/log" >/dev/null || ok=0
grep -F "nginx-1.27.4" "$work/log" >/dev/null || ok=0
if grep -E "panic|sha256 mismatch|index sha256 mismatch|FAILED" "$work/log" >/dev/null; then
  echo "guest-fnupkg-remote: found error markers" >&2
  grep -E "panic|sha256 mismatch|index sha256 mismatch|FAILED|warnx" "$work/log" | head -20 >&2
  ok=0
fi

if [ "$ok" -ne 1 ]; then
  echo "guest-fnupkg-remote: FAILED" >&2
  tail -50 "$work/log" >&2
  exit 1
fi
echo "guest-fnupkg-remote: ok"
