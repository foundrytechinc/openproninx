#!/bin/sh
# mkfpkg.sh STAGEDIR OUT.fpkg
# STAGEDIR holds +META and payload files at their install paths (usr/bin/...).
# Produces a ustar archive that fnupkg can install.
set -eu
stage=${1:?usage: mkfpkg.sh STAGEDIR OUT.fpkg}
out=${2:?usage: mkfpkg.sh STAGEDIR OUT.fpkg}
[ -f "$stage/+META" ] || { echo "mkfpkg: $stage/+META missing" >&2; exit 1; }
# ustar keeps paths short enough; strip the leading ./
tar --format=ustar -cf "$out" -C "$stage" .
echo "mkfpkg: wrote $out"
