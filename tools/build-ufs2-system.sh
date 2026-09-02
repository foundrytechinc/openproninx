#!/bin/sh
# Build an FNU/PRONINX immutable UFS2 system image on the host. This is release
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

mkdir -p "$stage/bin" "$stage/etc" "$stage/usr" "$stage/var" "$stage/tmp"
install -m 0555 "$root/obj/user/init" "$stage/init"

for program in fnusvc fnu-health sh ls mkdir cat echo ln rm wc cp top ped fstest \
               preemptiontest1 preemptiontest2 vatest; do
  install -m 0555 "$root/obj/user/$program" "$stage/bin/$program"
done

# UFS2 system root: immutable; mutable service state belongs on FNU Data.
makefs -t ffs -o version=2,bsize=4096,fsize=512,label=FNU-SYSTEM \
  "$output" "$stage"
echo "FNU UFS2 system image created: $output"
