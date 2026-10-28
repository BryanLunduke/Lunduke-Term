#!/bin/bash
# Lunduke Terminal M3 item 2 TCG VM, virtio-vga 2D (TCG only, never KVM). Live ISO read-only, -snapshot.
D=/workspace/lterm-tcg; ISO=/workspace/lcos-iso/lcos-live-100-01.iso
rm -f /tmp/q-lterm-m3.sock
exec /usr/bin/qemu-system-x86_64 -name lterm-tcg-m3b -accel tcg,thread=multi -cpu max -smp 4 -m 2048 -machine q35 \
  -vga none -device virtio-vga -display none -nic none -usb -device usb-tablet -snapshot \
  -drive file=$ISO,media=cdrom,readonly=on,if=ide \
  -drive file=$D/payload-m3b.img,format=raw,if=virtio,snapshot=off \
  -kernel $D/vmlinuz -initrd $D/initrd.img \
  -append "boot=live components hostname=LCOS username=lcos user-password=live loglevel=3 console=ttyS0,115200 console=tty0" \
  -qmp unix:/tmp/q-lterm-m3.sock,server,nowait -serial file:$D/serial-m3b.log -pidfile $D/qemu-m3.pid -daemonize
