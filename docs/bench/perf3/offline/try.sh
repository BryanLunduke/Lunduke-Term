#!/bin/bash
# try.sh TAG : rebuild release, render, compare vs master
set -e
tag=$1; cd /workspace/lunduke-term-perf3; ninja -C build -j2 > /workspace/perf3-work/b-$tag.log 2>&1 || { tail -20 /workspace/perf3-work/b-$tag.log; exit 1; }
cd build; rm -rf /workspace/perf3-work/$tag-libs; for l in liblterm-gl liblterm-effects; do mkdir -p /workspace/perf3-work/$tag-libs/$l; cp $(ar t $l.a) /workspace/perf3-work/$tag-libs/$l/; done
cd /workspace/perf3-work/render && ./build.sh /workspace/lunduke-term-perf3 /workspace/perf3-work/$tag-libs render-$tag && rm -rf out-$tag && mkdir -p out-$tag && LIBGL_ALWAYS_SOFTWARE=1 ./render-$tag out-$tag >/dev/null && python3 psnr.py out-master out-$tag > psnr-$tag.md; sort -t'|' -k4 psnr-$tag.md | sed -n 3,6p; tail -4 psnr-$tag.md
