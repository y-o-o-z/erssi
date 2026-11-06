#!/bin/sh
# utils/apt-repo.sh: the signed apt repository of erssi's Debian packages.
#
# Usage:
#   apt-repo.sh --repo DIR --gnupghome DIR --key KEYID DEB...
#       Adds the packages DEB (made by build-deb.sh) to the repository in
#       --repo (made when it is not there) and writes its indexes again:
#         pool/main/e/erssi/<package>.deb          every version, kept
#         dists/<codename>/main/binary-amd64/Packages{,.gz,.xz}
#         dists/<codename>/Release, InRelease, Release.gpg
#         erssi-archive-keyring.gpg (and .asc)     the public key
#       for bookworm (Debian 12, ~deb12), trixie (Debian 13, ~deb13) and
#       noble (Ubuntu 24.04, ~ubuntu24.04). The Release files are signed
#       with the OpenPGP key KEYID of the keyring in --gnupghome (gpg).
#       A package already in the pool must be the same bytes: a release
#       is never replaced.
#
# A system then takes erssi from it with (Debian 13; bookworm or noble
# for the others):
#   curl -fsSLo /etc/apt/keyrings/erssi.gpg <URL>/erssi-archive-keyring.gpg
#   echo 'deb [signed-by=/etc/apt/keyrings/erssi.gpg] <URL> trixie main' \
#       > /etc/apt/sources.list.d/erssi.list
#   apt update && apt install erssi
# and updates it with apt upgrade.
#
# It needs apt-ftparchive (apt-utils), gpg, gzip and xz.
#
# Exit: 0 written, 1 refused or failed, 2 usage.
set -u

say() { printf 'apt-repo: %s\n' "$*"; }
die() { printf 'apt-repo: %s\n' "$*" >&2; exit 1; }
usage() { sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

REPO="" GHOME="" KEY=""
while [ $# -gt 0 ]; do
    case $1 in
        --repo|--gnupghome|--key)
            [ $# -ge 2 ] || usage
            case $1 in --repo) REPO=$2 ;; --gnupghome) GHOME=$2 ;; --key) KEY=$2 ;; esac
            shift 2 ;;
        -h|--help) usage ;;
        --) shift; break ;;
        -*) usage ;;
        *) break ;;
    esac
done
[ -n "$REPO" ] && [ -n "$GHOME" ] && [ -n "$KEY" ] && [ $# -ge 1 ] || usage
for t in apt-ftparchive gpg gzip xz cmp; do
    command -v "$t" > /dev/null 2>&1 || die "$t is missing"
done
[ -d "$GHOME" ] || die "$GHOME: no such directory"
GPG="gpg --homedir $GHOME --batch --no-tty --local-user $KEY"
$GPG --list-secret-keys "$KEY" > /dev/null 2>&1 || die "no secret key $KEY in $GHOME"

POOL=pool/main/e/erssi
mkdir -p "$REPO/$POOL" || die "cannot make $REPO/$POOL"
for d in "$@"; do
    b=$(basename -- "$d")
    case $b in
        erssi_*-1~deb12_amd64.deb|erssi_*-1~deb13_amd64.deb|erssi_*-1~ubuntu24.04_amd64.deb) ;;
        *) die "$b: not a package of build-deb.sh" ;;
    esac
    [ -f "$d" ] || die "$d: no such file"
    if [ -e "$REPO/$POOL/$b" ]; then
        cmp -s "$d" "$REPO/$POOL/$b" || die "$b is in the pool already with other bytes - a release is never replaced"
        say "$b: in the pool already"
    else
        cp "$d" "$REPO/$POOL/$b.tmp" && mv "$REPO/$POOL/$b.tmp" "$REPO/$POOL/$b" || die "cannot copy $b"
        say "$b: added"
    fi
done

cd "$REPO" || exit 1
ALL=$(apt-ftparchive packages "$POOL") || die "apt-ftparchive packages failed"
for s in bookworm:deb12:"Debian 12" trixie:deb13:"Debian 13" noble:ubuntu24.04:"Ubuntu 24.04"; do
    code=${s%%:*} rest=${s#*:}
    tag=${rest%%:*} what=${rest#*:}
    dir=dists/$code/main/binary-amd64
    mkdir -p "$dir" || exit 1
    # the stanzas of this suite's packages
    printf '%s\n' "$ALL" | awk -v t="~$tag" '
        BEGIN { RS = ""; ORS = "\n\n" }
        { if (match($0, /\nVersion: [^\n]*/)) { v = substr($0, RSTART, RLENGTH)
              if (substr(v, length(v) - length(t) + 1) == t) print } }' > "$dir/Packages"
    gzip -9n < "$dir/Packages" > "$dir/Packages.gz"
    xz -9 < "$dir/Packages" > "$dir/Packages.xz"
    apt-ftparchive \
        -o APT::FTPArchive::Release::Origin="erssi Shellter Edition" \
        -o APT::FTPArchive::Release::Label="erssi" \
        -o APT::FTPArchive::Release::Suite="$code" \
        -o APT::FTPArchive::Release::Codename="$code" \
        -o APT::FTPArchive::Release::Architectures="amd64" \
        -o APT::FTPArchive::Release::Components="main" \
        -o APT::FTPArchive::Release::Description="erssi Shellter Edition for $what" \
        release "dists/$code" > "dists/$code/Release.tmp" || die "apt-ftparchive release failed"
    mv "dists/$code/Release.tmp" "dists/$code/Release"
    rm -f "dists/$code/InRelease" "dists/$code/Release.gpg"
    $GPG --clearsign -o "dists/$code/InRelease" "dists/$code/Release" || die "cannot sign dists/$code"
    $GPG --armor --detach-sign -o "dists/$code/Release.gpg" "dists/$code/Release" || die "cannot sign dists/$code"
    say "dists/$code: $(grep -c '^Package:' "$dir/Packages") package(s), signed"
done
gpg --homedir "$GHOME" --batch --export "$KEY" > erssi-archive-keyring.gpg || die "cannot export the key"
gpg --homedir "$GHOME" --batch --armor --export "$KEY" > erssi-archive-keyring.asc || die "cannot export the key"
say "the key: erssi-archive-keyring.gpg, fingerprint $(gpg --homedir "$GHOME" --batch --with-colons --fingerprint "$KEY" | awk -F: '$1 == "fpr" {print $10; exit}')"
