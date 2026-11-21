#!/bin/sh
# mkportpkg.sh kind OUT_DIR — build a full package for a ported tool
# kind: nginx | joe
set -eu
kind=${1:?usage: mkportpkg.sh nginx|joe OUT_DIR}
outdir=${2:?}
here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir -p "$outdir"

case "$kind" in
nginx)
	[ -f "$root/obj/install/nginx" ] || { echo "mkportpkg: build nginx first" >&2; exit 1; }
	mkdir -p "$stage/usr/sbin" "$stage/etc/nginx" "$stage/usr/share/nginx/html" "$stage/etc/rc.d"
	cp "$root/obj/install/nginx" "$stage/usr/sbin/nginx"
	chmod 0555 "$stage/usr/sbin/nginx"
	cp "$root/etc/nginx/nginx.conf" "$root/etc/nginx/mime.types" "$stage/etc/nginx/"
	cp "$root/share/nginx/html/index.html" "$stage/usr/share/nginx/html/"
	cp "$root/etc/rc.d/nginx" "$stage/etc/rc.d/"
	cat > "$stage/+META" <<'META'
name=nginx
version=1.27.4
arch=any
desc=nginx web server for OpenProninx (static; no TLS/PCRE)
depends=
META
	sh "$here/mkfpkg.sh" "$stage" "$outdir/nginx-1.27.4.fpkg" >/dev/null
	echo "mkportpkg: nginx-1.27.4.fpkg"
	;;
joe)
	[ -f "$root/obj/install/joe" ] || { echo "mkportpkg: build joe first" >&2; exit 1; }
	mkdir -p "$stage/usr/bin" "$stage/etc/joe" \
		"$stage/usr/share/joe/syntax" "$stage/usr/share/joe/colors"
	cp "$root/obj/install/joe" "$stage/usr/bin/joe"
	chmod 0555 "$stage/usr/bin/joe"
	cp "$root/etc/joe/"* "$stage/etc/joe/"
	# syntax/colors only if present
	if [ -d "$root/third_party/src/joe-releases-joe-4.8/syntax" ]; then
		cp "$root/third_party/src/joe-releases-joe-4.8/syntax/"* "$stage/usr/share/joe/syntax/" 2>/dev/null || true
	fi
	if [ -d "$root/third_party/src/joe-releases-joe-4.8/colors" ]; then
		cp "$root/third_party/src/joe-releases-joe-4.8/colors/"* "$stage/usr/share/joe/colors/" 2>/dev/null || true
	fi
	cat > "$stage/+META" <<'META'
name=joe
version=4.8
arch=any
desc=Joe's Own Editor
depends=
META
	sh "$here/mkfpkg.sh" "$stage" "$outdir/joe-4.8.fpkg" >/dev/null
	echo "mkportpkg: joe-4.8.fpkg"
	;;
*)
	echo "mkportpkg: unknown kind $kind" >&2
	exit 2
	;;
esac
