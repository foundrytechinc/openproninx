#!/bin/sh
# a tree as a UFS2 image: mkufs.sh MKIMG OUT SIZE_MB LABEL TREE
# makefs writes the filesystem; mkimg -S gives it the owners and modes the
# tree should have on the machine, which a build as an ordinary user cannot
set -eu

mkimg=${1:?usage: mkufs.sh MKIMG OUT SIZE_MB LABEL TREE}
out=${2:?}
size=${3:?}
label=${4:?}
tree=${5:?}

# Prefer the FreeBSD makefs from this tree: Debian's 2019 makefs puts the
# UFS2 superblock at 8192, but mkimg (and the kernel) expect it at 65536.
here=$(dirname "$0")
makefs=
for c in "$here/../makefs/src/usr.sbin/makefs/makefs" makefs; do
	if [ -x "$c" ] || command -v "$c" >/dev/null 2>&1; then
		makefs=$c
		break
	fi
done
if [ -z "$makefs" ]; then
	echo "mkufs: no makefs on the host; cannot make $out" >&2
	exit 1
fi

spec=$(mktemp)
trap 'rm -f "$spec"' EXIT HUP INT TERM
"$mkimg" -S "$tree" > "$spec"
"$makefs" -t ffs -s "${size}m" -o version=2,bsize=8192,fsize=1024 \
	-F "$spec" "$out" "$tree" > /dev/null
"$mkimg" -i "$out" -L "$label"
