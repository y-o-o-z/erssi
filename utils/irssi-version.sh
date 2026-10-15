#!/bin/sh

# Header formats, tried in order: shellter-v (this fork, calendar versions),
# erssi-v (older erssi releases), v (irssi upstream)
DATE=`grep '^shellter-v' "$1"/NEWS | head -1`
if [ -z "$DATE" ]; then
    DATE=`grep '^erssi-v' "$1"/NEWS | head -1`
fi
if [ -z "$DATE" ]; then
    DATE=`grep '^v' "$1"/NEWS | head -1`
fi
# fields separated by any whitespace, as utils/check-version.sh reads them
VERSION_DATE=`echo "$DATE" | awk '{ print $2 }' | tr -d -`
case $VERSION_DATE in
  *xx)
    VERSION_DATE=`date +%Y%m%d`
    ;;
esac
HEADER_VERSION=`echo "$DATE" | awk '{ print $1 }'`
case $HEADER_VERSION in
  shellter-v[0-9][0-9][0-9][0-9].*)
    # calendar version YYYY.M.PATCH: month*100+patch without a leading zero,
    # C reads 0903 as octal
    VERSION_TIME=`echo "$HEADER_VERSION" | sed 's/^shellter-v//' | awk -F. '{ printf "%d", $2 * 100 + $3 }'`
    ;;
  *)
    # Remove the 'shellter-v', 'erssi-v' and 'v' prefixes for version parsing
    VERSION_TIME=`echo "$HEADER_VERSION" | sed 's/^shellter-v//; s/^erssi-v//' | tr -d v | tr .- ' '`
    VERSION_TIME=`printf %d%d%02d $VERSION_TIME 2>/dev/null | cut -c 1-4`
    ;;
esac

if echo "${VERSION}" | grep -q -- -head; then
  # -head version, get extra details from git if we can
  DATE=`GIT_DIR=$1/.git git log -1 --pretty=format:%ci HEAD 2>/dev/null`

  VERSION_DATE=`echo $DATE | cut -f 1 -d ' ' | tr -d -`

  if test -z "$VERSION_DATE"; then
    VERSION_DATE=`date +%Y%m%d`
  fi

  git_version=`GIT_DIR=$1/.git git describe --dirty --long --tags --match '[0-9]*.[0-9]*' 2>/dev/null`
  if [ $? = 0 ]; then
    echo "#undef PACKAGE_VERSION"
    echo "#define PACKAGE_VERSION \"${git_version}\""
  fi
fi

echo "#define IRSSI_VERSION_DATE $VERSION_DATE"
echo "#define IRSSI_VERSION_TIME $VERSION_TIME"
