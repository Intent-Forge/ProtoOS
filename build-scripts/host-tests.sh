#!/bin/sh
# Host-side regression: FDT parser, the Sv39 table builder and (v1.4) the
# program archive parser and ELF checker (plain + ASan/UBSan).
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -c tests/test_sv39_layout.c -o "$T/c1a.o"
cc -std=c11 -Wall -Wextra -Werror -O2 tests/test_sv39_builder.c src/sv39_builder.c -o "$T/c1b"
"$T/c1b"
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/test_sv39_builder.c src/sv39_builder.c -o "$T/c1b-san"
ASAN_OPTIONS=detect_leaks=0 "$T/c1b-san"
python3 tests/test_fdt.py
cc -std=c11 -Wall -Wextra -Werror -O2 tests/test_archive_elf.c src/archive.c src/elf.c -o "$T/ae"
"$T/ae"
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -fno-omit-frame-pointer tests/test_archive_elf.c src/archive.c src/elf.c -o "$T/ae-san"
ASAN_OPTIONS=detect_leaks=0 "$T/ae-san"
rm -rf "$T"
echo HOST_TESTS_PASS
