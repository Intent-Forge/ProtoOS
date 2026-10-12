#!/bin/sh
# Interactive ProtoOS shell (protoos.mode=shell) in this terminal.
# Quit QEMU with Ctrl-A, then X.
set -u
cd "$(dirname "$0")/.."
ELF=${1:-build/protoos.elf}
FW=${OPENSBI:-/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin}
echo "ProtoOS shell under QEMU. Commands: help, ps, uptime, echo <text>, attack."
echo "Quit: Ctrl-A, then X."
exec qemu-system-riscv64 -machine virt -accel tcg -cpu rv64 -smp 1 -m 128M \
  -bios "$FW" -kernel "$ELF" -display none -serial mon:stdio -nic none \
  -append protoos.mode=shell -no-reboot
