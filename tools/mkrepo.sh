#!/bin/sh
# mkrepo.sh REPODIR
# Scans REPODIR for *.fpkg, writes REPODIR/index in fnupkg's pipe format:
#   name|version|arch|file|sha256|size|depends|desc
# Reads +META from each package with tar -xO.
set -eu
repo=${1:?usage: mkrepo.sh REPODIR}
[ -d "$repo" ] || { echo "mkrepo: $repo: not a directory" >&2; exit 1; }

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT HUP INT TERM
: > "$tmp"

for fpkg in "$repo"/*.fpkg; do
	[ -f "$fpkg" ] || continue
	base=$(basename "$fpkg")
	meta=$(tar -xOf "$fpkg" ./+META 2>/dev/null || tar -xOf "$fpkg" +META)
	get() { printf '%s\n' "$meta" | sed -n "s/^$1=//p" | head -1; }
	name=$(get name)
	ver=$(get version)
	arch=$(get arch)
	[ -n "$arch" ] || arch=any
	desc=$(get desc)
	[ -n "$desc" ] || desc=-
	deps=$(get depends)
	[ -n "$deps" ] || deps=-
	sha=$(sha256sum "$fpkg" | awk '{print $1}')
	size=$(wc -c < "$fpkg" | tr -d ' ')
	printf '%s|%s|%s|%s|%s|%s|%s|%s\n' \
		"$name" "$ver" "$arch" "$base" "$sha" "$size" "$deps" "$desc" >> "$tmp"
	echo "mkrepo: $name-$ver"
done

sort -t'|' -k1,1 -o "$repo/index" "$tmp"
# detached index digest for fnupkg pin: sha256=<hex> in fnupkg.conf
idx_sha=$(sha256sum "$repo/index" | awk '{print $1}')
printf '%s\n' "$idx_sha" > "$repo/index.sha256"
echo "mkrepo: wrote $repo/index ($(wc -l < "$repo/index") packages)"
echo "mkrepo: index sha256=$idx_sha"
