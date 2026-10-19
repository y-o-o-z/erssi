#!/bin/sh
# shellter-install.sh - install and update erssi Shellter Edition without root.
#
#   curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
#   sh shellter-install.sh [--prefix DIR] [--src DIR] [--ref REF] [--repo URL]
#                          [--no-test|--test] [--clean|--no-clean]
#                          [--binary|--source] [--binary-base URL] [--keep N]
#                          [--dry-run] [--rollback] [--uninstall]
#   erssi --update          (= this installer, installed with erssi: --update)
#   erssi --check-update    (= --update --check)
#
# Installs to ~/.local/opt/erssi and links ~/.local/bin/erssi. A release
# that has a ready-made package for this platform (debian13-amd64,
# freebsd14-amd64) is installed from it, in seconds: SHA256SUMS of the
# release must carry a valid SSH signature by the release key below
# (namespace erssi-release), the package its checksum there, and its MANIFEST
# must name the release, its commit (the recorded one below, or the one of
# the signed tag on --update), this platform and this system's perl; the
# libraries it needs must be there. It is unpacked into
# <prefix>/releases/<version> and <prefix>/current switched to it in one
# step; the previous release stays for --rollback. Anything else is built
# from source, as below; a package that does not verify stops the installer
# with nothing changed.
#
# The source build: a missing
# build tool (meson, ninja) is installed into a private Python venv, as a
# ready-made wheel only; where there is none, the installer stops and names
# the package to ask for. Missing libraries are listed with the command for
# your system.
#
# Install: without --ref the newest release recorded below, checked against
# the recorded commit, so neither a moved nor a newly pushed tag changes what
# gets installed; a release marked "signed" must also carry a valid SSH
# signature by the key below (git verify-tag; needs git 2.34+ and
# ssh-keygen; without them only the recorded commit is checked, with a
# warning). --ref main (or any other ref) is built as is, unverified, with a
# warning. This script itself is trusted as it was downloaded: the key and
# the commits below are what everything else is checked against
# (docs/shellter/install.md: "What the installer verifies").
#
# Update (--update): the newest release on the repository that carries a
# valid signature by the key below - the key of the installed copy, so a
# release pushed by anyone else is skipped - if its version is newer than
# the installed one; --check only reports. Release tags never move (a
# release made again is a new version): a signed tag of the installed
# release that points to another commit is reported, never installed. An
# erssi installed from a branch or a commit (--ref) is offered a release
# only when its version is newer. The settings of the install (what it was
# installed from, the source directory, repository, tests, clean) come from
# <prefix>/share/irssi/shellter-install.conf; an erssi kept up to date by
# another tool names it there as UPDATE_COMMAND, which --update runs.
#
# Inside a user namespace (rootless container, bwrap) root's directories,
# like those of every user the namespace does not map, show as the overflow
# uid; SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 counts it as root's above the
# parent of --src (safe_path). SHELLTER_PROC, SHELLTER_OS_RELEASE and
# SHELLTER_BASE_OPENSSL are for the tests only (a stand-in /proc,
# os-release and FreeBSD base OpenSSL).
#
# Build output goes to a log (~/.local/state/erssi/install.log), shown when a
# step fails. Colours only on a terminal (NO_COLOR turns them off).
# Everything runs from main() at the end of the file, so a download cut
# short by the network does not run half a script.
set -eu

# release tag -> commit it must point to [signed] (one line per release;
# "signed": the tag must be signed by RELEASE_SIGNER)
RELEASES="
shellter-v1.3.2 216037a5966568b401129063959d7f24d9e61411 signed
shellter-v1.3.3 186c893c34410616b7e87e17e48516c3b0ad7ef0 signed
shellter-v1.3.4 ec182dacc33504cdb702028f3f0eff97cc6175ce signed
shellter-v1.3.5 7475d7588fd5f9edd4cf00640e24c3c362211e8b signed
shellter-v1.3.6 f6d162ef8a48180b20b2087d47041053ac7bed5f signed
shellter-v1.3.7 07fd5358ae4a0a031bca066e7acf2f98ded8edcf signed
shellter-v1.3.8 7f8f2280ddfd00d6cb771eb1b26056a5e02f26ad signed
shellter-v1.3.9 2a1a9791f2b27b25ea3f4e0f19a70745b862f2bb signed
shellter-v1.3.10 c5fd14a725373b1c77a962d87710d492abe1aa0a signed
shellter-v2026.10.0 c43238e7125e34a8d03d7a1797b9e93f3aec2c23 signed
shellter-v2026.10.1 f8a831803d5d85bccc7818c7e79c57be61127483 signed
shellter-v2026.10.2 d4e21dbe4573f584295ce4778539358a57f64af3 signed
shellter-v2026.10.3 4c040de974e10d837cfeba72b1aab43e4785ccb9 signed
shellter-v2026.10.4 e59994cba5db5510837c109d80d4fc4ae5474e8d signed
"
# who signs the releases (git allowed_signers format; utils/allowed_signers)
# and the fingerprint of that key (ssh-keygen -lf), which every signature
# check also compares: a new key changes both
RELEASE_SIGNER='136268860+y-o-o-z@users.noreply.github.com namespaces="git" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJmzPHTiL6dPEntggsVu9kJyAsY9KLaKNUW5kejyJOYS'
RELEASE_FINGERPRINT='SHA256:hF7dvX7vdTfqEsC9LHeujMoHf4NhWdqeBCvN6jXEDAw'
REPO_DEFAULT=https://github.com/y-o-o-z/erssi.git
# the release assets of the default repository: <base>/shellter-v<version>/<asset>
BINARY_BASE_DEFAULT=https://github.com/y-o-o-z/erssi/releases/download
# A fixed message signed once with a throwaway key (namespace erssi-probe):
# ssh-keygen proves it can check SSH signatures (-Y verify, OpenSSH 8.1+)
# by verifying it, before any signature of a release counts as bad
PROBE_KEY='ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIM+P2rYYHCSZZqWFSr5FeU+UzW+Pfou6WwVVY21luNSS'
PROBE_SIG='-----BEGIN SSH SIGNATURE-----
U1NIU0lHAAAAAQAAADMAAAALc3NoLWVkMjU1MTkAAAAgz4/athgcJJlmpYVKvkV5T5TNb4
9+i7pbBVVjbWW41JIAAAALZXJzc2ktcHJvYmUAAAAAAAAABnNoYTUxMgAAAFMAAAALc3No
LWVkMjU1MTkAAABAl/SNOpZPDqDX4lqGV2GRdxwlMh9vgV0ameb+vFBuDrre1SigohZndm
gEVGAHMwzYnHcHITvNK47wzKTJCfzdBw==
-----END SSH SIGNATURE-----'
MESON_PIP="meson==1.12.1"
NINJA_PIP="ninja==1.13.2"

usage() {
    cat <<'USAGE'
erssi Shellter Edition - install and update

  curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
  sh shellter-install.sh [options]
  erssi --update | erssi --check-update

  --update       update to the newest signed release when its version is
                 newer than the installed one (also for an erssi installed
                 from a branch or a commit with --ref)
  --check        with --update: only report whether an update is available
  --prefix DIR   install here, an absolute path (default ~/.local/opt/erssi)
  --src DIR      source checkout used for building (default ~/.local/src/erssi)
  --ref REF      release tag, branch or full commit SHA (default: newest
                 verified release)
  --repo URL     git repository (default https://github.com/y-o-o-z/erssi.git)
  --no-test      skip the test suite
  --test         run the test suite (the default)
  --clean        remove the build directory afterwards (small disk quotas)
  --no-clean     keep the build directory (the default)
  --yes          as root: install the missing packages without asking
  --binary       install from the release's ready-made package only (the
                 default when there is one for this system); stop if none
  --source       always build from source (kept for erssi --update)
  --binary-base URL
                 where the packages are: a URL or a directory holding
                 shellter-vX.Y.Z/ (default: the GitHub releases)
  --keep N       old releases of a binary install kept besides the one in
                 use (default 2, at least 1: a running erssi needs the one
                 it was started from until /upgrade)
  --dry-run      check and show what would be done; change nothing
  --rollback     switch a binary install back to the previous release
  --uninstall    remove the prefix and ~/.local/bin/erssi; ~/.erssi stays

What is verified: a recorded release (the default) is built only if its
tag is signed by the release key this script carries and points to the
commit it records; without ssh-keygen or git 2.34+ only the commit, with a
warning. Any other --ref is built unverified, with a warning. --update
takes only a newer release signed by the key of the installed copy. This
script itself is trusted as downloaded; to check it first, see "What the
installer verifies" in docs/shellter/install.md. A binary package is
installed only if SHA256SUMS of its release is signed by the release key
(namespace erssi-release), the package matches its sum there, and its
MANIFEST names that release, the commit this script records (on --update:
the commit of the signed tag), this platform and this system's perl.

--src, --repo, --no-test/--test, --clean/--no-clean, --source (undone by
--binary), --binary-base and --keep are kept in
<prefix>/share/irssi/shellter-install.conf and used again by the next
install into the same prefix and by erssi --update, until given again.

As root the installer lists the system packages erssi needs and installs
them after you confirm; on a user account it lists them with the command
for the administrator. There a missing meson or ninja comes from pip, into
a venv next to the source directory (DIR.venv), as a ready-made wheel only:
where pip has none (ninja on FreeBSD), the package is asked for instead.

The source directory and its venv must be yours; the directories above
them yours or root's. In a user namespace (rootless container, bwrap,
unshare) root's directories, like those of every user it does not map,
show as the overflow uid (65534), which may also be a user the namespace
maps (rootless podman's nobody):
  SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1
                 count that uid as root's above the parent of --src (so
                 every unmapped user's directories there are trusted, not
                 only root's; never the parent, --src or its venv); does
                 nothing outside a user namespace or without /proc
USAGE
}

# an option that takes a value: present, not empty, not the next option
optval() {
    case "$2" in
        ''|-*) echo "shellter-install.sh: $1 needs a value, e.g. $1 $3 (--help)" >&2; exit 2 ;;
    esac
}

# ── output ───────────────────────────────────────────────────────────
# One line per step: "  [3/6] Building ........ ✓ 2m 04s". On a terminal
# the line shows a spinner and the time while the step runs. The symbols and
# the spinner are UTF-8 only when the locale is; otherwise ASCII ("ok").
utf8_locale() {
    cm=""
    have locale && cm=$(locale charmap 2>/dev/null) || cm=""
    if [ -n "$cm" ]; then
        case "$cm" in [Uu][Tt][Ff]-8|[Uu][Tt][Ff]8) return 0 ;; esac
        return 1
    fi
    # no locale command, or it printed nothing: the locale variables
    case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
        *[Uu][Tt][Ff]-8*|*[Uu][Tt][Ff]8*) return 0 ;;
    esac
    return 1
}
ui_init() {
    TTY=0
    if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != dumb ]; then
        TTY=1
        B=$(printf '\033[1m'); D=$(printf '\033[2m'); A=$(printf '\033[38;5;214m')
        R=$(printf '\033[38;5;203m'); G=$(printf '\033[38;5;71m'); N=$(printf '\033[0m')
        CR=$(printf '\r\033[K')
    else
        B='' D='' A='' R='' G='' N='' CR=''
    fi
    if utf8_locale; then
        UTF8=1 S_OK='✓' S_FAIL='✗' S_SEP='·' S_ARROW='→' S_SKIP='–' S_BUSY='…'
    else
        UTF8=0 S_OK=ok S_FAIL=failed S_SEP=- S_ARROW='->' S_SKIP=- S_BUSY=...
    fi
    STEP=0; STEPS=6; IN_STEP=0; STEP_TITLE=""; LOG=""
}
header() {   # what
    printf '\n  %s%serssi Shellter Edition%s %s%s %s%s\n\n' "$B" "$A" "$N" "$D" "$S_SEP" "$1" "$N"
}
field() { printf '  %s%-9s%s %s\n' "$D" "$1" "$N" "$2"; }
tilde() { case "$1" in "$HOME"/*) printf '~%s' "${1#"$HOME"}" ;; *) printf '%s' "$1" ;; esac; }
now() { date +%s; }
elapsed() {  # seconds -> "47s" / "2m 04s"
    if [ "$1" -lt 60 ]; then printf '%ds' "$1"; else printf '%dm %02ds' $(($1 / 60)) $(($1 % 60)); fi
}
step_line() {   # status text
    printf '%s  %s[%d/%d]%s %-20s %s' "$CR" "$D" "$STEP" "$STEPS" "$N" "$STEP_TITLE" "$1"
}
step() {   # title
    STEP=$((STEP + 1)); STEP_TITLE=$1; STEP_T0=$(now); IN_STEP=1
    [ "$TTY" = 1 ] && step_line "${D}${S_BUSY}${N}"
    return 0
}
done_() {   # detail
    step_line "${G}${S_OK}${N} ${D}$1${N}"; printf '\n'; IN_STEP=0
}
skip_() {   # detail
    step_line "${D}${S_SKIP} $1${N}"; printf '\n'; IN_STEP=0
}
warn() {   # shown under the step it belongs to
    if [ "$IN_STEP" = 1 ] && [ "$TTY" = 1 ]; then printf '%s' "$CR"; fi
    printf '        %s!%s %s\n' "$A" "$N" "$*"
    if [ "$IN_STEP" = 1 ] && [ "$TTY" = 1 ]; then step_line "${D}${S_BUSY}${N}"; fi
    return 0
}
die() {
    if [ "$IN_STEP" = 1 ]; then step_line "${R}${S_FAIL}${N}"; printf '\n'; IN_STEP=0; fi
    printf '\n  %serror:%s %s\n' "$R" "$N" "$*" >&2
    exit 1
}
die_log() {   # message - with the end of the build log
    if [ "$IN_STEP" = 1 ]; then step_line "${R}${S_FAIL}${N}"; printf '\n'; IN_STEP=0; fi
    printf '\n  %serror:%s %s\n\n' "$R" "$N" "$1" >&2
    tail -n 15 "$LOG" | sed 's/^/    /' >&2
    printf '\n  %sfull log: %s%s\n' "$D" "$LOG" "$N" >&2
    exit 1
}
have() { command -v "$1" >/dev/null 2>&1; }
spinner_frame() {   # i
    if [ "$UTF8" = 1 ]; then
        case $(($1 % 10)) in
            0) printf '⠋' ;; 1) printf '⠙' ;; 2) printf '⠹' ;; 3) printf '⠸' ;; 4) printf '⠼' ;;
            5) printf '⠴' ;; 6) printf '⠦' ;; 7) printf '⠧' ;; 8) printf '⠇' ;; *) printf '⠏' ;;
        esac
    else
        case $(($1 % 4)) in
            0) printf '|' ;; 1) printf '/' ;; 2) printf '-' ;; *) printf '\134' ;;
        esac
    fi
}

# command >> log, with a spinner and the time on a terminal; returns its code
run_logged() {
    printf '\n$ %s\n' "$*" >> "$LOG"
    if [ "$TTY" = 0 ]; then
        "$@" >> "$LOG" 2>&1
        return
    fi
    "$@" >> "$LOG" 2>&1 &
    JOB=$!
    i=0
    while kill -0 "$JOB" 2>/dev/null; do
        step_line "$A$(spinner_frame "$i")$N $D$(elapsed $(($(now) - STEP_T0)))$N"
        i=$((i + 1))
        sleep 1
    done
    rc=0; wait "$JOB" || rc=$?
    JOB=""
    return "$rc"
}

# ── versions and signatures ──────────────────────────────────────────
# "2026.10.0" vs "1.3.10", "1.3.10" vs "1.3.9": 0 when $1 is newer than $2
newer() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        n = split(a, x, "."); m = split(b, y, ".")
        for (i = 1; i <= (n > m ? n : m); i++) {
            if ((x[i] + 0) > (y[i] + 0)) exit 0
            if ((x[i] + 0) < (y[i] + 0)) exit 1
        }
        exit 1 }'
}
# signatures can be checked here: git 2.34+ and ssh-keygen
can_verify() {
    gv=$(git --version | sed -n 's/^git version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')
    # shellcheck disable=SC2086 # "2 47" -> $1 $2
    set -- $gv
    [ -n "${2:-}" ] && { [ "$1" -gt 2 ] || { [ "$1" -eq 2 ] && [ "$2" -ge 34 ]; }; } && have ssh-keygen
}
# FETCH_HEAD in repository $1 is the annotated tag $2 with an SSH signature
# by RELEASE_SIGNER. FETCH_HEAD is read once: the tag object checked is the
# one built (TAG_OBJ). gpg and gpgsm are disabled - git picks the verifier
# from the signature, and an OpenPGP signature by any key in the user's
# keyring would otherwise pass.
signed_tag() {
    TAG_OBJ=$(git -C "$1" rev-parse --verify -q FETCH_HEAD) || return 1
    [ "$(git -C "$1" cat-file -t "$TAG_OBJ" 2>/dev/null)" = tag ] || return 1
    [ "$(git -C "$1" cat-file tag "$TAG_OBJ" | sed -n '1,/^$/s/^tag //p')" = "$2" ] || return 1
    signers=$(mktemp)
    printf '%s\n' "$RELEASE_SIGNER" > "$signers"
    if git -C "$1" -c gpg.format=ssh -c gpg.ssh.allowedSignersFile="$signers" \
            -c gpg.program=false -c gpg.openpgp.program=false -c gpg.x509.program=false -c gpg.ssh.program=ssh-keygen \
            verify-tag --raw "$TAG_OBJ" 2>&1 |
            grep -qxF "Good \"git\" signature for ${RELEASE_SIGNER%% *} with ED25519 key $RELEASE_FINGERPRINT"; then
        rm -f "$signers"; return 0
    fi
    rm -f "$signers"; return 1
}
fetch_ref() {   # repository-dir ref
    git -C "$1" -c advice.detachedHead=false fetch -q --depth 1 -- "$REPO" "$2" >> "${LOG:-/dev/null}" 2>&1
}
# the newest shellter-vX.Y.Z on $REPO newer than $2 with a valid signature,
# checked in the repository $1; unsigned ones are named and skipped. Prints
# the tag; none newer than $2: prints nothing (an up to date install). With
# the commit $3 that release $2 was installed from, the tag of $2 is checked
# too: release tags never move (a release made again is a new version), so
# one that now points to another commit - moved by mistake, or an older tag
# object served by a mirror to roll the install back - is reported and
# never installed. Codes: 1 no signed release, 2 $REPO cannot be reached,
# 3 a release cannot be downloaded (its name printed) - a failed download
# is not an unsigned release, and must not end as "up to date".
newest_signed() {
    refs=$(git ls-remote --tags --refs -- "$REPO" 'shellter-v*' 2>>"${LOG:-/dev/null}") || return 2
    tags=$(printf '%s\n' "$refs" |
        sed -n 's|.*refs/tags/\(shellter-v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)$|\1|p' |
        sed 's/^shellter-v//' | sort -t. -k1,1nr -k2,2nr -k3,3nr)
    [ -n "$tags" ] || return 1
    tries=0
    for v in $tags; do
        if ! newer "$v" "$2"; then
            # older, or the installed release without a known commit
            newer "$2" "$v" || [ -z "${3:-}" ] && return 0
            fetch_ref "$1" "shellter-v$v" || { echo "shellter-v$v"; return 3; }
            if signed_tag "$1" "shellter-v$v"; then
                [ "$(git -C "$1" rev-parse "$TAG_OBJ^{commit}")" = "$3" ] ||
                    warn "the release tag shellter-v$v now points to a different commit than the installed one - not updating; report it" >&2
                return 0
            fi
            warn "shellter-v$v is not signed by the erssi Shellter release key - skipped" >&2
            return 0
        fi
        fetch_ref "$1" "shellter-v$v" || { echo "shellter-v$v"; return 3; }
        if signed_tag "$1" "shellter-v$v"; then
            echo "shellter-v$v $TAG_OBJ"; return 0
        fi
        warn "shellter-v$v is not signed by the erssi Shellter release key - skipped" >&2
        tries=$((tries + 1))
        [ "$tries" -lt 5 ] || { warn "five unsigned newer tags - giving up (report it)" >&2; return 1; }
    done
    return 1
}
# ~/.local/bin/erssi -> $PREFIX/bin/erssi only for an install of the user's
# own in the home: never as root, never for a prefix outside the home, and
# never when ~/.local/bin is $PREFIX/bin (the link would replace the
# installed binary). Paths compared as resolved (pwd -P).
bin_link_wanted() {
    [ "$ROOT" = 0 ] || return 1
    p=$(cd "$PREFIX" 2>/dev/null && pwd -P) || return 1
    h=$(cd "$HOME" 2>/dev/null && pwd -P) || return 1
    case "$p" in "$h"/*) ;; *) return 1 ;; esac
    pb=$(cd "$PREFIX/bin" 2>/dev/null && pwd -P) || return 1
    lb=$(cd "$HOME/.local/bin" 2>/dev/null && pwd -P) || lb=""
    [ "$pb" != "$lb" ]
}
# The source directory, its venv and every directory above them must be
# the user's own: whoever can make, change or rename them runs code as this
# user (the venv's meson and ninja, the build files of the checkout), and
# --src is kept for every erssi --update.
# shellcheck disable=SC2012 # find -printf is not POSIX; ls -n is
mode_gid() { ls -ldn "$1" 2>/dev/null | awk '{print $1, $4}'; }
# group write by group $2 (mode $1) is the user's alone: the user's private
# group (the user's gid, named like the user, no other members) and no ACL
private_group() {
    case "$1" in *+) return 1 ;; esac
    [ "$2" = "$(id -g)" ] || return 1
    [ "$(id -gn)" = "$(id -un)" ] || return 1
    have getent || return 1
    members=$(getent group "$(id -gn)" | cut -d: -f4)
    [ -z "$members" ] || [ "$members" = "$(id -un)" ]
}
# an ACL ("+") outside Linux: FreeBSD and macOS (NFSv4/ZFS ACLs) can grant
# write without the mode bits showing it; on Linux the group bits are the
# ACL mask
untrusted_acl() {   # mode
    case "$1" in *+) [ "$(uname -s)" != Linux ] ;; *) return 1 ;; esac
}
# 0 when another user can write to $1
others_can_write() {
    pg=$(mode_gid "$1")
    [ -n "$pg" ] || return 0
    if untrusted_acl "${pg%% *}"; then return 0; fi
    case "${pg%% *}" in
        ????????w*) return 0 ;;
        ?????w*) if private_group "${pg%% *}" "${pg#* }"; then return 1; fi; return 0 ;;
    esac
    return 1
}
safe_dir() {   # dir - when it exists: the user's, and only the user can write to it
    mapped_self
    [ -e "$1" ] || [ -L "$1" ] || return 0
    [ ! -L "$1" ] || die "$1 is a symbolic link - use a directory of your own: --src DIR"
    o=$(owner "$1")
    [ "$o" = "$(id -u)" ] || die "$1 belongs to another user (uid ${o:-unknown}) - use a directory of your own: --src DIR"
    if others_can_write "$1"; then
        die "$1 can be changed by other users - use a directory of your own (--src DIR) or take their write access away"
    fi
}
# In a user namespace (a rootless container, bwrap, unshare) the files of
# every user it does not map, root's among them, show as the kernel's
# overflow uid (65534). SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 counts that
# uid as root's above the parent of --src - so the directories there of
# every unmapped user, not only root's, and of a user the namespace maps to
# that same uid (rootless podman maps its "nobody" to 65534); never for the
# parent, the source directory or its venv. Outside a user namespace, and
# without /proc (FreeBSD), it does nothing. SHELLTER_PROC (test-only, like
# SHELLTER_OS_RELEASE and SHELLTER_BASE_OPENSSL): a stand-in /proc.
in_userns() {   # 0 when /proc/self/uid_map is not the initial (identity) map
    um="${SHELLTER_PROC:-/proc}/self/uid_map"
    [ -r "$um" ] || return 1
    [ "$(awk '{$1 = $1; print}' "$um" 2>/dev/null)" != "0 0 4294967295" ]
}
overflow_uid() {   # the overflow uid inside a user namespace, else nothing
    in_userns || return 0
    ov=$(cat "${SHELLTER_PROC:-/proc}/sys/kernel/overflowuid" 2>/dev/null) || return 0
    case "$ov" in ''|*[!0-9]*|0) return 0 ;; esac
    printf '%s\n' "$ov"
}
# A namespace that does not map the user: id -u is the overflow uid as well,
# so every unmapped user's directory would pass as the user's own - refused
mapped_self() {
    ov=$(overflow_uid)
    [ -z "$ov" ] || [ "$(id -u)" != "$ov" ] ||
        die "your uid is not mapped in this user namespace (id -u is the overflow uid $ov, which the files of every unmapped user show as, so no directory can be told to be yours) - run the installer where your uid is mapped (e.g. unshare --map-current-user, podman --userns=keep-id)"
}
# $1, the resolved parent of the source directory, and every directory above
# it: the user's or root's, and nobody else can write to the parent; above
# it only a sticky directory of root's (/tmp) may be open to others
# (ssh StrictModes). Above the parent, the overflow uid counts as root's
# with SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 (see in_userns).
safe_path() {
    d=$1
    hint="choose a --src under a directory only you can write"
    mapped_self
    ovf=$(overflow_uid)
    trust=""
    [ "${SHELLTER_INSTALL_TRUST_OVERFLOW_UID:-}" != 1 ] || trust=$ovf
    while :; do
        o=$(owner "$d")
        r=$o        # the owner as the rules below see it
        [ -z "$trust" ] || [ "$d" = "$1" ] || [ "$o" != "$trust" ] || r=0
        if [ "$o" != "$(id -u)" ] && [ "$r" != 0 ]; then
            if [ -n "$ovf" ] && [ "$o" = "$ovf" ]; then
                # a user namespace: root's directories show as this uid
                if [ "$d" = "$1" ]; then
                    die "$d belongs to another user (uid $o) - $hint (in this user namespace uid $o is every user outside it, root included; SHELLTER_INSTALL_TRUST_OVERFLOW_UID does not cover the parent of --src)"
                fi
                die "$d belongs to another user (uid $o) - $hint, or, as uid $o is every user outside this user namespace (root included), set SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 to count it as root's above the parent of --src"
            fi
            die "$d belongs to another user (uid ${o:-unknown}) - $hint"
        fi
        if others_can_write "$d"; then
            # above the parent: a sticky directory of root's without an
            # untrusted ACL (/tmp) is fine
            pg=$(mode_gid "$d")
            m=${pg%% *}
            if [ "$d" = "$1" ] || [ "$r" != 0 ] || untrusted_acl "$m"; then
                die "$d is writable by other users - $hint"
            fi
            case "$m" in
                ?????????[tT]*) ;;
                *) die "$d is writable by other users - $hint" ;;
            esac
        fi
        [ "$d" != / ] || break
        d=$(dirname -- "$d")
    done
}
conf_get() {   # key -> value from shellter-install.conf (never run as shell)
    [ -f "$CONF" ] && sed -n "s/^$1=//p" "$CONF" | tail -n 1
    return 0
}

# ── binary packages ──────────────────────────────────────────────────
# A release can carry ready-made packages, one per platform:
# erssi-<version>-<platform>.tar.xz with one top directory holding the
# install tree and a MANIFEST, listed in SHA256SUMS, which the release key
# signs (SHA256SUMS.sig, namespace erssi-release). The installer unpacks one
# into <prefix>/releases/<version> and switches the link <prefix>/current to
# it; <prefix>/previous is the release --rollback returns to.

# the platform of the packages, <os><major>-<arch> (debian13-amd64,
# freebsd14-amd64); 1 when it cannot be told
platform_id() {
    case "$(uname -m 2>/dev/null)" in
        x86_64|amd64) arch=amd64 ;;
        aarch64|arm64) arch=arm64 ;;
        *) return 1 ;;
    esac
    case "$(uname -s 2>/dev/null)" in
        Linux)
            f=${SHELLTER_OS_RELEASE:-/etc/os-release}
            [ -r "$f" ] || return 1
            os=$(sed -n 's/^ID=//p' "$f" | tr -d "\"'" | tail -n 1)
            major=$(sed -n 's/^VERSION_ID=//p' "$f" | tr -d "\"'" | tail -n 1) ;;
        FreeBSD)
            os=freebsd
            major=$(freebsd-version -u 2>/dev/null) ;;
        *) return 1 ;;
    esac
    major=${major%%[!0-9]*}
    case "$os" in ''|*[!a-z0-9]*) return 1 ;; esac
    [ -n "$major" ] || return 1
    printf '%s%s-%s\n' "$os" "$major" "$arch"
}
perl_version() {   # 5.40.1 for the perl in PATH, nothing without one
    pv=$(perl -e 'print $^V' 2>/dev/null) || return 0
    pv=${pv#v}
    case "$pv" in ''|*[!0-9.]*) return 0 ;; esac
    printf '%s\n' "$pv"
}
sha256_of() {   # file -> its sha256, in hex
    if have sha256sum; then sha256sum < "$1" | awk '{print $1}'
    elif have sha256; then sha256 -q < "$1"
    elif have shasum; then shasum -a 256 < "$1" | awk '{print $1}'
    fi
}
# tar -J needs xz, except bsdtar (FreeBSD), which has it built in
can_unxz() { have xz || tar --version 2>/dev/null | grep -q bsdtar; }
# asset $1 of the release $2 into the file $3, from BINARY_BASE: an https
# (or a given http) URL, a file:// URL or a directory
download() {
    case "$BINARY_BASE" in
        http://*|https://*)
            p='=https'
            case "$BINARY_BASE" in http://*) p='=http,https' ;; esac
            if have curl; then
                run_logged curl -fsSL --proto "$p" --proto-redir "$p" --retry 2 -o "$3" "$BINARY_BASE/$2/$1"
            else
                run_logged fetch -q -o "$3" "$BINARY_BASE/$2/$1"
            fi ;;
        file://*) run_logged cp "${BINARY_BASE#file://}/$2/$1" "$3" ;;
        *) run_logged cp "$BINARY_BASE/$2/$1" "$3" ;;
    esac
}
# ssh-keygen here can check SSH signatures: it verifies PROBE_SIG (an old
# one, before OpenSSH 8.1, has no -Y and cannot)
can_check_sig() {
    have ssh-keygen || return 1
    pd=$(mktemp -d) || return 1
    printf 'probe namespaces="erssi-probe" %s\n' "$PROBE_KEY" > "$pd/signers"
    printf '%s\n' "$PROBE_SIG" > "$pd/sig"
    pr=0
    printf 'erssi ssh-keygen -Y probe\n' |
        ssh-keygen -Y verify -f "$pd/signers" -I probe -n erssi-probe -s "$pd/sig" > /dev/null 2>&1 || pr=1
    rm -rf "$pd"
    return "$pr"
}
# SHA256SUMS in the directory $1 carries a valid SSH signature
# (SHA256SUMS.sig) by the release key for erssi releases: RELEASE_SIGNER
# with the namespace erssi-release instead of git's, and the fingerprint
# RELEASE_FINGERPRINT
verify_sums() {
    signer=$(printf '%s\n' "$RELEASE_SIGNER" | sed 's/^\([^ ]*\) namespaces="git" /\1 namespaces="erssi-release" /')
    case "$signer" in *' namespaces="erssi-release" '*) ;; *) return 1 ;; esac
    printf '%s\n' "$signer" > "$1/allowed_signers" || return 1
    out=$(ssh-keygen -Y verify -f "$1/allowed_signers" -I "${RELEASE_SIGNER%% *}" -n erssi-release \
            -s "$1/SHA256SUMS.sig" < "$1/SHA256SUMS" 2>&1) || { printf '%s\n' "$out" >> "$LOG"; return 1; }
    printf '%s\n' "$out" >> "$LOG"
    printf '%s\n' "$out" | grep -qxF "Good \"erssi-release\" signature for ${RELEASE_SIGNER%% *} with ED25519 key $RELEASE_FINGERPRINT"
}
# the sha256 that SHA256SUMS ($1) gives for the file $2, if it names it once
sum_of() {
    awk -v n="$2" 'NF == 2 && ($2 == n || $2 == "*" n) && length($1) == 64 && $1 !~ /[^0-9a-f]/ { c++; s = $1 }
        END { if (c == 1) print s }' "$1"
}
# the value of key $2 in MANIFEST $1 (key=value lines, never run as shell);
# 1 unless the key is there exactly once
mf() {
    awk -v k="$2" 'index($0, k "=") == 1 { c++; v = substr($0, length(k) + 2) }
        END { if (c != 1) exit 1; print v }' "$1"
}
# the shared libraries that the programs and modules of the tree $1 need
# and this system lacks (ldd; nothing without ldd - starting the packaged
# erssi then shows it)
missing_libs() {
    have ldd || return 0
    { printf '%s\n' "$1/bin/erssi"; find "$1/lib" -type f -name '*.so*' 2>/dev/null; } |
        while IFS= read -r f; do ldd "$f" 2>/dev/null; done |
        awk '/not found/ { print $1 }' | sort -u | tr '\n' ' ' | sed 's/ $//'
}
lib_package() {   # platform soname -> the package that has it, if known
    case "$1:$2" in
        debian*:libglib-2.0.so.*|debian*:libgio-2.0.so.*|debian*:libgmodule-2.0.so.*|debian*:libgobject-2.0.so.*)
            echo libglib2.0-0t64 ;;
        debian*:libssl.so.3|debian*:libcrypto.so.3) echo libssl3t64 ;;
        debian*:libperl.so.*) echo "libperl${2#libperl.so.}" ;;
        debian*:libncursesw.so.6) echo libncursesw6 ;;
        debian*:libtinfo.so.6) echo libtinfo6 ;;
        debian*:libotr.so.5) echo libotr5t64 ;;
        debian*:libgcrypt.so.20) echo libgcrypt20 ;;
        debian*:libgpg-error.so.0) echo libgpg-error0 ;;
        debian*:libcurl.so.4) echo libcurl4t64 ;;
        debian*:libchafa.so.0) echo libchafa0t64 ;;
        debian*:libutf8proc.so.3) echo libutf8proc3 ;;
        debian*:libsodium.so.23) echo libsodium23 ;;
        debian*:libz.so.1) echo zlib1g ;;
        freebsd*:libglib-2.0.so.*|freebsd*:libgio-2.0.so.*|freebsd*:libgmodule-2.0.so.*|freebsd*:libgobject-2.0.so.*)
            echo glib ;;
        freebsd*:libintl.so.*) echo gettext-runtime ;;
        freebsd*:libiconv.so.*) echo libiconv ;;
        freebsd*:libpcre2-8.so.*) echo pcre2 ;;
        freebsd*:libperl.so.*) echo perl5 ;;
        freebsd*:libotr.so.*) echo libotr ;;
        freebsd*:libgcrypt.so.*) echo libgcrypt ;;
        freebsd*:libgpg-error.so.*) echo libgpg-error ;;
        freebsd*:libcurl.so.*) echo curl ;;
        freebsd*:libchafa.so.*) echo chafa ;;
        freebsd*:libutf8proc.so.*) echo utf8proc ;;
        freebsd*:libsodium.so.*) echo libsodium ;;
    esac
}
# what installs the libraries $2... on platform $1: the command, as root
# the package manager's own, else what to ask the administrator for
lib_hint() {
    pl=$1; shift
    pk="" unknown=""
    for l in "$@"; do
        p=$(lib_package "$pl" "$l")
        if [ -n "$p" ]; then pk="$pk $p"; else unknown="$unknown $l"; fi
    done
    pk=$(printf '%s\n' "$pk" | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' ' | sed 's/ $//')
    if [ -n "$pk" ]; then
        # shellcheck disable=SC2086 # package list
        if [ "$ROOT" = 1 ] && [ -n "$PM" ]; then pm_install_cmd $pk; else pm_hint $pk; fi
    fi
    [ -z "$unknown" ] || echo "the package that has${unknown} (no package known here)"
}
# the release the link $PREFIX/$1 (current, previous) points to:
# releases/<version> -> <version>; nothing when it is not such a link
link_release() {
    [ -L "$PREFIX/$1" ] || return 0
    l=$(readlink "$PREFIX/$1") || return 0
    case "$l" in releases/*) l=${l#releases/} ;; *) return 0 ;; esac
    case "$l" in ''|*[!0-9.]*) return 0 ;; esac
    printf '%s\n' "$l"
}
# the link $1 -> $2, replaced in one step: a new link renamed over the old
# one (rename(2) replaces a link itself; mv would move the new link into
# the directory the old one points to). perl is there: erssi needs it.
switch_link() {
    t="$1.new$$"
    rm -f "$t" && ln -s "$2" "$t" &&
        perl -e 'rename($ARGV[0], $ARGV[1]) or die "rename $ARGV[1]: $!\n"' "$t" "$1" 2>> "$LOG" && return 0
    rm -f "$t"
    return 1
}
# releases beyond KEEP (at least 1) old ones go, newest kept first; the one in use
# always stays, and the one --rollback returns to counts first
prune_releases() {
    RELS="$PREFIX/releases"
    cur=$(link_release current) prev=$(link_release previous) kept=0
    [ -z "$prev" ] || [ "$prev" = "$cur" ] || kept=1
    for v in $(for d in "$RELS"/*; do
            d=${d##*/}
            case "$d" in [0-9]*) case "$d" in *[!0-9.]*) ;; *) echo "$d" ;; esac ;; esac
        done | sort -t. -k1,1nr -k2,2nr -k3,3nr); do
        if [ "$v" = "$cur" ] || [ "$v" = "$prev" ]; then continue; fi
        if [ "$kept" -lt "$KEEP" ]; then kept=$((kept + 1)); else rm -rf "${RELS:?}/$v"; fi
    done
    # what an interrupted install left behind (the lock keeps out another)
    for s in "$RELS"/.stage.*; do
        [ "$s" = "${STAGE:-}" ] || rm -rf "$s"
    done
}

# ~/.local/bin/erssi -> $PREFIX/bin/erssi (bin_link_wanted); LINKED=1 when made
link_bin() {
    LINKED=0
    bin_link_wanted || return 0
    mkdir -p "$HOME/.local/bin"
    l="$HOME/.local/bin/erssi"
    # a link (ours from before, or to anywhere) is replaced; a file or a
    # directory of the user's own there is kept
    if [ -L "$l" ] || [ ! -e "$l" ]; then
        rm -f "$l"
        ln -s "$PREFIX/bin/erssi" "$l"
        LINKED=1
    else
        warn "$(tilde "$l") exists and is not a link - kept; this erssi is $(tilde "$PREFIX/bin/erssi")"
    fi
}
write_conf() {   # METHOD (binary|source)
    {
        echo "# written by shellter-install.sh - the settings erssi --update uses"
        echo "METHOD=$1"
        echo "REF=$REF"
        echo "VERSION=$VERSION"
        echo "COMMIT=$HEAD_SHA"
        [ "$1" != binary ] || echo "PLATFORM=$PLATFORM"
        echo "SRC=$SRC"
        echo "REPO=$REPO"
        echo "TESTS=$RUN_TESTS"
        echo "CLEAN=$CLEAN"
        echo "USE=$USE_SAVED"
        echo "KEEP=$KEEP"
        [ -z "$BINARY_BASE_GIVEN" ] || echo "BINARY_BASE=$BINARY_BASE_GIVEN"
    } > "$CONF.tmp" && chmod 600 "$CONF.tmp" && mv "$CONF.tmp" "$CONF"
}
summary() {
    IN_PATH=1
    if [ "$LINKED" = 1 ]; then
        # shellcheck disable=SC2088 # shown, not run
        case ":$PATH:" in
            *":$HOME/.local/bin:"*) RUN=erssi ;;
            *) RUN="~/.local/bin/erssi"; IN_PATH=0 ;;
        esac
    else
        case ":$PATH:" in
            *":$PREFIX/bin:"*) RUN=erssi ;;
            *) RUN=$(tilde "$PREFIX/bin/erssi"); IN_PATH=0 ;;
        esac
    fi
    printf '\n'
    if [ -n "$INSTALLED" ] && [ "$INSTALLED" != "$VERSION" ]; then
        printf '  %s%sUpdated: %s %s %s%s  %s(%s)%s\n\n' "$B" "$G" "$INSTALLED" "$S_ARROW" "$VERSION" "$N" "$D" "$(elapsed $(($(now) - T_START)))" "$N"
        field "running" "in a running erssi, /upgrade loads $VERSION (TLS connections reconnect once)"
    else
        printf '  %s%serssi Shellter Edition %s is installed%s  %s(%s)%s\n\n' "$B" "$G" "$VERSION" "$N" "$D" "$(elapsed $(($(now) - T_START)))" "$N"
        field "start" "$RUN   (inside tmux, so it keeps running when you log out)"
        field "connect" "/connect IRCnet"
    fi
    field "update" "erssi --update"
    if [ "$IN_PATH" = 0 ] && [ "$LINKED" = 1 ]; then
        # shellcheck disable=SC2088 # shown, not run
        field "path" "~/.local/bin is not in PATH - add to ~/.profile: export PATH=\"\$HOME/.local/bin:\$PATH\""
    elif [ "$IN_PATH" = 0 ]; then
        field "path" "$(tilde "$PREFIX/bin") is not in PATH"
    fi
    field "log" "$(tilde "$LOG")"
    printf '\n'
}

# ── the binary path ──────────────────────────────────────────────────
# A release with a package for this platform is installed from it: no
# build tools, no build. binary_install returns when there is none, or this
# system cannot use it, and the source build runs instead (with --binary
# the installer stops there). A package whose signature, checksum or
# MANIFEST does not verify stops the installer with nothing changed.
bin_cleanup() {
    [ -z "${STAGE:-}" ] || rm -rf "$STAGE"
    [ -z "${BWORK:-}" ] || rm -rf "$BWORK"
    [ -z "${BLOCK:-}" ] || rmdir "$BLOCK" 2>/dev/null
    [ -z "${BLOG:-}" ] || rm -f "$BLOG"
    STAGE="" BWORK="" BLOCK="" BLOG=""
}
binary_fallback() {   # why
    bin_cleanup
    # the releases directory this run made, if it holds nothing
    [ "$DRY_RUN" = 1 ] || rmdir "$PREFIX/releases" 2>/dev/null || true
    trap - EXIT
    [ "$USE" != binary ] || die "no binary install (--binary): $1"
    if [ "$IN_STEP" = 1 ]; then skip_ "$1"; else warn "$1"; fi
    warn "building from source instead, in $(tilde "$SRC")"
    SRC_NAMED=1
    printf '\n'
    STEP=0 STEPS=6
}
binary_install() {
    # silently the source build: what has no packages (a branch or a
    # commit, a repository of one's own without --binary-base)
    case "$REF" in
        ''|shellter-v*) ;;
        *) [ "$USE" != binary ] || die "--binary installs releases only, not --ref $REF"; return 0 ;;
    esac
    if [ -z "$BINARY_BASE" ]; then
        [ "$USE" != binary ] || die "--binary: $REPO has no binary packages here - give --binary-base URL"
        return 0
    fi
    PLATFORM=$(platform_id) || PLATFORM=""
    PERL_V=$(perl_version)
    why=""
    if [ -z "$PLATFORM" ]; then why="no binary package: the platform cannot be told"
    elif [ -z "$PERL_V" ]; then why="no binary package without perl"
    elif ! have ssh-keygen; then why="no ssh-keygen to check the signature of a binary package"
    elif ! can_check_sig; then why="this ssh-keygen cannot check SSH signatures (OpenSSH 8.1 or newer is needed) - no binary package"
    elif ! have sha256sum && ! have sha256 && ! have shasum; then why="no sha256sum to check a binary package"
    elif ! can_unxz; then why="no xz to unpack a binary package"
    else
        case "$BINARY_BASE" in
            http*) have curl || have fetch || why="neither curl nor fetch to download a binary package" ;;
        esac
    fi
    if [ -n "$why" ]; then binary_fallback "$why"; return 0; fi

    trap 'bin_cleanup' EXIT
    if [ "$DRY_RUN" = 1 ]; then
        BLOG=$(mktemp) || die "cannot create a temporary file"
        LOG=$BLOG
    else
        mkdir -p "$PREFIX" 2>/dev/null; [ -w "$PREFIX" ] || die "cannot write to $PREFIX"
        # one installer at a time on this prefix
        mkdir "$PREFIX/.install.lock" 2>/dev/null ||
            die "another installer is using $PREFIX (if not, remove $PREFIX/.install.lock)"
        BLOCK="$PREFIX/.install.lock"
        mkdir -p "$STATE"
        LOG="$STATE/install.log"
        rm -f "$LOG"
        ( umask 077; : > "$LOG" )
    fi
    STEP=0 STEPS=5
    step "Checking the system"
    done_ "$PLATFORM $S_SEP perl $PERL_V"

    TAG_COMMIT=""
    if [ "$UPDATE" = 1 ]; then
        # the newest signed release tag newer than the installed one, as for
        # the source build; the package must then name its commit
        BWORK=$(mktemp -d) || die "cannot create a temporary directory"
        git init -q "$BWORK" || die "cannot create a temporary git repository"
        find_update "$BWORK"
        # the source build takes this tag object if it comes to that
        FOUND_UPDATE=1
        TAG_COMMIT=$(git -C "$BWORK" rev-parse "$TAG_OBJ^{commit}") || die "cannot read $REF"
    else
        step "Choosing the release"
        [ -n "$REF" ] || REF=$(printf '%s\n' "$RELEASES" | awk 'NF >= 2 {r = $1} END {print r}')
        [ -n "$REF" ] || die "no release recorded in this installer - use --ref"
        done_ "$REF"
    fi
    V=${REF#shellter-v}
    case "$V" in
        [0-9]*.[0-9]*.[0-9]*) case "$V" in *[!0-9.]*|*..*|*.) die "not a release name: $REF" ;; esac ;;
        *) die "not a release name: $REF" ;;
    esac
    PIN=$(printf '%s\n' "$RELEASES" | awk -v r="$REF" '$1 == r {print $2}')
    if [ -n "$PIN" ] && [ -n "$TAG_COMMIT" ] && [ "$TAG_COMMIT" != "$PIN" ]; then
        die "$REF points to $TAG_COMMIT, but this installer expects $PIN - refusing to install (download the installer again, or report it)"
    fi
    EXPECT=${PIN:-$TAG_COMMIT}

    step "Downloading"
    ASSET="erssi-$V-$PLATFORM.tar.xz"
    if [ "$DRY_RUN" = 1 ]; then
        STAGE=$(mktemp -d) || die "cannot create a temporary directory"
    else
        mkdir -p "$PREFIX/releases" || die "cannot create $PREFIX/releases"
        STAGE=$(mktemp -d "$PREFIX/releases/.stage.XXXXXX") || die "cannot create a directory in $PREFIX/releases"
    fi
    if ! download SHA256SUMS "$REF" "$STAGE/SHA256SUMS" || ! download SHA256SUMS.sig "$REF" "$STAGE/SHA256SUMS.sig"; then
        binary_fallback "no binary packages found for $REF"; return 0
    fi
    # not signed yet: only enough to tell there is no package for this
    # platform, and build from source (which blocking the download would
    # do as well); nothing else in it counts before the signature
    if [ -z "$(sum_of "$STAGE/SHA256SUMS" "$ASSET")" ]; then
        binary_fallback "no package for $PLATFORM in $REF"; return 0
    fi
    verify_sums "$STAGE" ||
        die "the SHA256SUMS of $REF does not carry a valid signature by the erssi Shellter release key - refusing to install, nothing was changed (report it)"
    want=$(sum_of "$STAGE/SHA256SUMS" "$ASSET")
    # listed but not there (not uploaded yet, a network error): no
    # package, as for a missing line; one that arrives must match
    if ! download "$ASSET" "$REF" "$STAGE/$ASSET"; then
        binary_fallback "$ASSET is listed in the signed SHA256SUMS but cannot be downloaded"; return 0
    fi
    got=$(sha256_of "$STAGE/$ASSET")
    [ "$got" = "$want" ] ||
        die "$ASSET does not match the signed SHA256SUMS - refusing to install, nothing was changed (report it)"
    done_ "$ASSET $S_SEP signed $S_SEP sha256 verified"

    step "Checking the package"
    TOP="erssi-$V-$PLATFORM"
    tar -tJf "$STAGE/$ASSET" > "$STAGE/list" 2>> "$LOG" || die "cannot read $ASSET"
    # every name below the one top directory (a leading ./ aside), none
    # going up
    awk -v t="$TOP" '{ sub(/^\.\//, "") } $0 == "" || $0 == "." { next }
        $0 != t && $0 != t "/" && index($0, t "/") != 1 { bad = 1 }
        /(^|\/)\.\.(\/|$)/ { bad = 1 } END { exit bad }' "$STAGE/list" ||
        die "$ASSET holds files outside $TOP/ - refusing to install, nothing was changed (report it)"
    # a hard link only to a file of the package
    tar -tvJf "$STAGE/$ASSET" 2>> "$LOG" | awk -v t="$TOP" '/^h/ {
            i = index($0, " link to "); if (!i) { bad = 1; next }
            x = substr($0, i + 9); sub(/^\.\//, "", x)
            if (index(x, t "/") != 1 || x ~ /(^|\/)\.\.(\/|$)/) bad = 1 }
        END { exit bad }' ||
        die "$ASSET holds a hard link to a file outside $TOP/ - refusing to install, nothing was changed (report it)"
    mkdir "$STAGE/u" || die "cannot create a directory in $STAGE"
    # owners and modes are the installing user's (and umask's), not the
    # archive's: as root, tar would otherwise restore them
    run_logged tar --no-same-owner --no-same-permissions -xJf "$STAGE/$ASSET" -C "$STAGE/u" ||
        die_log "unpacking $ASSET failed"
    # a symbolic link only within the package: its target neither absolute
    # nor with a .. anywhere. Read as text, one link at a time, a chain
    # (a -> ., b -> a/.., c -> b/..) would get out; without / and .. no
    # link or chain of links can leave the tree.
    find "$STAGE/u" -type l | while IFS= read -r l; do
            printf '%s\n' "$(readlink "$l")"
        done | awk '$0 == "" || substr($0, 1, 1) == "/" || $0 ~ /(^|\/)\.\.(\/|$)/ { bad = 1 }
        END { exit bad }' ||
        die "$ASSET holds a symbolic link whose target is absolute or has a .. in it - refusing to install, nothing was changed (report it)"
    T="$STAGE/u/$TOP"
    [ -f "$T/MANIFEST" ] || die "$ASSET has no MANIFEST - refusing to install, nothing was changed (report it)"
    for k in version tag platform commit; do
        case "$k" in
            version) w=$V ;; tag) w=$REF ;; platform) w=$PLATFORM ;; commit) w=$EXPECT ;;
        esac
        g=$(mf "$T/MANIFEST" "$k") || g=""
        if [ "$k" = commit ] && [ -z "$w" ]; then
            # a release this installer does not record: the signature
            # vouches for the package, and the commit is at least one
            case "$g" in *[!0-9a-f]*) g="" ;; esac
            [ ${#g} -eq 40 ] && continue
            w="a full commit SHA"
        fi
        [ "$g" = "$w" ] ||
            die "the MANIFEST of $ASSET says $k=${g:-(none)}, not $w - refusing to install, nothing was changed (report it)"
    done
    HEAD_SHA=$(mf "$T/MANIFEST" commit)
    mp=$(mf "$T/MANIFEST" perl_version) || mp=""
    if [ "$mp" != "$PERL_V" ]; then
        binary_fallback "the package is built for perl ${mp:-(none)}, this system has perl $PERL_V"; return 0
    fi
    if [ ! -f "$T/bin/erssi" ] || [ ! -x "$T/bin/erssi" ]; then
        die "$ASSET has no bin/erssi - refusing to install, nothing was changed (report it)"
    fi
    miss=$(missing_libs "$T")
    if [ -n "$miss" ]; then
        # shellcheck disable=SC2086 # library list
        hint=$(lib_hint "$PLATFORM" $miss)
        warn "libraries missing: $miss"
        printf '%s\n' "$hint" | while IFS= read -r h; do warn "install: $h"; done
        binary_fallback "the package needs libraries this system lacks"; return 0
    fi
    # the packaged erssi starts here and is the release it claims to be
    if ! rv=$("$T/bin/erssi" --version 2>> "$LOG"); then
        binary_fallback "the packaged erssi does not start on this system (log: $(tilde "$LOG"))"; return 0
    fi
    [ "$(printf '%s\n' "$rv" | awk 'NR == 1 {print $2}')" = "$V" ] ||
        die "the packaged erssi says \"$rv\", not $V - refusing to install, nothing was changed (report it)"
    # and finds its files where it is (a relocatable build), not where it
    # was built
    rp=$("$T/bin/erssi" --print-prefix 2>> "$LOG") && rp=$(cd "$rp" 2>/dev/null && pwd -P) || rp=""
    if [ -z "$rp" ] || [ "$rp" != "$(cd "$T" && pwd -P)" ]; then
        binary_fallback "the packaged erssi does not find its own files (--print-prefix: ${rp:-nothing})"; return 0
    fi
    if [ -n "$PIN" ]; then c="commit recorded"; elif [ -n "$TAG_COMMIT" ]; then c="commit of the signed tag"; else c="release not recorded here"; fi
    done_ "$(echo "$HEAD_SHA" | cut -c1-8) $S_SEP $c $S_SEP libraries present"

    RELS="$PREFIX/releases"
    cur=$(link_release current)
    if [ "$DRY_RUN" = 1 ]; then
        step "Installing"
        skip_ "dry run"
        printf '\n  Dry run - nothing was changed. The install would:\n'
        printf '    unpack %s into %s\n' "$ASSET" "$(tilde "$RELS/$V")"
        if [ -n "$cur" ] && [ "$cur" != "$V" ]; then
            printf '    switch %s from releases/%s to releases/%s (--rollback returns to %s)\n' "$(tilde "$PREFIX/current")" "$cur" "$V" "$cur"
        else
            printf '    point %s to releases/%s\n' "$(tilde "$PREFIX/current")" "$V"
        fi
        printf '    link %s to ../current/bin/erssi\n' "$(tilde "$PREFIX/bin/erssi")"
        printf '    keep %s old release(s) besides the one in use\n' "$KEEP"
        printf '    record METHOD=binary VERSION=%s PLATFORM=%s in %s\n\n' "$V" "$PLATFORM" "$(tilde "$CONF")"
        exit 0
    fi

    # Installing: the new tree goes into releases/<version> with one rename,
    # then current is switched in one step; until then the release in use is
    # untouched, and an interrupted install leaves only its .stage.
    step "Installing"
    REUSED=0
    if [ -e "$RELS/$V" ] || [ -L "$RELS/$V" ]; then
        if [ "$cur" = "$V" ]; then
            # the release in use: not replaced under a running erssi; the
            # same package stays as it is
            cmp -s "$RELS/$V/MANIFEST" "$T/MANIFEST" ||
                die "$(tilde "$RELS/$V") is in use but holds another package - nothing was changed; --rollback first, or remove it"
            REUSED=1
        else
            mv "$RELS/$V" "$STAGE/old" || die "cannot move $(tilde "$RELS/$V") aside - nothing was changed"
        fi
    fi
    if [ "$REUSED" = 0 ] && ! mv "$T" "$RELS/$V"; then
        [ ! -d "$STAGE/old" ] || mv "$STAGE/old" "$RELS/$V"
        die "cannot move the package to $(tilde "$RELS/$V") - nothing was changed"
    fi
    if [ "$cur" != "$V" ]; then
        switch_link "$PREFIX/current" "releases/$V" || die "cannot switch $(tilde "$PREFIX/current") to releases/$V"
        [ -z "$cur" ] || switch_link "$PREFIX/previous" "releases/$cur" ||
            warn "cannot link $(tilde "$PREFIX/previous") - --rollback will not find releases/$cur"
    fi
    mkdir -p "$PREFIX/bin" "$PREFIX/share/irssi"
    if [ "$(readlink "$PREFIX/bin/erssi" 2>/dev/null)" != ../current/bin/erssi ]; then
        [ ! -d "$PREFIX/bin/erssi" ] || [ -L "$PREFIX/bin/erssi" ] || die "$PREFIX/bin/erssi is a directory"
        switch_link "$PREFIX/bin/erssi" ../current/bin/erssi || die "cannot link $(tilde "$PREFIX/bin/erssi")"
    fi
    prune_releases
    # erssi --update and --rollback run the installer of the release
    [ ! -f "$RELS/$V/share/irssi/shellter-install.sh" ] ||
        cp "$RELS/$V/share/irssi/shellter-install.sh" "$PREFIX/share/irssi/shellter-install.sh" 2>/dev/null || true
    VERSION=$V
    write_conf binary || die "cannot write $CONF"
    link_bin
    bin_cleanup
    trap - EXIT
    if [ "$REUSED" = 1 ]; then done_ "$(tilde "$PREFIX") $S_SEP releases/$V was already there"; else done_ "$(tilde "$PREFIX") $S_SEP releases/$V"; fi
    summary
    exit 0
}

# step "Finding the release" of an update, in the git repository $1: the
# newest release signed by the release key and newer than the installed
# one -> REF, TAG_OBJ, NEWEST; exits when there is nothing to do, and
# after --check
find_update() {
    WORK=$1
    step "Finding the release"
    # the current installer instead: it records the newest release's commit
    # and checks that without a signature check (this copy cannot know a
    # release newer than its own)
    if ! can_verify; then
        p=""; [ "$PREFIX" = "$HOME/.local/opt/erssi" ] || p=" -s -- --prefix $PREFIX"
        die "release signatures cannot be checked here (git 2.34+ and ssh-keygen are needed) - install the newest release with the current installer, which checks its recorded commit: curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh$p"
    fi
    case "$INSTALLED_REF" in
        ''|shellter-v*)
            # a release: a newer one, and its own tag checked
            rc=0; FOUND=$(newest_signed "$WORK" "$INSTALLED" "$INSTALLED_COMMIT") || rc=$?
            FROM_BRANCH="" ;;
        *)
            # a branch or a commit: main keeps the version of the last
            # release until the next one, so its commit says nothing about
            # the release of that version - the newest signed release, and
            # only a newer version counts
            rc=0; FOUND=$(newest_signed "$WORK" 0) || rc=$?
            FROM_BRANCH=$INSTALLED_REF ;;
    esac
    [ "$rc" != 2 ] || die "cannot reach $REPO"
    [ "$rc" != 3 ] || die "cannot download $FOUND from $REPO - nothing was changed; try again later"
    if [ "$rc" != 0 ]; then
        [ -n "$FROM_BRANCH" ] || die "no signed release newer than $INSTALLED found on $REPO"
        die "no signed release found on $REPO"
    fi
    REF=${FOUND%% *}; TAG_OBJ=${FOUND#* }
    if [ -n "$REF" ]; then
        NEWEST=${REF#shellter-v}
        done_ "$REF $S_SEP signed by the erssi Shellter release key"
    else
        NEWEST=$INSTALLED
        done_ "none newer than $INSTALLED"
    fi
    if [ -n "$FROM_BRANCH" ]; then
        printf '\n  Installed from %s; newest release is %s' "$FROM_BRANCH" "$NEWEST"
        if ! newer "$NEWEST" "$INSTALLED"; then
            printf ', not newer than the installed %s - nothing to do.\n\n' "$INSTALLED"
            exit 0
        fi
        printf '.\n'
    elif [ -z "$REF" ]; then
        printf '\n  %serssi Shellter Edition %s is up to date%s (the newest signed release).\n\n' "$B" "$INSTALLED" "$N"
        exit 0
    fi
    if [ "$CHECK" = 1 ]; then
        printf '\n  %sUpdate available: %s %s %s%s\n  Run: erssi --update\n\n' "$B" "$INSTALLED" "$S_ARROW" "$NEWEST" "$N"
        exit 0
    fi
}

# --rollback: current back to the release it was switched from (previous),
# and previous to the one it leaves, so a second --rollback undoes it
do_rollback() {
    header "rollback"
    field "prefix" "$(tilde "$PREFIX")"
    printf '\n'
    RELS="$PREFIX/releases"
    cur=$(link_release current) prev=$(link_release previous)
    if [ -z "$cur" ] || [ "$(conf_get METHOD)" != binary ]; then
        die "no binary install in $PREFIX - --rollback switches between the releases of a binary install"
    fi
    if [ -z "$prev" ] || [ "$prev" = "$cur" ] || [ ! -f "$RELS/$prev/bin/erssi" ] || [ ! -f "$RELS/$prev/MANIFEST" ]; then
        die "no previous release to roll back to in $(tilde "$RELS") - nothing was changed"
    fi
    if [ "$DRY_RUN" = 1 ]; then
        printf '  Dry run - nothing was changed. --rollback would switch %s from releases/%s to releases/%s.\n\n' \
            "$(tilde "$PREFIX/current")" "$cur" "$prev"
        exit 0
    fi
    mkdir "$PREFIX/.install.lock" 2>/dev/null ||
        die "another installer is using $PREFIX (if not, remove $PREFIX/.install.lock)"
    trap 'rmdir "$PREFIX/.install.lock" 2>/dev/null' EXIT
    LOG=/dev/null
    switch_link "$PREFIX/current" "releases/$prev" || die "cannot switch $(tilde "$PREFIX/current") - nothing was changed"
    switch_link "$PREFIX/previous" "releases/$cur" || warn "cannot link $(tilde "$PREFIX/previous") to releases/$cur"
    [ "$(readlink "$PREFIX/bin/erssi" 2>/dev/null)" = ../current/bin/erssi ] ||
        switch_link "$PREFIX/bin/erssi" ../current/bin/erssi || warn "cannot link $(tilde "$PREFIX/bin/erssi")"
    # what is installed now, from the MANIFEST it was installed with
    VERSION=$prev REF="shellter-v$prev"
    HEAD_SHA=$(mf "$RELS/$prev/MANIFEST" commit) || HEAD_SHA=""
    PLATFORM=$(mf "$RELS/$prev/MANIFEST" platform) || PLATFORM=""
    write_conf binary || warn "cannot write $CONF"
    printf '  %s%sRolled back: %s %s %s%s\n\n' "$B" "$G" "$cur" "$S_ARROW" "$prev" "$N"
    field "running" "in a running erssi, /upgrade loads $prev (TLS connections reconnect once)"
    field "again" "--rollback once more returns to $cur"
    printf '\n'
    exit 0
}
# every entry of the directory $1 is one of the names $2 (when it exists)
only_erssi() {
    for e in "$1"/* "$1"/.[!.]* "$1"/..?*; do
        [ -e "$e" ] || [ -L "$e" ] || continue
        case " $2 " in
            *" ${e##*/} "*) ;;
            *) die "$1 holds ${e##*/}, which erssi did not install - nothing was removed; remove erssi from it by hand" ;;
        esac
    done
}
# --uninstall: the prefix and the ~/.local/bin/erssi link to it go; ~/.erssi
# (configuration, logs, keys) and the source directory stay. Only a prefix
# this installer installed, holding nothing else than erssi, is removed.
do_uninstall() {
    header "uninstall"
    field "prefix" "$(tilde "$PREFIX")"
    printf '\n'
    [ -f "$CONF" ] ||
        die "no erssi installed by shellter-install.sh in $PREFIX ($(tilde "$CONF") is missing) - nothing was removed"
    p=$(cd "$PREFIX" && pwd -P) || die "cannot open $PREFIX"
    h=$(cd "$HOME" && pwd -P) || die "cannot open $HOME"
    case "$h/" in "${p%/}"/*) die "$PREFIX holds your home - nothing was removed" ;; esac
    case "$p" in /usr|/usr/local|/opt|"$h/.local") die "$PREFIX is shared with other software - nothing was removed; remove erssi from it by hand" ;; esac
    # what erssi installs there, and nothing else
    only_erssi "$PREFIX" "bin etc include lib lib64 libexec share releases current previous .install.lock"
    only_erssi "$PREFIX/bin" "erssi"
    only_erssi "$PREFIX/share" "irssi man doc"
    l="$HOME/.local/bin/erssi"
    unlink=0
    [ ! -L "$l" ] || [ "$(readlink "$l")" != "$PREFIX/bin/erssi" ] || unlink=1
    if [ "$DRY_RUN" = 1 ]; then
        printf '  Dry run - nothing was changed. --uninstall would remove %s' "$(tilde "$PREFIX")"
        [ "$unlink" = 0 ] || printf ' and the link %s' "$(tilde "$l")"
        printf '; %s would stay.\n\n' "$(tilde "$HOME/.erssi")"
        exit 0
    fi
    mkdir "$PREFIX/.install.lock" 2>/dev/null ||
        die "another installer is using $PREFIX (if not, remove $PREFIX/.install.lock)"
    [ "$unlink" = 0 ] || rm -f "$l"
    rm -rf "$PREFIX" || die "cannot remove all of $PREFIX"
    printf '  %s%serssi removed%s from %s.\n\n' "$B" "$G" "$N" "$(tilde "$PREFIX")"
    field "kept" "$(tilde "$HOME/.erssi") (your configuration, logs and keys)"
    [ ! -d "$SRC" ] || field "source" "$(tilde "$SRC") is kept - remove it by hand if you no longer build erssi"
    printf '\n'
    exit 0
}

main() {
unset CDPATH        # cd in $(...) must print nothing
PREFIX="$HOME/.local/opt/erssi"
SRC="" REF="" REPO="" RUN_TESTS="" CLEAN="" UPDATE=0 CHECK=0 YES=0
USE="" KEEP="" BINARY_BASE="" DRY_RUN=0 ACTION=""
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) optval "$1" "${2:-}" DIR; PREFIX=$2; shift 2 ;;
        --src) optval "$1" "${2:-}" DIR; SRC=$2; shift 2 ;;
        --ref) optval "$1" "${2:-}" TAG; REF=$2; shift 2 ;;
        --repo) optval "$1" "${2:-}" URL; REPO=$2; shift 2 ;;
        --no-test) RUN_TESTS=0; shift ;;
        --test) RUN_TESTS=1; shift ;;
        --clean) CLEAN=1; shift ;;
        --no-clean) CLEAN=0; shift ;;
        --update) UPDATE=1; shift ;;
        --check) CHECK=1; shift ;;
        --yes|-y) YES=1; shift ;;
        --binary) USE=binary; shift ;;
        --source) USE=source; shift ;;
        --binary-base) optval "$1" "${2:-}" URL; BINARY_BASE=$2; shift 2 ;;
        --keep) optval "$1" "${2:-}" 2; KEEP=$2; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        --rollback) ACTION=rollback; shift ;;
        --uninstall) ACTION=uninstall; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "shellter-install.sh: unknown option $1 (--help)" >&2; exit 2 ;;
    esac
done
# meson takes only an absolute prefix (it would fail after the download), and
# the ~/.local/bin link and the conf need one that does not depend on the
# current directory
case "$PREFIX" in
    /*) ;;
    *) echo "shellter-install.sh: --prefix needs an absolute path, e.g. --prefix $HOME/opt/erssi (--help)" >&2; exit 2 ;;
esac
[ "$CHECK" = 0 ] || [ "$UPDATE" = 1 ] || { echo "shellter-install.sh: --check goes with --update (--help)" >&2; exit 2; }
[ "$UPDATE" = 0 ] || [ -z "$REF" ] || { echo "shellter-install.sh: --update takes the newest signed release; for a given one use --ref without --update" >&2; exit 2; }
if [ -n "$ACTION" ] && { [ "$UPDATE$CHECK" != 00 ] || [ -n "$REF" ]; }; then
    echo "shellter-install.sh: --$ACTION goes with --prefix and --dry-run only (--help)" >&2; exit 2
fi
case "$KEEP" in *[!0-9]*) echo "shellter-install.sh: --keep needs a number, e.g. --keep 2 (--help)" >&2; exit 2 ;; esac
# the release just switched away from is the one every running erssi was
# started from: its help, modules, scripts and themes are needed until
# /upgrade, so it always stays
if [ -n "$KEEP" ] && [ "$KEEP" -lt 1 ]; then
    echo "shellter-install.sh: --keep must be at least 1 - a running erssi uses the release it was started from until /upgrade (--help)" >&2; exit 2
fi
case "$BINARY_BASE" in
    ''|https://*|http://*|file:///*|/*) ;;
    *) echo "shellter-install.sh: --binary-base needs a URL or an absolute directory (--help)" >&2; exit 2 ;;
esac
# erssi --update of a binary install names the prefix of its release,
# <prefix>/releases/<version>: the install is the prefix above it
PREFIX=${PREFIX%/}
case "$PREFIX" in
    */releases/*)
        t=${PREFIX%/releases/*}
        [ "$PREFIX" != "$t/releases/${PREFIX##*/}" ] || [ ! -L "$t/current" ] || PREFIX=$t ;;
esac

ui_init
JOB=""
# Ctrl-C: stop the step running in the background too (Python and meson
# started from a script ignore SIGINT), then leave
trap 'if [ -n "$JOB" ]; then kill -TERM "$JOB" 2>/dev/null; wait "$JOB" 2>/dev/null; fi; [ "$IN_STEP" = 1 ] && printf "\n"; printf "\n  Interrupted.\n\n" >&2; exit 130' HUP INT TERM
# shellcheck disable=SC2012 # find -printf is not POSIX; ls -n is
owner() { ls -ldn "$1" 2>/dev/null | awk '{print $3}'; }
ROOT=0
[ "$(id -u)" = 0 ] && ROOT=1
if [ "$ROOT" = 1 ]; then
    # su without -, sudo -E: HOME of a user, whose files root would run
    [ "$(owner "$HOME")" = 0 ] ||
        die "running as root with the HOME of another user ($HOME) - use sudo -i or su - (root's own home), or run the installer as that user"
    for d in "$PREFIX" "${SRC:-}"; do
        [ -z "$d" ] || [ ! -e "$d" ] || [ "$(owner "$d")" = 0 ] || die "$d belongs to another user - as root, use a directory of root's"
    done
fi
# the settings of the previous install, unless given again
CONF="$PREFIX/share/irssi/shellter-install.conf"
[ -n "$SRC" ] || SRC=$(conf_get SRC)
[ -n "$REPO" ] || REPO=$(conf_get REPO)
[ -n "$RUN_TESTS" ] || RUN_TESTS=$(conf_get TESTS)
[ -n "$CLEAN" ] || CLEAN=$(conf_get CLEAN)
: "${SRC:=$HOME/.local/src/erssi}" "${REPO:=$REPO_DEFAULT}" "${RUN_TESTS:=1}" "${CLEAN:=0}"
# --source is kept for erssi --update; --binary holds for this run only
[ -n "$USE" ] || USE=$(conf_get USE)
case "$USE" in binary|source) ;; *) USE=auto ;; esac
USE_SAVED=auto; [ "$USE" != source ] || USE_SAVED=source
[ -n "$KEEP" ] || KEEP=$(conf_get KEEP)
case "$KEEP" in ''|*[!0-9]*) KEEP=2 ;; esac
[ "$KEEP" -ge 1 ] || KEEP=1
[ -n "$BINARY_BASE" ] || BINARY_BASE=$(conf_get BINARY_BASE)
case "$BINARY_BASE" in ''|https://*|http://*|file:///*|/*) ;; *) BINARY_BASE="" ;; esac
BINARY_BASE_GIVEN=$BINARY_BASE
# the packages of the default repository; another one has none of its own
[ -n "$BINARY_BASE" ] || [ "$REPO" != "$REPO_DEFAULT" ] || BINARY_BASE=$BINARY_BASE_DEFAULT
for v in "$SRC" "$REPO" "$PREFIX" "$BINARY_BASE"; do
    case "$v" in
        -*|*"
"*) die "invalid directory or repository: $v" ;;
    esac
done
case "$ACTION" in
    rollback) do_rollback ;;
    uninstall) do_uninstall ;;
esac

INSTALLED=""
[ -x "$PREFIX/bin/erssi" ] && INSTALLED=$("$PREFIX/bin/erssi" --version 2>/dev/null | awk '{print $2}')
# the commit it was built from; installs before COMMIT was recorded: the
# checkout in the source directory, if it is still there
INSTALLED_COMMIT=$(conf_get COMMIT)
[ -n "$INSTALLED_COMMIT" ] || [ ! -d "$SRC/.git" ] ||
    INSTALLED_COMMIT=$(git -C "$SRC" rev-parse -q --verify HEAD 2>/dev/null) || INSTALLED_COMMIT=""
# what it was installed from: a release tag, or a branch or commit (--ref).
# Nothing recorded (installed before 2026.10.3): a release, as every
# --update and every install without --ref made one
INSTALLED_REF=$(conf_get REF)

if [ "$UPDATE" = 1 ]; then
    OTHER=$(conf_get UPDATE_COMMAND)
    if [ -n "$OTHER" ]; then
        header "update"
        field "updater" "$OTHER"
        if [ "$CHECK" = 1 ]; then
            printf '\n  This erssi is updated with: %s\n\n' "$OTHER"
            exit 0
        fi
        printf '\n'
        exec sh -c "$OTHER"
    fi
    [ -n "$INSTALLED" ] || die "no erssi in $PREFIX to update - install it first: curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh"
fi

# The system and its package manager: for step 1 of the source build, and
# for the libraries a binary package needs. What erssi needs to build, as
# features; each maps to the package names of the system's package manager.
# As root the missing packages are listed and, after a yes (or --yes),
# installed; on a user account the list and the command for the
# administrator are shown.
OS=$(uname -s)
OS_RELEASE=${SHELLTER_OS_RELEASE:-/etc/os-release}
DISTRO=""
# ID and ID_LIKE only, read as text (the file is not run)
[ -r "$OS_RELEASE" ] && DISTRO=$(sed -n -e 's/^ID=//p' -e 's/^ID_LIKE=//p' "$OS_RELEASE" | tr -d '"' | tr '\n' ' ')
PM=""
case "$OS $DISTRO" in
    FreeBSD*) PM=pkg ;;
    *debian*|*ubuntu*) PM=apt ;;
    *fedora*|*rhel*|*centos*|*rocky*|*alma*) PM=dnf ;;
    *alpine*) PM=apk ;;
    *arch*|*manjaro*) PM=pacman ;;
    *suse*) PM=zypper ;;
esac
packages() {   # feature -> package names for $PM
    case "$PM:$1" in
        *:git) echo git ;;
        apt:cc|dnf:cc|pacman:cc|zypper:cc) echo gcc ;;
        apk:cc) echo build-base ;;
        pkg:cc) echo "" ;;
        apt:pkgconf|apk:pkgconf|pacman:pkgconf|pkg:pkgconf) echo pkgconf ;;
        dnf:pkgconf) echo pkgconf-pkg-config ;;
        zypper:pkgconf) echo pkg-config ;;
        *:meson) echo meson ;;
        apt:ninja|dnf:ninja) echo ninja-build ;;
        *:ninja) echo ninja ;;
        apt:glib) echo libglib2.0-dev ;;
        dnf:glib|zypper:glib) echo glib2-devel ;;
        apk:glib) echo glib-dev ;;
        pacman:glib) echo glib2 ;;
        pkg:glib) echo glib ;;
        apt:openssl) echo libssl-dev ;;
        dnf:openssl) echo openssl-devel ;;
        apk:openssl) echo openssl-dev ;;
        zypper:openssl) echo libopenssl-devel ;;
        pacman:openssl|pkg:openssl) echo openssl ;;
        pkg:perl|pkg:perlembed) echo perl5 ;;
        *:perl) echo perl ;;
        apt:perlembed) echo libperl-dev ;;
        dnf:perlembed) echo "perl-devel perl-ExtUtils-Embed" ;;
        apk:perlembed) echo perl-dev ;;
        pacman:perlembed|zypper:perlembed) echo perl ;;
        apt:utf8proc) echo libutf8proc-dev ;;
        dnf:utf8proc|zypper:utf8proc) echo utf8proc-devel ;;
        apk:utf8proc) echo utf8proc-dev ;;
        pacman:utf8proc) echo libutf8proc ;;
        pkg:utf8proc) echo utf8proc ;;
        apt:otr) echo "libotr5-dev libgcrypt20-dev" ;;
        dnf:otr|zypper:otr) echo "libotr-devel libgcrypt-devel" ;;
        apk:otr) echo "libotr-dev libgcrypt-dev" ;;
        pacman:otr|pkg:otr) echo "libotr libgcrypt" ;;
        apt:preview) echo "libcurl4-openssl-dev libchafa-dev" ;;
        dnf:preview|zypper:preview) echo "libcurl-devel chafa-devel" ;;
        apk:preview) echo "curl-dev chafa-dev" ;;
        pacman:preview|pkg:preview) echo "curl chafa" ;;
    esac
}
pm_install_cmd() {   # the command that installs "$@" with $PM
    case "$PM" in
        apt) echo "apt-get install -y -q --no-install-recommends $*" ;;
        dnf) echo "dnf install -y $*" ;;
        apk) echo "apk add --no-cache $*" ;;
        pacman) echo "pacman -S --needed --noconfirm $*" ;;
        zypper) echo "zypper --non-interactive install $*" ;;
        pkg) echo "pkg install -y $*" ;;
    esac
}
pm_hint() {   # what a user tells the administrator
    case "$PM" in
        apt) echo "sudo apt install $*" ;;
        pkg) echo "pkg install $*   (as root)" ;;
        "") echo "install the development files of: $*" ;;
        *) echo "sudo $(pm_install_cmd "$@" | sed 's/ -y//; s/ --noconfirm//; s/ --non-interactive//')" ;;
    esac
}
# root: never a user's XDG_STATE_HOME (su without -)
if [ "$ROOT" = 1 ]; then STATE="$HOME/.local/state/erssi"; else STATE="${XDG_STATE_HOME:-$HOME/.local/state}/erssi"; fi

T_START=$(now)
if [ "$UPDATE" = 1 ]; then header "update"; else header "install"; fi
field "prefix" "$(tilde "$PREFIX")"
# the source checkout only where it is used for sure; a fallback from
# a package names it then
if [ "$USE" = source ]; then field "source" "$(tilde "$SRC")"; SRC_NAMED=1; fi
[ -n "$INSTALLED" ] && field "installed" "$INSTALLED"
printf '\n'

# Without --ref the newest release this installer records - an older one
# than the installed erssi only on purpose (--ref, --rollback): a copy of
# an older installer, such as the one an install leaves in the prefix,
# must not switch back
if [ "$UPDATE" = 0 ] && [ -z "$REF" ] && [ -n "$INSTALLED" ]; then
    r=$(printf '%s\n' "$RELEASES" | awk 'NF >= 2 {r = $1} END {print r}')
    if [ -n "$r" ] && newer "$INSTALLED" "${r#shellter-v}"; then
        printf '  The installed %s is newer than %s, the newest release this installer records - nothing was changed (erssi --update finds newer ones; --ref %s installs that one on purpose).\n\n' \
            "$INSTALLED" "$r" "$r"
        exit 0
    fi
fi
# a release with a package for this system: installed from it (and done),
# otherwise built from source below
if [ "$CHECK" = 0 ] && [ "$USE" != source ]; then binary_install; fi
# no package here and nothing said so: the source checkout, once
if [ "$CHECK" = 0 ] && [ "${SRC_NAMED:-}" != 1 ]; then field "source" "$(tilde "$SRC")"; printf '\n'; fi
if [ "$DRY_RUN" = 1 ] && [ "$CHECK" = 0 ]; then
    printf '  Dry run - nothing was changed. The source build would:\n'
    if [ "$UPDATE" = 1 ]; then
        printf '    find the newest release on %s signed by the release key and newer than %s\n' "$REPO" "$INSTALLED"
    else
        printf '    download %s from %s\n' "${REF:-$(printf '%s\n' "$RELEASES" | awk 'NF >= 2 {r = $1} END {print r}')}" "$REPO"
    fi
    printf '    check it, build it in %s, %s and install it into %s\n\n' "$(tilde "$SRC")" \
        "$(if [ "$RUN_TESTS" = 1 ]; then echo "test it"; else echo "skip the tests"; fi)" "$(tilde "$PREFIX")"
    exit 0
fi

if [ "$CHECK" = 0 ]; then
    # one installer at a time on this source directory, taken before
    # anything is written: the log, the meson/ninja venv ($SRC.venv, step 1)
    # and the checkout
    # the source directory as it is: links resolved once, used from here on
    mkdir -p "$(dirname -- "$SRC")"
    d=$(cd -P -- "$(dirname -- "$SRC")" && pwd -P) || die "cannot open $(dirname -- "$SRC")"
    SRC="${d%/}/$(basename -- "$SRC")"
    if [ -L "$SRC" ]; then
        t=$(cd -P -- "$SRC" 2>/dev/null && pwd -P) || die "$SRC is a link, but not to a directory - use --src DIR"
        SRC=$t
    fi
    # the resolved path: checked as the given one was
    case "$SRC" in
        -*|*"
"*) die "invalid directory: $SRC" ;;
    esac
    safe_path "$(dirname -- "$SRC")"
    mkdir "$SRC.lock" 2>/dev/null || die "another installer is using $SRC (if not, remove $SRC.lock)"
    trap 'rmdir "$SRC.lock" 2>/dev/null' EXIT
    safe_dir "$SRC"
    safe_dir "$SRC.venv"
fi
mkdir -p "$STATE"
if [ "$CHECK" = 1 ]; then
    LOG=$(mktemp)           # --check keeps the log of the last install
    trap 'rm -f "$LOG"' EXIT
else
    LOG="$STATE/install.log"
    rm -f "$LOG"
    ( umask 077; : > "$LOG" )
    mkdir -p "$PREFIX" 2>/dev/null; [ -w "$PREFIX" ] || die "cannot write to $PREFIX"
fi

# ── 1. the system ────────────────────────────────────────────────────
# FreeBSD: the OpenSSL of the base system (headers and libraries in /usr,
# no openssl.pc) is enough; meson finds it without pkg-config. Prints its
# version, or "unknown"; 1 when it is not there or the system is not
# FreeBSD. SHELLTER_BASE_OPENSSL: another base directory (tests).
base_openssl() {
    [ "$OS" = FreeBSD ] || return 1
    b=${SHELLTER_BASE_OPENSSL:-/usr}
    [ -f "$b/include/openssl/ssl.h" ] && [ -f "$b/include/openssl/opensslv.h" ] &&
        [ -e "$b/lib/libssl.so" ] && [ -e "$b/lib/libcrypto.so" ] || return 1
    v=$(sed -n 's/.*OPENSSL_VERSION_TEXT[[:space:]]*"OpenSSL \([^[:space:]"]*\).*/\1/p' "$b/include/openssl/opensslv.h" | head -n 1)
    echo "${v:-unknown}"
}
openssl_version() {
    $PKGCONFIG --modversion openssl 2>/dev/null || echo "$(base_openssl) (base system)"
}
# missing features -> $REQ (needed) and $REC (recommended)
check_system() {
    REQ="" REC=""
    have git || REQ="$REQ git"
    have cc || have gcc || have clang || REQ="$REQ cc"
    PKGCONFIG=""
    for t in pkg-config pkgconf; do have "$t" && { PKGCONFIG=$t; break; }; done
    if [ -z "$PKGCONFIG" ]; then
        REQ="$REQ pkgconf glib"
        base_openssl >/dev/null || REQ="$REQ openssl"
    else
        $PKGCONFIG --exists 'glib-2.0 >= 2.32' && $PKGCONFIG --exists gmodule-2.0 && $PKGCONFIG --exists gio-2.0 || REQ="$REQ glib"
        $PKGCONFIG --exists openssl || base_openssl >/dev/null || REQ="$REQ openssl"
    fi
    have perl || REQ="$REQ perl"
    if ! have meson || ! have ninja; then
        # as root from the packages; on an account into a private venv
        if [ "$ROOT" = 1 ] || ! have python3; then
            have meson || REQ="$REQ meson"
            have ninja || REQ="$REQ ninja"
        fi
    fi
    perl -MExtUtils::Embed -e1 >/dev/null 2>&1 || REC="$REC perlembed"
    if [ -n "$PKGCONFIG" ]; then
        $PKGCONFIG --exists libutf8proc || [ -e /usr/include/utf8proc.h ] || [ -e /usr/local/include/utf8proc.h ] || REC="$REC utf8proc"
        $PKGCONFIG --exists 'libotr libgcrypt' || REC="$REC otr"
        $PKGCONFIG --exists 'libcurl chafa' || REC="$REC preview"
    fi
}
names() {   # features -> package names (once each)
    for f in "$@"; do packages "$f"; done | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' ' | sed 's/ $//'
}
and_list() {   # words -> "a", "a and b"
    printf '%s' "$1"; [ $# -lt 2 ] || printf ' and %s' "$2"
}
pip_spec() {   # build tool -> the version pip installs
    case "$1" in meson) echo "$MESON_PIP" ;; ninja) echo "$NINJA_PIP" ;; esac
}
what() {   # recommended feature -> what it gives
    case "$1" in
        perlembed) echo "Perl scripts" ;; utf8proc) echo "emoji and wide characters" ;;
        otr) echo "OTR" ;; preview) echo "image preview" ;;
    esac
}
if [ "$CHECK" = 1 ]; then
    STEPS=1
else
    step "Checking the system"
    check_system
    # shellcheck disable=SC2086 # feature lists
    NEED=$(names $REQ) WANT=$(names $REC)
    if [ "$ROOT" = 1 ] && [ -n "$PM" ] && [ -n "$NEED$WANT" ]; then
        step_line "${A}!${N} ${D}packages needed${N}"; printf '\n'; IN_STEP=0
        printf '\n        To build erssi, these packages will be installed (%s):\n' "$PM"
        [ -n "$NEED" ] && printf '          %srequired%s      %s\n' "$B" "$N" "$NEED"
        [ -n "$WANT" ] && printf '          %srecommended%s   %s\n' "$B" "$N" "$WANT"
        printf '\n'
        if [ "$YES" = 1 ]; then
            answer=y
        elif [ -t 0 ] || { [ -r /dev/tty ] && (: < /dev/tty) 2>/dev/null; }; then
            printf '        Install them now? [y/N] '
            if [ -t 0 ]; then read -r answer || answer=""; else read -r answer < /dev/tty || answer=""; fi
        elif [ -z "$NEED" ]; then
            answer=n
        else
            die "packages are missing - run the installer on a terminal to confirm, or add --yes"
        fi
        case "$answer" in
            y|Y|yes|YES) ;;
            *)  if [ -n "$NEED" ]; then
                    printf '\n  Installation cancelled - nothing was installed.\n\n'; exit 1
                fi
                WANT=""
                printf '\n'; warn "continuing without the recommended packages"
                ;;
        esac
        if [ -n "$NEED$WANT" ]; then
        STEP=$((STEP - 1)); step "Installing packages"
        if [ "$PM" = apt ]; then run_logged apt-get update -q || die_log "apt-get update failed"; fi
        # shellcheck disable=SC2046 # the command and its package names
        # shellcheck disable=SC2086 # package lists are split on purpose
        run_logged env DEBIAN_FRONTEND=noninteractive $(pm_install_cmd $NEED $WANT) || die_log "installing the packages failed"
        hash -r 2>/dev/null || true
        check_system
        # shellcheck disable=SC2086
        still=$(names $REQ)
        [ -z "$still" ] || die "still missing after installing the packages: $still"
        done_ "installed"
        STEP=$((STEP - 1)); step "Checking the system"
        else
            STEP=$((STEP - 1)); step "Checking the system"
        fi
    elif [ -n "$NEED" ] || { [ -n "$REQ" ] && [ -z "$PM" ]; }; then
        # shellcheck disable=SC2086
        die "erssi needs packages that only the administrator can install:
      ${NEED:-$REQ}
    $(pm_hint ${NEED:-$REQ})${WANT:+
  recommended as well: $WANT}"
    fi
    if [ -n "$WANT" ] && [ "$ROOT" = 0 ]; then
        for f in $REC; do warn "$(what "$f") not available - recommended: $(names "$f")"; done
    fi
    MESON=meson; NINJA=ninja
    # only the tools the system lacks come from the venv
    MISSING=""
    have meson || MISSING="meson"
    have ninja || MISSING="${MISSING:+$MISSING }ninja"
    if [ -n "$MISSING" ]; then
        PY=""
        for p in python3 python3.14 python3.13 python3.12 python3.11 python3.10; do have "$p" && { PY=$p; break; }; done
        # a safeguard only: without python3, check_system has already asked
        # for meson and ninja as packages
        case "$MISSING" in *" "*) are=are ;; *) are=is ;; esac
        # shellcheck disable=SC2086 # tool list
        [ -n "$PY" ] || die "$(and_list $MISSING) $are missing, and so is Python 3: $(pm_hint "$(names $MISSING)")"
        VENV="$SRC.venv"
        # made here when missing, and checked before it is built or reused
        [ -e "$VENV" ] || [ -L "$VENV" ] || mkdir "$VENV" || die "cannot create $VENV"
        safe_dir "$VENV"
        # of those, what an earlier run did not put into the venv
        PIP=""
        for t in $MISSING; do [ -x "$VENV/bin/$t" ] || PIP="${PIP:+$PIP }$t"; done
        if [ -n "$PIP" ]; then
            # shellcheck disable=SC2046,SC2086 # tool lists
            warn "$(and_list $PIP) not found - installing $(and_list $(for t in $PIP; do pip_spec "$t"; done)) into $VENV (no root needed)"
            run_logged "$PY" -m venv "$VENV" || die_log "$PY -m venv failed ($(pm_hint python3-venv))"
            # wheels only: meson's is pure Python (any system); ninja has
            # none for some systems (FreeBSD), and its source package builds
            # cmake from source first - half an hour without a word. With
            # no wheel the installer stops here and names the package.
            for t in $PIP; do
                run_logged "$VENV/bin/pip" install -q --only-binary=:all: "$(pip_spec "$t")" ||
                    die_log "pip could not install $t (no network, or no ready-made wheel for this system): $(pm_hint "$(names "$t")")"
            done
        fi
        # after the system's directories: a tool of the system is never
        # shadowed by one of the venv
        PATH="$PATH:$VENV/bin"; export PATH
    fi
    done_ "meson $("$MESON" --version) $S_SEP ninja $("$NINJA" --version) $S_SEP glib $($PKGCONFIG --modversion glib-2.0) $S_SEP openssl $(openssl_version)"
fi

# ── 2. the release ───────────────────────────────────────────────────
# only this ref, without history: a few MB instead of the whole repository.
# Fetching by name works for tags, branches and full commit SHAs alike
# (git clone --branch takes no SHA), so a fresh checkout is git init + fetch.
if [ "$CHECK" = 1 ]; then
    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK" "$LOG"' EXIT
    git init -q "$WORK"
else
    WORK=$SRC           # locked above
    # made here when missing, and checked before anything in it is used
    [ -d "$SRC" ] || mkdir "$SRC" || die "cannot create $SRC - use another --src"
    safe_dir "$SRC"
    if [ -d "$SRC/.git" ]; then
        [ -z "$(git -C "$SRC" status --porcelain --untracked-files=no)" ] ||
            die "$SRC has local changes - the installer would overwrite them; use another --src"
    else
        # checkout --force must not land on somebody's files
        [ -z "$(ls -A "$SRC" 2>/dev/null)" ] ||
            die "$SRC exists and is not a git checkout - remove it or use another --src"
        git -c init.defaultBranch=main init -q "$SRC" || die "cannot create a git repository in $SRC"
        NEW_SRC=1
    fi
fi
if [ "$UPDATE" = 1 ] && [ -n "${FOUND_UPDATE:-}" ]; then
    # found and checked by the binary path already: the same signed tag
    # object, fetched into the source directory
    step "Finding the release"
    fetch_ref "$WORK" "$REF" || die "cannot download $REF from $REPO - nothing was changed; try again later"
    [ "$(git -C "$WORK" rev-parse --verify -q FETCH_HEAD)" = "$TAG_OBJ" ] ||
        die "$REF changed while it was downloaded - nothing was changed; try again (report it if it happens again)"
    done_ "$REF $S_SEP signed by the erssi Shellter release key"
elif [ "$UPDATE" = 1 ]; then
    find_update "$WORK"
else
    step "Choosing the release"
    if [ -z "$REF" ]; then
        # the newest release this installer knows (and can verify)
        REF=$(printf '%s\n' "$RELEASES" | awk 'NF >= 2 {r = $1} END {print r}')
        [ -n "$REF" ] || die "no release recorded in this installer - use --ref"
    fi
    done_ "$REF"
fi

# ── 3. download and verify ───────────────────────────────────────────
step "Downloading"
SHA_HINT=""
case "$REF" in
    *[!0-9a-f]*) ;;
    *) [ ${#REF} -eq 40 ] || SHA_HINT=" (a commit must be given as the full 40-character SHA)" ;;
esac
if [ "$UPDATE" = 0 ]; then
    if ! fetch_ref "$SRC" "$REF"; then
        [ -n "${NEW_SRC:-}" ] && rm -rf "$SRC/.git"
        die "cannot download $REF from $REPO$SHA_HINT"
    fi
fi
PINNED=$(printf '%s\n' "$RELEASES" | awk -v r="$REF" '$1 == r {print $2}')
SIGNED=$(printf '%s\n' "$RELEASES" | awk -v r="$REF" '$1 == r && $3 == "signed" {print "yes"}')
VERIFIED=""
if [ "$UPDATE" = 1 ]; then
    VERIFIED="signed"           # checked while finding it
else
    TAG_OBJ=$(git -C "$SRC" rev-parse --verify -q FETCH_HEAD) || die "cannot download $REF from $REPO"
    if [ -n "$SIGNED" ]; then
        if can_verify; then
            signed_tag "$SRC" "$REF" ||
                die "$REF does not carry a valid signature by the erssi Shellter release key - refusing to build (report it)"
            VERIFIED="signed"
        else
            warn "signatures cannot be checked here (git 2.34+ and ssh-keygen) - the recorded commit is still checked"
        fi
    fi
fi
# exactly the object that was checked, not whatever FETCH_HEAD is by now
git -C "$SRC" -c advice.detachedHead=false checkout -q --force "$TAG_OBJ^{commit}" >> "$LOG" 2>&1 || die "cannot check out $REF"
HEAD_SHA=$(git -C "$SRC" rev-parse HEAD)
# release tags never move: a recorded release elsewhere is refused, on an
# update too
if [ -n "$PINNED" ]; then
    [ "$HEAD_SHA" = "$PINNED" ] || die "$REF points to $HEAD_SHA, but this installer expects $PINNED - refusing to build (download the installer again, or report it)"
    VERIFIED="${VERIFIED:+$VERIFIED $S_SEP }commit recorded"
elif [ "$UPDATE" = 0 ]; then
    case "$REF" in
        shellter-v*) warn "$REF is not recorded in this installer - not verified; download the installer again to verify it" ;;
        *) warn "$REF is not a release - built as is, not verified" ;;
    esac
fi
done_ "$(echo "$HEAD_SHA" | cut -c1-8)${VERIFIED:+ $S_SEP $VERIFIED}"

# ── 4. build ─────────────────────────────────────────────────────────
step "Building"
if [ -f "$SRC/Build/build.ninja" ]; then
    run_logged "$MESON" setup --reconfigure "$SRC/Build" "$SRC" -Dprefix="$PREFIX" || die_log "configuring the build failed"
else
    run_logged "$MESON" setup "$SRC/Build" "$SRC" -Dprefix="$PREFIX" -Dwith-proxy=yes || die_log "configuring the build failed"
fi
run_logged "$NINJA" -C "$SRC/Build" || die_log "the build failed"
done_ "$(elapsed $(($(now) - STEP_T0)))"

# ── 5. test ──────────────────────────────────────────────────────────
step "Testing"
if [ "$RUN_TESTS" = 1 ]; then
    run_logged "$MESON" test -C "$SRC/Build" || die_log "tests failed (all results: $SRC/Build/meson-logs/testlog.txt)"
    n=$(sed -n 's/^Ok: *\([0-9]*\).*/\1/p' "$LOG" | tail -n 1)
    done_ "${n:+$n tests $S_SEP }$(elapsed $(($(now) - STEP_T0)))"
else
    skip_ "skipped"
fi

# ── 6. install ───────────────────────────────────────────────────────
step "Installing"
# a binary install before: its bin/erssi is a link into current, which the
# build's erssi must replace, not write through; put back if that fails
OLD_LINK=$(readlink "$PREFIX/bin/erssi" 2>/dev/null) || OLD_LINK=""
[ -z "$OLD_LINK" ] || rm -f "$PREFIX/bin/erssi"
if ! run_logged "$NINJA" -C "$SRC/Build" install; then
    [ -z "$OLD_LINK" ] || [ -e "$PREFIX/bin/erssi" ] || ln -s "$OLD_LINK" "$PREFIX/bin/erssi"
    die_log "installing failed"
fi
# erssi before 2026.10.0 shipped it
rm -f "$PREFIX/share/irssi/themes/nexus.theme"
mkdir -p "$PREFIX/share/irssi"
link_bin
# erssi --update runs this installer (the copy of the installed release)
cp "$SRC/shellter-install.sh" "$PREFIX/share/irssi/shellter-install.sh" 2>/dev/null || true
VERSION=$("$PREFIX/bin/erssi" --version | awk '{print $2}')
PLATFORM=""
write_conf source
if [ "$CLEAN" = 1 ]; then
    rm -rf "$SRC/Build"
    done_ "$(tilde "$PREFIX") $S_SEP build directory removed"
else
    done_ "$(tilde "$PREFIX")"
fi

summary
}

[ -n "${SHELLTER_INSTALL_NO_MAIN:-}" ] || main "$@"
