#!/bin/sh
# guest-fnupkg-default.sh QEMU DISK_IMAGE
# Fresh boot: install nginx WITHOUT update; both repos in conf by default.
set -eu
qemu=${1:?usage: guest-fnupkg-default.sh QEMU DISK_IMAGE}
disk=${2:?usage: guest-fnupkg-default.sh QEMU DISK_IMAGE}
work=$(mktemp -d "${TMPDIR:-/tmp}/proninx-fnupkg-def.XXXXXX")
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
      tail -25 "$work/log" >&2
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

  # default conf has local + remote
  say "fnupkg repo list"
  wait_for "/usr/share/fnupkg/repo" 15 || ok=0
  wait_for "foundry-repo.522368.xyz" 15 || ok=0

  # install WITHOUT update — must work from baked local index
  say "fnupkg install nginx"
  wait_for "installed nginx-1.27.4" 60 || ok=0

  say "test -x /usr/sbin/nginx && echo NGINX_BIN_OK"
  wait_for "NGINX_BIN_OK" 15 || ok=0

  say "test -f /etc/nginx/nginx.conf && echo NGINX_CONF_OK"
  wait_for "NGINX_CONF_OK" 15 || ok=0

  # search without update
  say "fnupkg search tsort"
  wait_for "tsort-1.0.0" 15 || ok=0

  say "poweroff"
  wait_for "Powering system off" 60 || ok=0
else
  ok=0
fi
exec 3>&-
wait "$qemupid" 2>/dev/null || true

grep -F "installed nginx-1.27.4" "$work/log" >/dev/null || ok=0
grep -F "NGINX_BIN_OK" "$work/log" >/dev/null || ok=0
grep -F "foundry-repo.522368.xyz" "$work/log" >/dev/null || ok=0
if grep -E "panic|not in any repository|FAILED" "$work/log" >/dev/null; then
  echo "found error markers:" >&2
  grep -E "panic|not in any repository|FAILED" "$work/log" | head >&2
  ok=0
fi

if [ "$ok" -ne 1 ]; then
  echo "guest-fnupkg-default: FAILED" >&2
  tail -50 "$work/log" >&2
  exit 1
fi
echo "guest-fnupkg-default: ok"
