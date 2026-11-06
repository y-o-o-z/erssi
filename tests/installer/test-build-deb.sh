#!/bin/sh
# utils/build-deb.sh and utils/apt-repo.sh on made-up release tarballs,
# with a throwaway OpenPGP key: no network, no root, no chroot.
# Usage: test-build-deb.sh SOURCE_ROOT
# Skipped (77) without dpkg-deb, apt-ftparchive, gpg or gpgv.
SRC=$1
FAILED=0
ok() { echo "ok - $1"; }
bad() { echo "FAIL - $1"; FAILED=1; }
check() {   # description command...
    d=$1; shift
    if "$@"; then ok "$d"; else bad "$d"; fi
}
for t in dpkg-deb apt-ftparchive gpg gpgv xz tar ssh-keygen flock; do
    command -v "$t" > /dev/null 2>&1 || { echo "skip: no $t"; exit 77; }
done
W=$(mktemp -d) && W=$(cd -P "$W" && pwd -P) || exit 1
trap 'rm -rf "$W"' EXIT
trap 'exit 1' HUP INT TERM
# a throwaway release key: SHA256SUMS of the tarballs in $W, signed
ssh-keygen -q -t ed25519 -N '' -C test -f "$W/relkey" || exit 1
printf 'test@example.invalid %s\n' "$(cut -d' ' -f1,2 "$W/relkey.pub")" > "$W/signers"
sign_sums() {
    (cd "$W" && sha256sum erssi-*.tar.xz | LC_ALL=C sort -k 2 > SHA256SUMS) || exit 1
    rm -f "$W/SHA256SUMS.sig"
    ssh-keygen -q -Y sign -n erssi-release -f "$W/relkey" "$W/SHA256SUMS" || exit 1
}
BD="sh $SRC/utils/build-deb.sh --signers $W/signers"

# erssi-$1-$2.tar.xz as a release packs it: one top directory, a
# MANIFEST naming the libraries ($3: needed=), dated to a commit
tarball() {   # version platform needed [glibc]
    t=erssi-$1-$2
    rm -rf "$W/t" && mkdir -p "$W/t/$t/bin" "$W/t/$t/share/irssi" "$W/t/$t/lib/irssi/modules" || exit 1
    printf '#!/bin/sh\necho "erssi %s (20261103 1102)"\n' "$1" > "$W/t/$t/bin/erssi"
    chmod 750 "$W/t/$t/bin/erssi"
    printf 'theme\n' > "$W/t/$t/share/irssi/default.theme"
    chmod 640 "$W/t/$t/share/irssi/default.theme"
    printf 'version=%s\nplatform=%s\nlibc=glibc %s\nneeded=%s\n' "$1" "$2" "${4:-2.41}" "$3" > "$W/t/$t/MANIFEST"
    find "$W/t/$t" -exec touch -h -d @1793700159 {} +
    (cd "$W/t" && tar --sort=name --owner=0 --group=0 --numeric-owner -cJf "$W/$t.tar.xz" "$t") || exit 1
    sign_sums
}
NEEDED13='ld-linux-x86-64.so.2 libc.so.6 libcrypto.so.3 libglib-2.0.so.0 libm.so.6 libperl.so.5.40 libssl.so.3 libutf8proc.so.3'

# --- the package ---
tarball 2026.11.3 debian13-amd64 "$NEEDED13"
mkdir -p "$W/o1" "$W/o2" "$W/o3"
(umask 022; $BD --out "$W/o1" "$W/erssi-2026.11.3-debian13-amd64.tar.xz") > "$W/out" 2>&1
check "a .deb is made" [ -f "$W/o1/erssi_2026.11.3-1~deb13_amd64.deb" ]
D=$W/o1/erssi_2026.11.3-1~deb13_amd64.deb
check "... its sha256 said" grep -q "erssi_2026.11.3-1~deb13_amd64.deb, sha256 [0-9a-f]\{64\}" "$W/out"
(umask 000; $BD --out "$W/o2" "$W/erssi-2026.11.3-debian13-amd64.tar.xz") > /dev/null 2>&1
(umask 077; $BD --out "$W/o3" "$W/erssi-2026.11.3-debian13-amd64.tar.xz") > /dev/null 2>&1
check "the same bytes under another umask (000)" cmp -s "$D" "$W/o2/erssi_2026.11.3-1~deb13_amd64.deb"
check "... and 077" cmp -s "$D" "$W/o3/erssi_2026.11.3-1~deb13_amd64.deb"
field() { dpkg-deb -f "$D" "$1"; }
check "Package erssi, version 2026.11.3-1~deb13" [ "$(field Package) $(field Version)" = "erssi 2026.11.3-1~deb13" ]
check "Depends: each library's package once, sorted, libc6 at least the glibc built with" \
    [ "$(field Depends)" = "libc6 (>= 2.41), libglib2.0-0t64, libperl5.40, libssl3t64, libutf8proc3" ]
dpkg-deb -c "$D" > "$W/list"
check "the files under /opt/erssi" grep -q ' ./opt/erssi/share/irssi/default.theme$' "$W/list"
check "/usr/bin/erssi links to /opt/erssi/bin/erssi" grep -q ' ./usr/bin/erssi -> /opt/erssi/bin/erssi$' "$W/list"
check "the program 755 (it was 750)" grep -q '^-rwxr-xr-x root/root .* ./opt/erssi/bin/erssi$' "$W/list"
check "other files 644 (it was 640)" grep -q '^-rw-r--r-- root/root .* ./opt/erssi/share/irssi/default.theme$' "$W/list"
check "packaged-by says apt" sh -c 'dpkg-deb --fsys-tarfile "$1" | tar -xOf - ./opt/erssi/share/irssi/packaged-by | grep -qx apt' sh "$D"
check "dated to the commit" sh -c '! grep -v " 2026-11-03 " "$1" | grep -q .' sh "$W/list"
check "nothing outside /opt/erssi and /usr" sh -c '! awk "{print \$NF}" "$1" | grep -v -e "^./$" -e "^./opt/" -e "^./usr/" -e "^/opt/erssi/bin/erssi$" | grep -q .' sh "$W/list"
for p in debian12-amd64:deb12:'libc6 (>= 2.36), libglib2.0-0, libperl5.36, libssl3' \
         ubuntu24-amd64:ubuntu24.04:'libc6 (>= 2.39), libglib2.0-0t64, libperl5.38t64, libssl3t64'; do
    plat=${p%%:*} rest=${p#*:}
    tag=${rest%%:*} want=${rest#*:}
    case $plat in
        debian12-amd64) n='libc.so.6 libglib-2.0.so.0 libperl.so.5.36 libssl.so.3' g=2.36 ;;
        *) n='libc.so.6 libglib-2.0.so.0 libperl.so.5.38 libssl.so.3' g=2.39 ;;
    esac
    tarball 2026.11.3 "$plat" "$n" "$g"
    $BD --out "$W/o1" "$W/erssi-2026.11.3-$plat.tar.xz" > /dev/null 2>&1
    f=$W/o1/erssi_2026.11.3-1~${tag}_amd64.deb
    check "$plat: ~$tag, its own package names" sh -c '[ -f "$1" ] && [ "$(dpkg-deb -f "$1" Depends)" = "$2" ]' sh "$f" "$want"
done

# --- refused ---
refused() {   # description pattern tarball
    $BD --out "$W/o1" "$3" > "$W/out" 2>&1
    rc=$?
    if [ "$rc" = 1 ] && grep -qF -- "$2" "$W/out"; then ok "$1"; else bad "$1 (exit $rc): $(cat "$W/out")"; fi
}
tarball 2026.11.3 debian13-amd64 "$NEEDED13 libnew.so.1"
refused "a library the table does not know" "no package known for libnew.so.1 on debian13-amd64" "$W/erssi-2026.11.3-debian13-amd64.tar.xz"
tarball 2026.11.3 freebsd15-amd64 "libc.so.7"
refused "a platform without Debian packages" "freebsd15-amd64: no Debian package" "$W/erssi-2026.11.3-freebsd15-amd64.tar.xz"
tarball 2026.11.3 debian13-amd64 "$NEEDED13"
cp "$W/erssi-2026.11.3-debian13-amd64.tar.xz" "$W/erssi-2026.11.4-debian13-amd64.tar.xz"
sign_sums
refused "a name of another version than its directory's" "not one directory erssi-2026.11.4-debian13-amd64/ inside" "$W/erssi-2026.11.4-debian13-amd64.tar.xz"
# its directory renamed too: the MANIFEST still says the old version
(cd "$W/t" && mv erssi-2026.11.3-debian13-amd64 erssi-2026.11.4-debian13-amd64 &&
    tar --sort=name --owner=0 --group=0 --numeric-owner -cJf "$W/erssi-2026.11.4-debian13-amd64.tar.xz" erssi-2026.11.4-debian13-amd64)
sign_sums
refused "a MANIFEST of another version" "its MANIFEST is of version 2026.11.3" "$W/erssi-2026.11.4-debian13-amd64.tar.xz"
rm "$W/erssi-2026.11.4-debian13-amd64.tar.xz"
# only what the release signed is packed
tarball 2026.11.3 debian13-amd64 "$NEEDED13"
T13=$W/erssi-2026.11.3-debian13-amd64.tar.xz
cp "$T13" "$W/keep.tar.xz"
printf 'x' >> "$T13"
refused "a tarball other than the one signed" "does not match its sum in the release's SHA256SUMS" "$T13"
cp "$W/keep.tar.xz" "$T13"
cp "$W/SHA256SUMS" "$W/keep.sums"
printf '%s  extra\n' "$(sha256sum < "$T13" | cut -d' ' -f1)" >> "$W/SHA256SUMS"
refused "SHA256SUMS changed after it was signed" "SHA256SUMS.sig is not a signature of SHA256SUMS" "$T13"
cp "$W/keep.sums" "$W/SHA256SUMS"
ssh-keygen -q -t ed25519 -N '' -C other -f "$W/otherkey" || exit 1
printf 'test@example.invalid %s\n' "$(cut -d' ' -f1,2 "$W/otherkey.pub")" > "$W/other-signers"
$BD --signers "$W/other-signers" --out "$W/o1" "$T13" > "$W/out" 2>&1
check "signed by a key not of the signers: refused" grep -q "SHA256SUMS.sig is not a signature" "$W/out"
mv "$W/SHA256SUMS.sig" "$W/keep.sig"
refused "no signature next to it" "no SHA256SUMS and SHA256SUMS.sig" "$T13"
mv "$W/keep.sig" "$W/SHA256SUMS.sig"
cp "$W/erssi-2026.11.3-debian13-amd64.tar.xz" "$W/other.tar.xz"
refused "a file not named as a release's" "not erssi-<version>-<platform>.tar.xz" "$W/other.tar.xz"

# --- the repository ---
G=$W/gnupg
mkdir -m 700 "$G"
gpg --homedir "$G" --batch --passphrase '' --quick-gen-key "erssi test <test@example.invalid>" ed25519 sign never > /dev/null 2>&1 ||
    { echo "skip: gpg cannot make a key here"; exit 77; }
K=$(gpg --homedir "$G" --batch --with-colons --list-secret-keys | awk -F: '$1 == "fpr" {print $10; exit}')
AR="sh $SRC/utils/apt-repo.sh"
$AR --repo "$W/repo" --gnupghome "$G" --key "$K" "$W"/o1/*.deb > "$W/out" 2>&1
check "the repository is written" grep -q "dists/trixie: 1 package(s), signed" "$W/out"
for c in bookworm trixie noble; do
    check "$c: InRelease verifies with the exported key" gpgv --keyring "$W/repo/erssi-archive-keyring.gpg" "$W/repo/dists/$c/InRelease" > /dev/null 2>&1
done
check "Release.gpg verifies too" gpgv --keyring "$W/repo/erssi-archive-keyring.gpg" "$W/repo/dists/trixie/Release.gpg" "$W/repo/dists/trixie/Release" > /dev/null 2>&1
P=$W/repo/dists/noble/main/binary-amd64/Packages
check "each suite lists its own package only" sh -c 'grep -q "^Version: 2026.11.3-1~ubuntu24.04$" "$1" && [ "$(grep -c "^Package:" "$1")" = 1 ]' sh "$P"
sum=$(sha256sum < "$W/repo/pool/main/e/erssi/erssi_2026.11.3-1~ubuntu24.04_amd64.deb" | awk '{print $1}')
check "... with the sha256 of the pool's file" grep -q "^SHA256: $sum$" "$P"
check "Release names Packages with its sha256" grep -q " $(sha256sum < "$P" | awk '{print $1}') .* main/binary-amd64/Packages$" "$W/repo/dists/noble/Release"
# a package that is not what its name says (another Package, or an erssi
# of another version under this name): refused before anything is written
mkdir "$W/fake"
dpkg-deb -R "$W/o1/erssi_2026.11.3-1~deb13_amd64.deb" "$W/fake/root"
sed -i 's/^Package: erssi$/Package: bash/' "$W/fake/root/DEBIAN/control"
dpkg-deb --root-owner-group -b "$W/fake/root" "$W/fake/erssi_2026.11.9-1~deb13_amd64.deb" > /dev/null 2>&1
before=$(ls -lR "$W/repo")
$AR --repo "$W/repo" --gnupghome "$G" --key "$K" "$W/fake/erssi_2026.11.9-1~deb13_amd64.deb" > "$W/out" 2>&1
check "a package of another name inside: refused" grep -q "its control fields are not Package erssi, Version 2026.11.9-1~deb13" "$W/out"
cp "$W/o1/erssi_2026.11.3-1~deb13_amd64.deb" "$W/fake/erssi_2026.11.8-1~deb13_amd64.deb"
$AR --repo "$W/repo" --gnupghome "$G" --key "$K" "$W/fake/erssi_2026.11.8-1~deb13_amd64.deb" > "$W/out" 2>&1
check "an erssi of another version under the name: refused" grep -q "its control fields are not" "$W/out"
check "... the repository unchanged" [ "$(ls -lR "$W/repo")" = "$before" ]
# a relative --gnupghome: the same home when it signs in the repository
(cd "$W" && sh "$SRC/utils/apt-repo.sh" --repo repo --gnupghome gnupg --key "$K" "$W/o1/erssi_2026.11.3-1~deb13_amd64.deb") > "$W/out" 2>&1
check "relative --repo and --gnupghome: signed" grep -q "dists/trixie: 1 package(s), signed" "$W/out"
# a newer release next to the old one: both kept, both listed
tarball 2026.11.4 debian13-amd64 "$NEEDED13"
mkdir "$W/o4" && $BD --out "$W/o4" "$W/erssi-2026.11.4-debian13-amd64.tar.xz" > /dev/null 2>&1
$AR --repo "$W/repo" --gnupghome "$G" --key "$K" "$W"/o4/*.deb > /dev/null 2>&1
check "a newer release: added, the old one kept" sh -c '[ "$(grep -c "^Package:" "$1")" = 2 ]' sh "$W/repo/dists/trixie/main/binary-amd64/Packages"
# the same name with other bytes: refused, the pool unchanged
cp "$W/o4/erssi_2026.11.4-1~deb13_amd64.deb" "$W/x.deb"
printf 'x' >> "$W/x.deb"
mkdir "$W/o5" && mv "$W/x.deb" "$W/o5/erssi_2026.11.4-1~deb13_amd64.deb"
$AR --repo "$W/repo" --gnupghome "$G" --key "$K" "$W/o5/erssi_2026.11.4-1~deb13_amd64.deb" > "$W/out" 2>&1
check "a release with other bytes: refused" grep -q "a release is never replaced" "$W/out"
check "... the pool's file kept" cmp -s "$W/o4/erssi_2026.11.4-1~deb13_amd64.deb" "$W/repo/pool/main/e/erssi/erssi_2026.11.4-1~deb13_amd64.deb"
[ "$FAILED" = 0 ]
