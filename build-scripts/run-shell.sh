#!/bin/sh
# Interactive ProtoOS shell (protoos.mode=shell) in this terminal, with the
# v1.4 program archive (guess, primes, clock, files, cat, note, rm) and the
# v1.5 disk when they exist.
# Usage: run-shell.sh [elf] [program archive] [disk image]. Quit QEMU with Ctrl-A, then X.
# The disk defaults to build/disk-session.img, a copy of build/disk.img made
# on first use and kept between sessions (the built disk.img itself, like
# release/disk.img, is never written). "none" runs without a disk.
set -u
cd "$(dirname "$0")/.."
ELF=${1:-build/protoos.elf}
IMG=${2:-build/programs.img}
DISK=${3:-build/disk-session.img}
FW=${OPENSBI:-/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin}
if [ "$DISK" = build/disk-session.img ] && [ ! -f "$DISK" ] && [ -f build/disk.img ]; then
  cp build/disk.img "$DISK"
fi
set --
[ -f "$IMG" ] && set -- -initrd "$IMG"
if [ "$DISK" != none ] && [ -f "$DISK" ]; then
  set -- "$@" -drive "if=none,file=$DISK,format=raw,id=hd0" -device virtio-blk-device,drive=hd0 \
    -global virtio-mmio.force-legacy=false
fi
echo "ProtoOS shell under QEMU. Try: help, ls, run primes, run files, run hello world, run note todo."
echo "Quit: Ctrl-A, then X."
exec qemu-system-riscv64 -machine virt -accel tcg -cpu rv64 -smp 1 -m 128M \
  -bios "$FW" -kernel "$ELF" "$@" -display none -serial mon:stdio -nic none \
  -append protoos.mode=shell -no-reboot
