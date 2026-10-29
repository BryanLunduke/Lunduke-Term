M4 item 3 TCG VM run, 2026-10-28 ~20:01-20:11 CDT (guest clock UTC). Build 95fd762 (BUILD, bin.sha256).
qemu 10.0.13, -accel tcg (no KVM), 4 vCPU, 2 GB, std VGA, LCOS live ISO lcos-live-100-01.iso readonly=on + -snapshot.
ISO sha256 identical before and after (iso-sha256-before.txt / iso-sha256-after.txt).
payload-m4.img: ext4 (label LTERM) made with mkfs.ext4 -d from payload-m3's libs/fonts + this build; results read back with debugfs rdump.
Known problems:
- The plain run's tag became "plain-/default0" in the guest's tr, so its .log/trace files were not written (its idle.txt line is there).
- drive-1.log: the first driver kept a stale QMP connection open (makefile fd not closed), so the screendumps for the
  first three idle runs were not taken. Fixed in drive-m4.py (done()) and re-run in "watch" mode (drive-2-watch.log),
  which captured only the last run's screenshot.
- system_powerdown did not finish within 90 s; qemu was stopped with QMP quit after all output was synced.
