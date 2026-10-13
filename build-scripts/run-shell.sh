#!/bin/sh
# Interactive ProtoOS shell (protoos.mode=shell) in this terminal, with the
# v1.4 program archive (guess, primes, clock) when it exists.
# Usage: run-shell.sh [elf] [program archive]. Quit QEMU with Ctrl-A, then X.
set -u
cd "$(dirname "$0")/.."
ELF=${1:-build/protoos.elf}
IMG=${2:-build/programs.img}
FW=${OPENSBI:-/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin}
set --
[ -f "$IMG" ] && set -- -initrd "$IMG"
echo "ProtoOS shell under QEMU. Try: help, ls, run primes, run guess, run clock (Ctrl-C stops it)."
echo "Quit: Ctrl-A, then X."
exec qemu-system-riscv64 -machine virt -accel tcg -cpu rv64 -smp 1 -m 128M \
  -bios "$FW" -kernel "$ELF" "$@" -display none -serial mon:stdio -nic none \
  -append protoos.mode=shell -no-reboot
