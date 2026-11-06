#!/bin/sh
# utils/build-deb.sh: the Debian package of a released binary package.
#
# Usage:
#   build-deb.sh [--out DIR] [--signers FILE] TARBALL
#       TARBALL is erssi-<version>-<platform>.tar.xz of a release (built,
#       reproduced and signed as docs/shellter/releasing.md says) for
#       debian12-amd64, debian13-amd64 or ubuntu24-amd64, with the
#       release's SHA256SUMS and SHA256SUMS.sig next to it. Writes
#       erssi_<version>-1~<suite>_amd64.deb into DIR (default: the current
#       directory) and prints its name and sha256.
#
# Only a tarball the release signed is packed: SHA256SUMS.sig must be a
# signature of SHA256SUMS by a key of FILE (allowed_signers format;
# default: allowed_signers next to this script, the release key) in the
# namespace erssi-release, and the tarball's sum must be the one there.
# All three are copied to a private directory first and the copies are
# checked and packed: the files next to TARBALL may change meanwhile.
#
# The .deb holds exactly the files of the tarball, under /opt/erssi (the
# tarball is relocatable: erssi finds its modules and scripts next to
# its binary), with /usr/bin/erssi a link to /opt/erssi/bin/erssi. Debian's
# own irssi lives in /usr/lib/irssi and /usr/share/irssi: nothing of it
# is touched. The package depends on the libraries the tarball's MANIFEST
# names (its needed= line), by the package names of that release.
#
# Nothing is built: the .deb is the tarball packed again, the same bytes
# every time (its files, owner root, dated to the release's commit as the
# tarball is), so anyone can make it from the published tarball and
# compare. It needs tar, xz and dpkg-deb (Debian, Ubuntu).
#
# /opt/erssi/share/irssi/packaged-by says "apt": erssi --update leaves an
# install that the package manager keeps alone.
#
# Exit: 0 written, 1 refused or failed, 2 usage.
set -u

say() { printf 'build-deb: %s\n' "$*"; }
die() { printf 'build-deb: %s\n' "$*" >&2; exit 1; }
usage() { sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

OUT=.
SIGNERS=$(dirname -- "$0")/allowed_signers
while [ $# -gt 0 ]; do
    case $1 in
        --out) [ $# -ge 2 ] || usage; OUT=$2; shift 2 ;;
        --signers) [ $# -ge 2 ] || usage; SIGNERS=$2; shift 2 ;;
        -h|--help) usage ;;
        --) shift; break ;;
        -*) usage ;;
        *) break ;;
    esac
done
[ $# = 1 ] || usage
TARBALL=$1
for t in tar xz dpkg-deb sha256sum find sort ssh-keygen; do
    command -v "$t" > /dev/null 2>&1 || die "$t is missing"
done
[ -f "$TARBALL" ] || die "$TARBALL: no such file"
[ -d "$OUT" ] || die "$OUT: no such directory"

base=$(basename -- "$TARBALL")
case $base in
    erssi-*-*-amd64.tar.xz) ;;
    *) die "$base: not erssi-<version>-<platform>.tar.xz" ;;
esac
top=${base%.tar.xz}
VERSION=${top#erssi-}
PLATFORM=${VERSION#*-}
VERSION=${VERSION%%-*}
case $VERSION in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) die "$base: no version in the name" ;;
esac
case $VERSION in *[!0-9.]*) die "$base: no version in the name" ;; esac

# the suite's tag in the version (the newer release sorts after the
# older: ~deb12 < ~deb13 < ~ubuntu24.04, and all before an official -1)
# and the package of each library erssi links
case $PLATFORM in
    debian12-amd64) SUITE=deb12
        LIBS='libc.so.6=libc6 libm.so.6=libc6 ld-linux-x86-64.so.2=libc6
              libglib-2.0.so.0=libglib2.0-0 libgio-2.0.so.0=libglib2.0-0
              libgmodule-2.0.so.0=libglib2.0-0 libgobject-2.0.so.0=libglib2.0-0
              libssl.so.3=libssl3 libcrypto.so.3=libssl3 libcurl.so.4=libcurl4
              libotr.so.5=libotr5 libgcrypt.so.20=libgcrypt20 libchafa.so.0=libchafa0
              libutf8proc.so.2=libutf8proc2 libperl.so.5.36=libperl5.36' ;;
    debian13-amd64) SUITE=deb13
        LIBS='libc.so.6=libc6 libm.so.6=libc6 ld-linux-x86-64.so.2=libc6
              libglib-2.0.so.0=libglib2.0-0t64 libgio-2.0.so.0=libglib2.0-0t64
              libgmodule-2.0.so.0=libglib2.0-0t64 libgobject-2.0.so.0=libglib2.0-0t64
              libssl.so.3=libssl3t64 libcrypto.so.3=libssl3t64 libcurl.so.4=libcurl4t64
              libotr.so.5=libotr5t64 libgcrypt.so.20=libgcrypt20 libchafa.so.0=libchafa0t64
              libutf8proc.so.3=libutf8proc3 libperl.so.5.40=libperl5.40' ;;
    ubuntu24-amd64) SUITE=ubuntu24.04
        LIBS='libc.so.6=libc6 libm.so.6=libc6 ld-linux-x86-64.so.2=libc6
              libglib-2.0.so.0=libglib2.0-0t64 libgio-2.0.so.0=libglib2.0-0t64
              libgmodule-2.0.so.0=libglib2.0-0t64 libgobject-2.0.so.0=libglib2.0-0t64
              libssl.so.3=libssl3t64 libcrypto.so.3=libssl3t64 libcurl.so.4=libcurl4t64
              libotr.so.5=libotr5t64 libgcrypt.so.20=libgcrypt20 libchafa.so.0=libchafa0t64
              libutf8proc.so.3=libutf8proc3 libperl.so.5.38=libperl5.38t64' ;;
    *) die "$PLATFORM: no Debian package for this platform (debian12-amd64, debian13-amd64, ubuntu24-amd64)" ;;
esac
DEBVER=$VERSION-1~$SUITE

WORK=$(mktemp -d "${TMPDIR:-/tmp}/build-deb.XXXXXX") || die "cannot make a temporary directory"
trap 'rm -rf "$WORK"' EXIT
trap 'exit 1' HUP INT TERM

# the release's signature over the sums, and the tarball's sum there - of
# private copies, which are what is packed
dir=$(dirname -- "$TARBALL")
[ -f "$dir/SHA256SUMS" ] && [ -f "$dir/SHA256SUMS.sig" ] ||
    die "no SHA256SUMS and SHA256SUMS.sig of the release next to $base"
[ -r "$SIGNERS" ] || die "cannot read the signers file $SIGNERS"
cp "$TARBALL" "$WORK/$base" && cp "$dir/SHA256SUMS" "$WORK/SHA256SUMS" &&
    cp "$dir/SHA256SUMS.sig" "$WORK/SHA256SUMS.sig" || die "cannot copy $base and its sums"
awk '/^[ \t]*(#|$)/ { next }
    { k = ($2 ~ /^(ssh-|ecdsa-|sk-)/) ? 2 : 3
      if ($k == "" || $(k + 1) == "") next
      printf "%s namespaces=\"erssi-release\" %s %s\n", $1, $k, $(k + 1) }' "$SIGNERS" > "$WORK/signers"
[ -s "$WORK/signers" ] || die "no key in the signers file $SIGNERS"
good=0
# shellcheck disable=SC2013 # principals: words without blanks
for who in $(awk '{print $1}' "$WORK/signers" | sort -u); do
    if ssh-keygen -Y verify -f "$WORK/signers" -I "$who" -n erssi-release \
            -s "$WORK/SHA256SUMS.sig" < "$WORK/SHA256SUMS" > /dev/null 2>&1; then
        good=1
        break
    fi
done
[ "$good" = 1 ] || die "SHA256SUMS.sig is not a signature of SHA256SUMS by a key of $SIGNERS (namespace erssi-release) - not packed"
want=$(awk -v f="$base" '$2 == f && NF == 2 {print $1}' "$WORK/SHA256SUMS")
[ -n "$want" ] || die "$base is not in the release's SHA256SUMS - not packed"
[ "$(sha256sum < "$WORK/$base" | awk '{print $1}')" = "$want" ] ||
    die "$base does not match its sum in the release's SHA256SUMS - not packed"

# the tarball's own checks (no path outside its top directory, no links
# out of it) were made when it was built and reproduced; here only its
# shape: one top directory of its name with a MANIFEST of this version
mkdir "$WORK/x" || exit 1
tar -xJf "$WORK/$base" -C "$WORK/x" --no-same-owner || die "$base: cannot unpack"
[ "$(ls -A "$WORK/x")" = "$top" ] || die "$base: not one directory $top/ inside"
T=$WORK/x/$top
[ -f "$T/MANIFEST" ] && [ -x "$T/bin/erssi" ] || die "$base: no MANIFEST or bin/erssi"
# its installer (what erssi --update runs) must leave a package alone:
# the releases before 2026.11.3 do not know packaged-by, and run as root
# they would replace what dpkg keeps
grep -q 'share/irssi/packaged-by' "$T/share/irssi/shellter-install.sh" 2>/dev/null ||
    die "$base: its installer does not know packaged-by (a release before 2026.11.3) - erssi --update would change what apt installed; package a newer release"
field() { sed -n "s/^$1=//p" "$T/MANIFEST" | head -n 1; }
[ "$(field version)" = "$VERSION" ] || die "$base: its MANIFEST is of version $(field version)"
[ "$(field platform)" = "$PLATFORM" ] || die "$base: its MANIFEST is of platform $(field platform)"
needed=$(field needed)
[ -n "$needed" ] || die "$base: its MANIFEST names no libraries (needed=)"
libc=$(field libc)
case $libc in "glibc "[0-9]*) ;; *) die "$base: its MANIFEST names no glibc version (libc=)" ;; esac
libc=${libc#glibc }

# Depends: each library's package once, libc6 at least the glibc it was
# built with; a library the table does not know is refused (a package
# that installs and then cannot start helps nobody)
deps=""
for so in $needed; do
    pkg=""
    for m in $LIBS; do
        [ "${m%%=*}" = "$so" ] && pkg=${m#*=}
    done
    [ -n "$pkg" ] || die "$base: no package known for $so on $PLATFORM - add it to the table in build-deb.sh"
    [ "$pkg" = libc6 ] && continue
    case " $deps " in *" $pkg "*) ;; *) deps="$deps $pkg" ;; esac
done
DEPENDS="libc6 (>= $libc)"
# shellcheck disable=SC2086 # the words of deps, one a line
for p in $(printf '%s\n' $deps | LC_ALL=C sort); do DEPENDS="$DEPENDS, $p"; done

# dated as the tarball's files are: the release's commit
EPOCH=$(date -r "$T/MANIFEST" +%s 2> /dev/null) || die "cannot read the date of the MANIFEST"
SOURCE_DATE_EPOCH=$EPOCH
export SOURCE_DATE_EPOCH

R=$WORK/root
mkdir -p "$R/opt" "$R/usr/bin" "$R/usr/share/doc/erssi" "$R/DEBIAN" || exit 1
mv "$T" "$R/opt/erssi" || exit 1
printf 'apt\n' > "$R/opt/erssi/share/irssi/packaged-by" || die "no share/irssi in $base"
ln -s /opt/erssi/bin/erssi "$R/usr/bin/erssi"
cat > "$R/usr/share/doc/erssi/copyright" << 'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: erssi Shellter Edition
Source: https://github.com/y-o-o-z/erssi

Files: *
Copyright: the irssi project, the erssi project (kofany), the Shellter team
License: GPL-2+
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 .
 On Debian systems, the complete text of the GNU General Public License
 version 2 can be found in /usr/share/common-licenses/GPL-2.
EOF
# in KiB, as dpkg-gencontrol counts it: each file's size rounded up, one
# for each directory and link - not the blocks of the file system it is
# built on (tmpfs and ext4 give other numbers)
size=$(find "$R/opt" "$R/usr" \( -type f -printf '%s\n' \) -o \( \( -type d -o -type l \) -printf '0\n' \) |
    awk '{s += ($1 == 0) ? 1 : int(($1 + 1023) / 1024)} END {print s}')
cat > "$R/DEBIAN/control" << EOF
Package: erssi
Version: $DEBVER
Architecture: amd64
Maintainer: the Shellter team <136268860+y-o-o-z@users.noreply.github.com>
Installed-Size: $size
Depends: $DEPENDS
Section: net
Priority: optional
Homepage: https://erssi.shellter.me
Description: IRC client - erssi Shellter Edition
 erssi, an irssi-based terminal IRC client with side panels, mouse
 support, native end-to-end encryption (RPE2E), a web frontend server,
 image previews and automatic backups, in the Shellter Edition: ready
 for shell boxes, with its own defaults and theme.
 .
 Installed in /opt/erssi, /usr/bin/erssi starts it.
EOF
(cd "$R" && find opt usr -type f | LC_ALL=C sort | xargs md5sum) > "$R/DEBIAN/md5sums" || exit 1
# the same modes and dates every time, whatever the umask of who builds
# (tar takes it off the modes it unpacks): directories and programs 755,
# every other file 644 - a system install every user may run
find "$R" -type d -exec chmod 755 {} +
find "$R" -type f -perm -u+x -exec chmod 755 {} +
find "$R" -type f ! -perm -u+x -exec chmod 644 {} +
find "$R" -exec touch -h -d "@$EPOCH" {} +

NAME=erssi_${DEBVER}_amd64.deb
dpkg-deb --root-owner-group -Zxz -z9 --build "$R" "$WORK/$NAME" > "$WORK/dpkg-deb.log" 2>&1 ||
    die "dpkg-deb failed: $(tail -n 1 "$WORK/dpkg-deb.log")"
mv "$WORK/$NAME" "$OUT/$NAME" || die "cannot write $OUT/$NAME"
say "$NAME, sha256 $(sha256sum < "$OUT/$NAME" | awk '{print $1}')"
