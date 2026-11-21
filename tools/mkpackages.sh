#!/bin/sh
# mkpackages.sh INSTALL_DIR OUT_DIR BASE_LIST LINKS [REPO_ONLY_LIST]
# Builds one .fpkg per non-base binary in INSTALL_DIR, bundling its
# BIN_LINKS aliases as hardlinks.
#
# BASE_LIST      — stay in the image, not packages
# REPO_ONLY_LIST — packages only (not pre-installed, not pre-registered);
#                  fnupkg install pulls them from the repo
# everything else non-base — packaged AND pre-registered (in image, removable)
set -eu

indir=${1:?usage: mkpackages.sh INSTALL_DIR OUT_DIR BASE_LIST LINKS [REPO_ONLY]}
outdir=${2:?}
baselist=${3:?}
links=${4:-}
repolist=${5:-}

here=$(dirname "$0")
mkdir -p "$outdir/db/installed" "$outdir/db/files"

basef=$(mktemp)
repof=$(mktemp)
stage_root=$(mktemp -d)
revf=$(mktemp)
trap 'rm -f "$basef" "$repof" "$revf"; rm -rf "$stage_root"' EXIT HUP INT TERM

grep -v '^#' "$baselist" | grep -v '^[[:space:]]*$' | tr -d ' \t' > "$basef"
if [ -n "$repolist" ] && [ -f "$repolist" ]; then
	grep -v '^#' "$repolist" | grep -v '^[[:space:]]*$' | tr -d ' \t' > "$repof"
else
	: > "$repof"
fi

is_base() { grep -qx "$1" "$basef"; }
is_repo_only() { grep -qx "$1" "$repof"; }

# reverse links: "target alias" lines
: > "$revf"
for l in $links; do
	alias=${l%%=*}
	target=${l#*=}
	[ "$alias" != "$target" ] || continue
	printf '%s %s\n' "$target" "$alias" >> "$revf"
done

aliases_for() {
	awk -v t="$1" '$1 == t { print $2 }' "$revf"
}

emit_files() {
	dir="$1/$2"
	for entry in "$dir"/*; do
		[ -e "$entry" ] || [ -L "$entry" ] || continue
		base=$(basename "$entry")
		if [ -n "$2" ]; then
			rel="$2/$base"
		else
			rel="$base"
		fi
		case "$rel" in
		+*) continue ;;
		esac
		if [ -L "$entry" ]; then
			target=$(readlink "$entry")
			printf -- '-  %s -> %s\n' "$rel" "$target"
		elif [ -d "$entry" ]; then
			emit_files "$1" "$rel"
		elif [ -f "$entry" ]; then
			sha=$(sha256sum "$entry" | awk '{print $1}')
			printf '%s  %s\n' "$sha" "$rel"
		fi
	done
}

n=0
npre=0
for src in "$indir"/*; do
	[ -f "$src" ] || continue
	[ -x "$src" ] || continue
	name=$(basename "$src")
	is_base "$name" && continue
	# nginx/joe get a full package (configs) from mkportpkg.sh
	case "$name" in nginx|joe) continue ;; esac

	stage="$stage_root/$name"
	rm -rf "$stage"
	mkdir -p "$stage/usr/bin"
	cp "$src" "$stage/usr/bin/$name"
	chmod 0555 "$stage/usr/bin/$name"

	for a in $(aliases_for "$name"); do
		ln -f "$stage/usr/bin/$name" "$stage/usr/bin/$a"
	done

cat > "$stage/+META" <<EOF
name=$name
version=1.0.0
arch=amd64
desc=$name (FNU)
depends=
EOF

	sh "$here/mkfpkg.sh" "$stage" "$outdir/$name-1.0.0.fpkg" >/dev/null
	n=$((n + 1))

	# pre-register only for tools that ship installed-by-default
	if ! is_repo_only "$name"; then
		cp "$stage/+META" "$outdir/db/installed/$name"
		emit_files "$stage" "" > "$outdir/db/files/$name"
		npre=$((npre + 1))
	fi
done

# demo packages from the source tree (not pre-installed)
for f in "$here"/../share/fnupkg/repo/*.fpkg; do
	[ -f "$f" ] || continue
	cp "$f" "$outdir/"
done

sh "$here/mkrepo.sh" "$outdir"
echo "mkpackages: $n packages ($npre pre-registered, rest repo-only) in $outdir"
