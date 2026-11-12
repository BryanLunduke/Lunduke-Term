#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the lunduke-terminal binary package.
# VERSION is the upstream version: meson.build, `lunduke-terminal --version`,
# and the AppStream <release>. The Debian package version is ${VERSION}-1
# (debian/changelog).
set -eu

VERSION=1.1.0

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"

meson_ver=$(sed -n "s/^  version : '\\([^']*\\)'.*/\\1/p" meson.build)
if [ "$meson_ver" != "$VERSION" ]; then
    echo "packaging/build-deb.sh: meson.build version is '$meson_ver', want $VERSION" >&2
    exit 1
fi

deb_ver=$(dpkg-parsechangelog -S Version)
if [ "$deb_ver" != "${VERSION}-1" ]; then
    echo "packaging/build-deb.sh: debian/changelog version is '$deb_ver', want ${VERSION}-1" >&2
    exit 1
fi

if ! grep -q "<release version=\"${VERSION}\"" data/org.lunduke.LundukeTerminal.metainfo.xml; then
    echo "packaging/build-deb.sh: metainfo has no <release version=\"${VERSION}\">" >&2
    exit 1
fi

# Binary-only. debian/source/format is 3.0 (native), and a native source build
# rejects a Debian revision. The .deb version is still ${VERSION}-1.
dpkg-buildpackage -us -uc -b

deb=$(echo ../lunduke-terminal_${deb_ver}_*.deb)
echo "built: $deb"
sha256sum "$deb"
