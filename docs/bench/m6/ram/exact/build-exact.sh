#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: build-exact.sh SRC_DIR OUT_BINARY  (old sources: git archive <sha> src | tar x)
S=$1; PC=$(pkg-config --cflags --libs pangocairo fontconfig glib-2.0)
g++ -std=c++17 -O2 -I"$S" "$(dirname "$0")/cpu_exact.cpp" "$S"/cpu_effects.cpp "$S"/effects.cpp "$S"/thread_pool.cpp \
    "$S"/synthetic_frame.cpp "$S"/grid_painter.cpp "$S"/font_resolve.cpp $PC -lpthread -o "$2"
