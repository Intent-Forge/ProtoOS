#!/usr/bin/env python3
"""Build PFS disk images (ProtoOS v1.5, docs/SPEC_V1_5.md), byte-reproducible:
the output depends only on the block count, the file names and contents and
their order.

  block 0   superblock: "PFS1", version 1, block count, CRC-32
  block 1   directory copy A, seq 1   } "PFSDIR01", seq, CRC-32, 63
  block 2   directory copy B, seq 0   } entries of 64 bytes
  block 3.. file data, one contiguous extent per file, in order

Usage: mkpfs.py <out.img> <blocks> [name=path ...]
As a module: image(blocks, [(name, bytes), ...]) and the low-level helpers
mkimages.py uses for the malformed images of N13.
"""
import struct
import sys
import zlib

BLOCK = 4096
FILES = 63
FILE_MAX = 128 * 1024
DATA_START = 3


def crc_block(b, at):
    """CRC-32 of a block with its 4-byte CRC field at `at` taken as zero."""
    return zlib.crc32(bytes(b[:at]) + bytes(4) + bytes(b[at + 4:])) & 0xffffffff


def superblock(blocks, magic=b'PFS1', version=1, reserved=b''):
    b = bytearray(BLOCK)
    b[0:4] = magic
    struct.pack_into('<II', b, 4, version, blocks)
    b[16:16 + len(reserved)] = reserved
    struct.pack_into('<I', b, 12, crc_block(b, 12))
    return b


def entry(name, size, first, count, crc):
    e = bytearray(64)
    nb = name.encode() if isinstance(name, str) else name
    e[0:len(nb)] = nb
    struct.pack_into('<IIII', e, 16, size, first, count, crc)
    return e


def directory(entries, seq, magic=b'PFSDIR01'):
    """entries: list of 63 or fewer 64-byte entries (slot order)."""
    b = bytearray(BLOCK)
    b[0:8] = magic
    struct.pack_into('<Q', b, 8, seq)
    for i, e in enumerate(entries):
        b[64 + 64 * i:128 + 64 * i] = e
    struct.pack_into('<I', b, 16, crc_block(b, 16))
    return b


def layout(files):
    """(entries, data blocks) for files placed contiguously from block 3."""
    entries, data, nxt = [], [], DATA_START
    for name, content in files:
        assert len(content) <= FILE_MAX and 1 <= len(name) <= 15
        count = -(-len(content) // BLOCK)
        entries.append(entry(name, len(content), nxt if count else 0, count,
                             zlib.crc32(content) & 0xffffffff))
        padded = content + bytes(count * BLOCK - len(content))
        data += [padded[i:i + BLOCK] for i in range(0, len(padded), BLOCK)]
        nxt += count
    return entries, data


def assemble(blocks, sb, dir_a, dir_b, data):
    img = bytearray(blocks * BLOCK)
    img[0:BLOCK] = sb
    img[BLOCK:2 * BLOCK] = dir_a
    img[2 * BLOCK:3 * BLOCK] = dir_b
    for i, d in enumerate(data):
        img[(DATA_START + i) * BLOCK:(DATA_START + i + 1) * BLOCK] = d
    return bytes(img)


def image(blocks, files=()):
    """A valid image: both directory copies list the same files; A has
    seq 1 and B seq 0, so a fresh disk mounts copy A."""
    entries, data = layout(list(files))
    assert DATA_START + len(data) <= blocks
    return assemble(blocks, superblock(blocks), directory(entries, 1), directory(entries, 0), data)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    files = []
    for arg in sys.argv[3:]:
        name, path = arg.split('=', 1)
        with open(path, 'rb') as f:
            files.append((name, f.read()))
    with open(sys.argv[1], 'wb') as f:
        f.write(image(int(sys.argv[2]), files))


if __name__ == '__main__':
    main()
