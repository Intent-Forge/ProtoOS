#!/bin/sh
# Run one bootargs scenario for a bounded time and print the UART output.
# Usage: run-scenario.sh <bootargs> [seconds] [elf] [program archive]
set -u
cd "$(dirname "$0")/.." || exit 1
ARGS=${1:?bootargs, e.g. protoos.mode=normal}
SECS=${2:-3}
ELF=${3:-build/protoos.elf}
INITRD=${4:-}
FW=${OPENSBI:-/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin}
set --
[ -n "$INITRD" ] && set -- -initrd "$INITRD"
timeout --foreground "$SECS" qemu-system-riscv64 -machine virt -accel tcg -cpu rv64 \
  -smp 1 -m 128M -bios "$FW" -kernel "$ELF" "$@" -display none -monitor none \
  -serial stdio -nic none -append "$ARGS" -no-reboot \
  | sed -n '/^ProtoOS boot foundation/,$p'
exit 0
