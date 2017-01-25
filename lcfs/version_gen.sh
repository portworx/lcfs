#!/bin/bash

[ "$1" == "-p" ] && ECHOVERSIONONLY="yes"

BUILD_VERSION="$(git rev-parse HEAD)"
SHORT_BUILD_VERSION="$(git rev-parse --short HEAD)"
if [ -z "${VERSION}" ]; then
    VERSION="$(git symbolic-ref -q --short HEAD || git describe --tags --exact-match)"
fi

if [[ "${VERSION}" == *"-v"* || "${VERSION}" == "v"[0-9]* ]]; then
    NEWVERSION=$(echo "${VERSION}" | sed -e 's/^v//' -e 's/.*-v//');
    [ -n "${NEWVERSION}" ] && VERSION=${NEWVERSION}
fi

if [ -n "$(echo ${VERSION} | sed -e 's/[0-9.]*//g' -e 's/-.*$//')" -o -z "${VERSION//./}" ]; then
    VERSION=0.5.0
    [ -n "${DEFVERSION}" ] && VERSION="${DEFVERSION}"
fi

[ "${ECHOVERSIONONLY}" == "yes" ] &&  echo "${VERSION}" && exit 0

VERSION="${VERSION}-"$SHORT_BUILD_VERSION

tmp_file=/tmp/version.h

printf '#ifndef _LC_VER_H\n#define _LC_VER_H\n\nstatic const char *Build = "Build: %s";\nstatic const char *Release = "Release: %s";\n\n#endif' $BUILD_VERSION $VERSION> $tmp_file

if ! cmp $tmp_file version.h > /dev/null 2> /dev/null
then
	mv $tmp_file version.h
else
	rm -f $tmp_file
fi
