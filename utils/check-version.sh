#!/bin/sh
# Usage: check-version.sh SOURCE [TAG]
#
# SOURCE is a directory, or a commit-ish of the current repository.
# Checks that meson.build and the first line of NEWS agree on one calendar
# version YYYY.M.PATCH. With TAG, also checks that the release is dated and the
# tag names that version. Prints one "check-version: ..." line per problem to
# stderr and exits 1 if there was any.

set -u

usage() {
  echo "check-version: $1" >&2
  echo "check-version: usage: check-version.sh SOURCE [TAG]" >&2
  exit 2
}

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
  usage "expected 1 or 2 arguments"
fi

source=$1
tag_mode=0
tag=
if [ $# -eq 2 ]; then
  tag_mode=1
  tag=$2
  [ -n "$tag" ] || usage "the TAG argument is empty"
fi

read_file() {
  if [ -d "$source" ]; then
    cat "$source/$1" 2>/dev/null
  else
    git show "$source^{commit}:$1" 2>/dev/null
  fi
}

meson_build=$(read_file meson.build) || {
  echo "check-version: cannot read meson.build from $source" >&2
  exit 2
}
news=$(read_file NEWS) || {
  echo "check-version: cannot read NEWS from $source" >&2
  exit 2
}
news_first=$(printf '%s\n' "$news" | head -n 1)

version=$(printf '%s\n' "$meson_build" |
  sed -n "s/^  version : '\\([^']*\\)',.*/\\1/p" | head -n 1)

status=0
problem() {
  echo "check-version: $1" >&2
  status=1
}

if ! printf '%s\n' "$version" |
  grep -Eq '^20[0-9]{2}\.([1-9]|1[0-2])\.([0-9]|[1-9][0-9])$'; then
  problem "meson.build: $version is not a calendar version YYYY.M.PATCH"
  exit 1
fi

# the header and the date are separated by any whitespace
header=${news_first%%[[:space:]]*}
cr=$(printf '\r')
if [ "$header" != "shellter-v$version" ] ||
  [ "${news_first#*"$cr"}" != "$news_first" ]; then
  problem "NEWS: the first line is not the header of shellter-v$version"
fi

if [ "$tag_mode" = 1 ]; then
  if [ "$tag" != "shellter-v$version" ]; then
    problem "tag $tag does not name meson.build version $version"
  fi

  date=$(printf '%s\n' "$news_first" | awk '{ print $2 }')
  if ! printf '%s\n' "$date" |
    grep -Eq '^[0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])$'; then
    problem "NEWS: the release has no date ($date), expected 'shellter-v$version YYYY-MM-DD'"
  else
    year=${date%%-*}
    month=${date#*-}
    month=${month%%-*}
    month=$((${month#0}))
    if [ "$version" != "$year.$month.${version##*.}" ]; then
      problem "$version is not the month of its release date $date"
    fi
  fi
fi

exit $status
