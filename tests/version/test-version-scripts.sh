#!/bin/sh
# Tests for utils/irssi-version.sh and utils/check-version.sh.
# Usage: test-version-scripts.sh SOURCE_ROOT

# a clean environment: nothing from git (GIT_DIR when the real pre-push
# runs this suite would make the git init and commit below work on the
# pushing repository) or the caller's HOME
if [ -z "${VERSION_TEST_CLEAN:-}" ]; then
    exec env -i VERSION_TEST_CLEAN=1 PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" sh "$0" "$@"
fi

root=$1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export HOME="$tmp" GIT_CONFIG_NOSYSTEM=1
failed=0
n=0
today=$(date +%Y%m%d)
who='y-o-o-z <https://github.com/y-o-o-z/erssi>'

pass() { echo "ok $1"; }
fail() { echo "FAIL $1: $2"; failed=1; }

# --- irssi-version.sh -------------------------------------------------------
# iv NAME FIRST_LINE EXPECTED_DATE EXPECTED_TIME [EXTRA_LINE]
iv() {
  n=$((n + 1))
  d="$tmp/iv$n"
  mkdir -p "$d"
  printf '%s\n' "$2" > "$d/NEWS"
  [ -n "${5:-}" ] && printf '\n%s\n' "$5" >> "$d/NEWS"
  out=$(VERSION=2026.10.0 sh "$root/utils/irssi-version.sh" "$d" 2>&1)
  want="#define IRSSI_VERSION_DATE $3
#define IRSSI_VERSION_TIME $4"
  if [ "$out" = "$want" ]; then
    pass "$1"
  else
    fail "$1" "$(printf '%s' "$out" | tr '\n' '|')"
  fi
  case $2 in
    shellter-v202[67].*)
      if printf '%s\n' "$out" | grep -q '^#define IRSSI_VERSION_TIME [1-9][0-9]*$'; then
        pass "$1 (no leading zero)"
      else
        fail "$1 (no leading zero)" "$out"
      fi
      ;;
  esac
}

iv 'shellter 2026.10.0' "shellter-v2026.10.0 2026-10-10  $who" 20261010 1000
iv 'shellter 2026.9.3' "shellter-v2026.9.3 2026-09-30  $who" 20260930 903
iv 'shellter 2027.1.12' "shellter-v2027.1.12 2027-01-05  $who" 20270105 112
iv 'shellter 2026.10.0 unreleased' "shellter-v2026.10.0 2026-10-xx  $who" "$today" 1000
iv 'shellter 1.3.10 legacy' "shellter-v1.3.10 2026-10-08  $who" 20261008 1310
iv 'erssi 1.3.10' "erssi-v1.3.10 2026-10-08  $who" 20261008 1310
iv 'irssi 1.4.5' "v1.4.5 2023-07-13  The Irssi team <staff@irssi.org>" 20230713 1405
iv 'shellter 2026.10.0, two spaces before the date' "shellter-v2026.10.0  2026-10-10  $who" 20261010 1000
iv 'shellter 2026.10.0, a tab before the date' "shellter-v2026.10.0$(printf '\t')2026-10-10  $who" 20261010 1000
iv 'shellter first, older erssi later' "shellter-v2026.10.0 2026-10-10  $who" 20261010 1000 \
  'erssi-v1.3.0 2026-03-24  erssi-org team <https://github.com/erssi-org>'

# --- check-version.sh -------------------------------------------------------
# mkfix DIR VERSION HEADER
mkfix() {
  mkdir -p "$1"
  printf "project('erssi', 'c',\n  version : '%s',\n" "$2" > "$1/meson.build"
  printf '%s  %s\n' "$3" "$who" > "$1/NEWS"
}

# cv NAME VERSION HEADER TAG WANT_STATUS WANT_STDERR
cv() {
  n=$((n + 1))
  d="$tmp/cv$n"
  mkfix "$d" "$2" "$3"
  if [ -n "$4" ]; then
    err=$(sh "$root/utils/check-version.sh" "$d" "$4" 2>&1 >/dev/null)
  else
    err=$(sh "$root/utils/check-version.sh" "$d" 2>&1 >/dev/null)
  fi
  st=$?
  if [ "$st" = "$5" ] && [ "$err" = "$6" ]; then
    pass "$1"
  else
    fail "$1" "status=$st stderr=$err"
  fi
}

cv 'unreleased, no tag' 2026.10.0 'shellter-v2026.10.0 2026-10-xx' '' 0 ''
cv 'unreleased, tag' 2026.10.0 'shellter-v2026.10.0 2026-10-xx' shellter-v2026.10.0 1 \
  "check-version: NEWS: the release has no date (2026-10-xx), expected 'shellter-v2026.10.0 YYYY-MM-DD'"
cv 'released, tag' 2026.10.0 'shellter-v2026.10.0 2026-10-12' shellter-v2026.10.0 0 ''
cv 'wrong tag' 2026.10.0 'shellter-v2026.10.0 2026-10-12' shellter-v2026.10.1 1 \
  'check-version: tag shellter-v2026.10.1 does not name meson.build version 2026.10.0'
cv 'leading zero month' 2026.09.0 'shellter-v2026.09.0 2026-09-01' '' 1 \
  'check-version: meson.build: 2026.09.0 is not a calendar version YYYY.M.PATCH'
cv 'month 13' 2026.13.0 'shellter-v2026.13.0 2026-10-01' '' 1 \
  'check-version: meson.build: 2026.13.0 is not a calendar version YYYY.M.PATCH'
cv 'patch 100' 2026.10.100 'shellter-v2026.10.100 2026-10-01' '' 1 \
  'check-version: meson.build: 2026.10.100 is not a calendar version YYYY.M.PATCH'
cv 'erssi header' 2026.10.0 'erssi-v2026.10.0 2026-10-12' '' 1 \
  'check-version: NEWS: the first line is not the header of shellter-v2026.10.0'
cv 'month mismatch' 2026.10.0 'shellter-v2026.10.0 2026-11-02' shellter-v2026.10.0 1 \
  'check-version: 2026.10.0 is not the month of its release date 2026-11-02'

# a NEWS larger than a pipe buffer, with SIGPIPE ignored as a service
# manager may leave it: the first line is read without an error
n=$((n + 1)); d="$tmp/cv$n"
mkfix "$d" 2026.10.0 'shellter-v2026.10.0 2026-10-12'
awk 'BEGIN { for (i = 0; i < 4000; i++) print "\t  - an older release line, long enough to fill the pipe" }' >> "$d/NEWS"
err=$( (trap '' PIPE; sh "$root/utils/check-version.sh" "$d" shellter-v2026.10.0) 2>&1 >/dev/null)
st=$?
if [ "$st" = 0 ] && [ -z "$err" ]; then
  pass 'NEWS over 64 KB, SIGPIPE ignored'
else
  fail 'NEWS over 64 KB, SIGPIPE ignored' "status=$st stderr=$err"
fi

cv 'impossible release day' 2026.10.0 'shellter-v2026.10.0 2026-10-99' shellter-v2026.10.0 1 \
  "check-version: NEWS: the release has no date (2026-10-99), expected 'shellter-v2026.10.0 YYYY-MM-DD'"
cv 'impossible release month' 2026.10.0 'shellter-v2026.10.0 2026-00-12' shellter-v2026.10.0 1 \
  "check-version: NEWS: the release has no date (2026-00-12), expected 'shellter-v2026.10.0 YYYY-MM-DD'"
cv 'two spaces before the date' 2026.10.0 'shellter-v2026.10.0  2026-10-12' shellter-v2026.10.0 0 ''
cv 'a tab before the date' 2026.10.0 "shellter-v2026.10.0$(printf '\t')2026-10-12" shellter-v2026.10.0 0 ''
cv 'no date, author only' 2026.10.0 'shellter-v2026.10.0' shellter-v2026.10.0 1 \
  "check-version: NEWS: the release has no date (y-o-o-z), expected 'shellter-v2026.10.0 YYYY-MM-DD'"
cv 'a space before the header' 2026.10.0 ' shellter-v2026.10.0 2026-10-12' '' 1 \
  'check-version: NEWS: the first line is not the header of shellter-v2026.10.0'

# Argument handling and unreadable input must fail with exit 2.
# ex NAME WANT_STATUS ARGS...
ex() {
  name=$1; want=$2; shift 2
  err=$(sh "$root/utils/check-version.sh" "$@" 2>&1 >/dev/null)
  st=$?
  case $err in
    'check-version: '*) pfx=1 ;;
    *) pfx=0 ;;
  esac
  if [ "$st" = "$want" ] && [ "$pfx" = 1 ]; then
    pass "$name"
  else
    fail "$name" "status=$st stderr=$err"
  fi
}

n=$((n + 1))
d="$tmp/arg$n"
mkfix "$d" 2026.10.0 'shellter-v2026.10.0 2026-10-xx'
ex 'empty tag' 2 "$d" ''
ex 'too many arguments' 2 "$d" shellter-v2026.10.0 extra
ex 'no arguments' 2
ex 'missing directory' 2 "$tmp/does-not-exist"

# CRLF line endings in NEWS are not accepted.
n=$((n + 1))
d="$tmp/crlf$n"
mkfix "$d" 2026.10.0 'shellter-v2026.10.0 2026-10-12'
sed 's/$/\r/' "$d/NEWS" > "$d/NEWS.crlf" && mv "$d/NEWS.crlf" "$d/NEWS"
err=$(sh "$root/utils/check-version.sh" "$d" shellter-v2026.10.0 2>&1 >/dev/null)
st=$?
if [ "$st" = 1 ] && [ -n "$err" ]; then pass 'CRLF NEWS'; else fail 'CRLF NEWS' "status=$st stderr=$err"; fi

# Commit mode.
if command -v git >/dev/null 2>&1; then
  d="$tmp/commit"
  mkfix "$d" 2026.10.0 'shellter-v2026.10.0 2026-10-12'
  (cd "$d" && git init -q && git add meson.build NEWS &&
    git -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false commit -qam x) >/dev/null 2>&1
  err=$(cd "$d" && sh "$root/utils/check-version.sh" HEAD shellter-v2026.10.0 2>&1)
  st=$?
  if [ "$st" = 0 ] && [ -z "$err" ]; then
    pass 'commit mode'
  else
    fail 'commit mode' "status=$st stderr=$err"
  fi
  err=$(cd "$d" && sh "$root/utils/check-version.sh" no-such-commit 2>&1 >/dev/null)
  st=$?
  case $err in
    'check-version: '*) pfx=1 ;;
    *) pfx=0 ;;
  esac
  if [ "$st" = 2 ] && [ "$pfx" = 1 ]; then
    pass 'nonexistent commit'
  else
    fail 'nonexistent commit' "status=$st stderr=$err"
  fi
fi

exit $failed
