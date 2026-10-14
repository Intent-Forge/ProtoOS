#!/bin/sh
# Build ProtoOS with the pinned Zig 0.13.0 (same compiler and flags as
# checkpoint 0008's build-portable.sh). Output: build/protoos.elf, the v1.4
# programs (build/user/*.elf) and archives (build/*.img, build/badarc/),
# and SHA-256 sums of the released files.
set -eu
cd "$(dirname "$0")/.."
ZIG=${ZIG:-"$HOME/.protoos-venv/bin/zig"}
[ -x "$ZIG" ] || { echo "ERROR: Zig not found at $ZIG (run build-scripts/setup-zig.sh)" >&2; exit 2; }
"$ZIG" version | grep -qx '0.13.0' || { echo 'ERROR: expected Zig 0.13.0' >&2; exit 2; }
OUT=${OUT:-build}
mkdir -p "$OUT"
: "${ZIG_GLOBAL_CACHE_DIR:=$(mktemp -d /tmp/protoos-zig-cache.XXXXXXXX)}"
export ZIG_GLOBAL_CACHE_DIR
ZIGLIB=$(dirname "$(readlink -f "$ZIG")")
"$ZIG" cc -target riscv64-freestanding \
  -mcpu=generic_rv64+m+zicsr+zifencei -mabi=lp64 -mcmodel=medany \
  -mno-relax -msmall-data-limit=0 -ffreestanding -fno-builtin \
  -fno-stack-protector -fno-pie -Wall -Wextra -Werror -O2 -g \
  -ffile-prefix-map="$ZIGLIB"=/protoos-toolchain -fdebug-compilation-dir=. \
  -nostdlib -static -Wl,--build-id=none,-T,src/linker.ld \
  src/boot.S src/trap.S src/user.S src/kernel.c src/fdt.c src/sv39_builder.c \
  src/vm.c src/proc.c src/input.c src/mem.c src/archive.c src/elf.c \
  src/pfs.c src/virtio_blk.c src/disk.c -o "$OUT/protoos.elf"
readelf -h -l -A -W "$OUT/protoos.elf" > "$OUT/elf-report.txt"
riscv64-unknown-elf-objdump -d "$OUT/protoos.elf" > "$OUT/disassembly.txt"
riscv64-unknown-elf-nm -n "$OUT/protoos.elf" > "$OUT/symbols.txt"

# v1.4 programs: RV64IM, soft-float, no compressed instructions (the
# kernel's loader requires e_flags = 0), static at 0x40000000.
mkdir -p "$OUT/user"
for src in user/programs/*.c user/tests/*.c; do
  name=$(basename "$src" .c)
  "$ZIG" cc -target riscv64-freestanding \
    -mcpu=generic_rv64+m+zicsr -mabi=lp64 -mcmodel=medany \
    -mno-relax -msmall-data-limit=0 -ffreestanding -fno-builtin \
    -fno-stack-protector -fno-pie -Wall -Wextra -Werror -O2 -g0 \
    -ffile-prefix-map="$ZIGLIB"=/protoos-toolchain -Iuser/include \
    -nostdlib -static -Wl,--build-id=none,-T,user/user.ld \
    user/lib/crt0.S user/lib/lib.c "$src" -o "$OUT/user/$name.elf"
done
python3 build-scripts/mkimages.py "$OUT"
(cd "$OUT" && sha256sum protoos.elf programs.img disk.img > SHA256SUMS)
cat "$OUT/SHA256SUMS"
