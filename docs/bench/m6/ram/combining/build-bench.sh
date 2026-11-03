#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: build-bench.sh SRC_DIR BUILD_DIR OUT_BINARY   (BUILD_DIR: a release meson build of SRC_DIR)
g++ -std=c++17 -O2 -I"$1/src" "$(dirname "$0")/sb_combining_bench.cpp" "$2/liblterm-vterm.a" "$2/liblterm-effects.a" \
    $(pkg-config --cflags --libs vterm pangocairo fontconfig) -lutil -o "$3"
