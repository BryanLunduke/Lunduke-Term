#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Lunduke Terminal M1 TCG benchmark VM (TCG only, never KVM). Live ISO read-only, -snapshot.
D=/workspace/lterm-tcg; ISO=/workspace/lcos-iso/lcos-live-100-01.iso
rm -f /tmp/q-lterm.sock
exec /usr/bin/qemu-system-x86_64 -name lterm-tcg -accel tcg,thread=multi -cpu max -smp 4 -m 2048 -machine q35 \
  -vga std -display none -nic none -usb -device usb-tablet -snapshot \
  -drive file=$ISO,media=cdrom,readonly=on,if=ide \
  -drive file=$D/payload-s2.img,format=raw,if=virtio,snapshot=off \
  -kernel $D/vmlinuz -initrd $D/initrd.img \
  -append "boot=live components hostname=LCOS username=lcos user-password=live loglevel=3 console=ttyS0,115200 console=tty0" \
  -qmp unix:/tmp/q-lterm.sock,server,nowait -serial file:$D/serial.log -pidfile $D/qemu.pid -daemonize
