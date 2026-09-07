#!/bin/sh
# clang-format over our own sources. the vendored trees stay as imported.
set -eu

sources() {
	find boot kern inc lib user \( -name '*.h' -o -name '*.c' \) -print
}

# crlf, lf, mixed or none
eol() {
	awk '{ if (substr($0, length($0)) == "\r") c++; else l++ }
	     END { if (c && l) print "mixed"
	           else if (c) print "crlf"
	           else if (l) print "lf"
	           else print "none" }' "$1"
}

skipped=
rc=0
for f in $(sources); do
	before=$(eol "$f")
	# clang-format would pick one ending and bury the real diff
	if [ "$before" = mixed ]; then
		skipped="$skipped $f"
		continue
	fi
	clang-format -i -style=file "$f"
	after=$(eol "$f")
	if [ "$after" != "$before" ]; then
		echo "format: $f: line endings $before -> $after" >&2
		rc=1
	fi
done

if [ -n "$skipped" ]; then
	echo "format: skipped, mixed line endings:" >&2
	for f in $skipped; do echo "	$f" >&2; done
fi
exit $rc
