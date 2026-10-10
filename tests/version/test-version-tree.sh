#!/bin/sh
# Usage: test-version-tree.sh SRC VERSION ERSSI
# Checks that the source tree and the built binary agree on the version.
SRC=$1
VERSION=$2
ERSSI=$3
fail=0

ok() { echo "ok - $1"; }
bad() { echo "FAIL - $1"; fail=1; }

err=$(sh "$SRC/utils/check-version.sh" "$SRC" 2>&1)
rc=$?
if [ "$rc" -eq 0 ] && [ -z "$err" ]; then
	ok "check-version.sh accepts the tree"
else
	echo "$err"
	bad "check-version.sh accepts the tree (exit $rc)"
fi

re=$(printf '%s' "$VERSION" | sed 's/\./\\./g')
out=$("$ERSSI" --version 2>&1)
if printf '%s\n' "$out" | grep -Eq "^erssi $re \\([0-9]{8} [0-9]{4}\\)\$"; then
	ok "--version output format"
else
	bad "--version output format: $out"
fi

field=$(printf '%s\n' "$out" | awk '{print $2}')
if [ "$field" = "$VERSION" ]; then
	ok "--version field 2 is the bare version"
else
	bad "--version field 2 is '$field', expected '$VERSION'"
fi

tag=$(head -n 1 "$SRC/NEWS" | cut -d' ' -f1)
if [ "$tag" = "shellter-v$VERSION" ]; then
	ok "NEWS first header is shellter-v$VERSION"
else
	bad "NEWS first header is '$tag'"
fi

if grep -Eq '^erssi-v1\.3\.([2-9]|10) ' "$SRC/NEWS"; then
	bad "fork headers in NEWS still use the erssi-v prefix"
else
	ok "fork headers in NEWS renamed"
fi

exit $fail
