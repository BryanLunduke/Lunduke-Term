#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch the pinned libghostty-vt and Zig tarballs, check SHA256, and install a
# static libghostty-vt into PREFIX. Re-running is a no-op when PREFIX already
# holds this pin. Tarballs are cached in third_party/cache (or $LTERM_TARBALL_CACHE)
# so a later build works with no network.
set -euo pipefail

ROOT=${1:?source root}
PREFIX=${2:?install prefix}
PINS=$ROOT/third_party/ghostty-vt.pins
CACHE=${LTERM_TARBALL_CACHE:-$ROOT/third_party/cache}
# shellcheck disable=SC1090
source "$PINS"

if [[ -f $PREFIX/lib/libghostty-vt.a && -f $PREFIX/PINNED && $(cat "$PREFIX/PINNED") == "$LIBGHOSTTY_VERSION+$ZIG_VERSION" ]]; then
    exit 0
fi

arch=$(uname -m)
case $arch in
    x86_64) ZIG_URL=$ZIG_X86_64_URL; ZIG_SHA=$ZIG_X86_64_SHA256; ZIG_DIR=zig-x86_64-linux-$ZIG_VERSION ;;
    aarch64|arm64) ZIG_URL=$ZIG_AARCH64_URL; ZIG_SHA=$ZIG_AARCH64_SHA256; ZIG_DIR=zig-aarch64-linux-$ZIG_VERSION ;;
    *) echo "build-libghostty-vt: no Zig 0.17.1 pin for $arch" >&2; exit 1 ;;
esac

mkdir -p "$CACHE"
fetch() {
    local url=$1 dest=$2 sha=$3
    if [[ -f $dest ]]; then
        echo "$sha  $dest" | sha256sum -c --status && return 0
        echo "build-libghostty-vt: $dest failed SHA256, downloading again" >&2
        rm -f "$dest"
    fi
    echo "build-libghostty-vt: fetching $(basename "$dest")" >&2
    curl -fL --retry 3 --retry-delay 2 -o "$dest.partial" "$url"
    mv "$dest.partial" "$dest"
    echo "$sha  $dest" | sha256sum -c --status
}

fetch "$LIBGHOSTTY_URL" "$CACHE/libghostty-vt-$LIBGHOSTTY_VERSION.tar.gz" "$LIBGHOSTTY_SHA256"
fetch "$LIBGHOSTTY_DEPS_URL" "$CACHE/libghostty-vt-$LIBGHOSTTY_VERSION-deps.tar.gz" "$LIBGHOSTTY_DEPS_SHA256"
fetch "$ZIG_URL" "$CACHE/$(basename "$ZIG_URL")" "$ZIG_SHA"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tar -xzf "$CACHE/libghostty-vt-$LIBGHOSTTY_VERSION.tar.gz" -C "$work"
tar -xzf "$CACHE/libghostty-vt-$LIBGHOSTTY_VERSION-deps.tar.gz" -C "$work"
tar -xJf "$CACHE/$(basename "$ZIG_URL")" -C "$work"
zig=$work/$ZIG_DIR/zig
src=$work/libghostty-vt-$LIBGHOSTTY_VERSION
deps=$work/libghostty-vt-$LIBGHOSTTY_VERSION-deps
# Zig 0.17 crashes if the global cache already holds a package archive.
cache_dir=$work/zig-cache
mkdir -p "$cache_dir" "$PREFIX"
rm -rf "$PREFIX/lib" "$PREFIX/include" "$PREFIX/lib/pkgconfig"
(
    cd "$src"
    ZIG_GLOBAL_CACHE_DIR=$cache_dir \
        "$zig" build \
        --prefix "$PREFIX" \
        --system "$deps" \
        -Doptimize=ReleaseFast \
        -Dcpu=baseline
)
printf '%s\n' "$LIBGHOSTTY_VERSION+$ZIG_VERSION" > "$PREFIX/PINNED"
echo "build-libghostty-vt: installed $LIBGHOSTTY_VERSION with Zig $ZIG_VERSION into $PREFIX" >&2
