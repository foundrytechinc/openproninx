#!/bin/sh
# Build an FNU/OpenProninx immutable UFS2 system image on the host. This is release
# tooling, not part of the PRONINX kernel and not a filesystem implementation.
set -eu

output=${1:?usage: build-ufs2-system.sh OUTPUT.ufs}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
stage=$(mktemp -d "${TMPDIR:-/tmp}/fnu-system.XXXXXX")

cleanup() {
  rm -rf "$stage"
}
trap cleanup EXIT HUP INT TERM

if ! command -v makefs >/dev/null 2>&1; then
  echo "build-ufs2-system: makefs with FFS/UFS2 support is required" >&2
  echo "Use the Foundry release image or run this on the audited image-builder." >&2
  exit 1
fi

mkdir -p "$stage/etc" "$stage/mount" "$stage/dev" "$stage/tmp" "$stage/home" \
  "$stage/usr/bin" "$stage/var"
# The kernel intercepts this path and supplies the console device. Keeping a
# placeholder makes the device visible while listing the otherwise static
# UFS2 image.
: > "$stage/dev/console"
: > "$stage/etc/passwd"
install -m 0555 "$root/obj/user/init" "$stage/usr/bin/init"

for program in fnusvc fnu-health sh ls mkdir cat chmod chown echo ln rm wc cp top ped fstest \
               preemptiontest1 preemptiontest2 vatest login doas whoami users useradd \
               adduser passwd ping netinfo udpecho netconfig tcpecho resolve ntp syslog; do
  install -m 0555 "$root/obj/user/$program" "$stage/usr/bin/$program"
done

# UFS2 system root is read-only at runtime; mutable service state belongs on
# a separate FNU Data volume until the kernel has a crash-safe UFS2 writer.
makefs -t ffs -o version=2,bsize=4096,fsize=512,label=FNU-SYSTEM \
  "$output" "$stage"
echo "FNU UFS2 system image created: $output"
