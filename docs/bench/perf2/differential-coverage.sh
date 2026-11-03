#!/bin/bash
set -e
cd /tmp/lvcov; rm -f *.gcda *.gcno *.gcov
R=/workspace/lunduke-term-perf2; L=$R/third_party/libvterm; P=$L/../libvterm-patches
python3 $R/tools/libvterm_patch.py patch $L/src/screen.c screen.c $P/series $P/0001-*.patch $P/0002-*.patch
gcc -std=c99 -O0 --coverage -c -I$L/include -I$L/src screen.c -o screen.o
cp /tmp/lvstock/libstock-rel.a lib.a; ar d lib.a r_screen.o; ar r lib.a screen.o
gcc -std=c99 -O0 --coverage -I$L/include $R/tests/libvterm_diff.c lib.a -o d
for s in $(seq 1 100); do ./d $s 2000 a > /dev/null; ./d $s 2000 b > /dev/null; done
gcov -o . screen.c > /dev/null 2>&1
