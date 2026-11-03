#!/bin/bash
# build.sh SRC_DIR OBJ_DIR(with liblterm-gl/ liblterm-effects/ objects) OUT_BIN
set -e
g++ -std=c++17 -O2 -I$1/src render.cpp -o $3 $2/liblterm-gl/*.o $2/liblterm-effects/*.o $(pkg-config --cflags --libs pangocairo epoxy egl fontconfig glib-2.0 gio-2.0) -lpthread
