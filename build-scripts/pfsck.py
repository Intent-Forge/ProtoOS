#!/usr/bin/env python3
"""pfsck: an independent checker for PFS disk images (ProtoOS v1.5),
written from docs/SPEC_V1_5.md, not from src/pfs.c.

It decides what the kernel must decide at mount (which directory copy is
used, or the refusal code), and also checks what the kernel only checks on
read: the CRC of every file's data. The acceptance harness runs it on the
disk image after every boot.

Usage: pfsck.py <image> [--extract DIR]    prints one JSON object;
exit status 0 if the image mounts and every file's data is intact.
"""
import hashlib
import json
import pathlib
import struct
import sys
import zlib

BLOCK, FILES, NAME_MAX, FILE_MAX, DATA_START = 4096, 63, 15, 128 * 1024, 3
MIN_BLOCKS, MAX_BLOCKS = 256, 16384
NAME_CHARS = set(b'abcdefghijklmnopqrstuvwxyz0123456789_.')
# refusal codes (docs/SPEC_V1_5.md, "Mount")
SB_IO, SB_MAGIC, SB_VERSION, SB_CRC, SB_RANGE, SB_DEVICE, SB_RESERVED = 1, 2, 3, 4, 5, 6, 7
DIR_IO, DIR_MAGIC, DIR_CRC, DIR_RESERVED, NAME, DUP, RANGE, SIZE, OVERLAP, EMPTY = (
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19)


def crc_with_hole(block, at):
    """CRC-32 over the block with the 4 bytes at `at` read as zero."""
    return zlib.crc32(block[:at] + b'\0\0\0\0' + block[at + 4:]) & 0xffffffff


def check_copy(block, nblocks):
    """(code, seq, files) for one directory copy; code 0 if valid."""
    if len(block) < BLOCK:
        return DIR_IO, None, []
    if block[:8] != b'PFSDIR01':
        return DIR_MAGIC, None, []
    seq, crc = struct.unpack_from('<QI', block, 8)
    if crc != crc_with_hole(block, 16):
        return DIR_CRC, None, []
    if any(block[20:64]):
        return DIR_RESERVED, None, []
    files = []
    for slot in range(FILES):
        raw = block[64 + 64 * slot:128 + 64 * slot]
        if raw[0] == 0:
            if any(raw):
                return EMPTY, None, []
            continue
        name = raw[:16].split(b'\0', 1)[0]
        if (len(name) > NAME_MAX or not set(name) <= NAME_CHARS
                or any(raw[len(name):16])):
            return NAME, None, []
        if any(raw[32:64]):
            return DIR_RESERVED, None, []
        size, first, count, fcrc = struct.unpack_from('<IIII', raw, 16)
        if size > FILE_MAX or count != (size + BLOCK - 1) // BLOCK:
            return SIZE, None, []
        if count == 0:
            if first != 0:
                return RANGE, None, []
        elif first < DATA_START or first + count > nblocks:
            return RANGE, None, []
        files.append({'slot': slot, 'name': name.decode(), 'size': size, 'first': first,
                      'count': count, 'crc': fcrc})
    # Pairs in slot order; for each pair the names first, then the extents.
    for i, f in enumerate(files):
        for g in files[i + 1:]:
            if f['name'] == g['name']:
                return DUP, None, []
            if (f['count'] and g['count'] and f['first'] < g['first'] + g['count']
                    and g['first'] < f['first'] + f['count']):
                return OVERLAP, None, []
    return 0, seq, files


def check(img, device_blocks=None):
    """Everything pfsck knows about an image (bytes) as a dict."""
    dev = device_blocks if device_blocks is not None else len(img) // BLOCK
    res = {'mounts': False, 'reject': None, 'slot': None, 'seq': None, 'files': [],
           'copies': {}, 'data_ok': None}
    sb = img[:BLOCK]
    if len(sb) < BLOCK:
        res['reject'] = SB_IO
        return res
    magic, version, nblocks, crc = struct.unpack_from('<4sIII', sb, 0)
    for bad, code in ((magic != b'PFS1', SB_MAGIC), (version != 1, SB_VERSION),
                      (crc != crc_with_hole(sb, 12), SB_CRC), (any(sb[16:]), SB_RESERVED),
                      (not MIN_BLOCKS <= nblocks <= MAX_BLOCKS, SB_RANGE),
                      (nblocks > min(dev, MAX_BLOCKS), SB_DEVICE)):
        if bad:
            res['reject'] = code
            return res
    res['blocks'] = nblocks
    copies = {s: check_copy(img[s * BLOCK:(s + 1) * BLOCK], nblocks) for s in (1, 2)}
    res['copies'] = {('A' if s == 1 else 'B'): {'code': c[0], 'seq': c[1]} for s, c in copies.items()}
    (ca, sa, fa), (cb, sb_, fb) = copies[1], copies[2]
    if ca and cb:
        res['reject'] = ca
        return res
    use_b = bool(ca) or (not cb and sb_ > sa)
    res.update(mounts=True, slot=2 if use_b else 1, seq=sb_ if use_b else sa)
    ok = True
    for f in (fb if use_b else fa):
        data = img[f['first'] * BLOCK:f['first'] * BLOCK + f['size']]
        f['crc_ok'] = (zlib.crc32(data) & 0xffffffff) == f['crc']
        f['sha256'] = hashlib.sha256(data).hexdigest()
        ok &= f['crc_ok']
        res['files'].append(f)
    res['data_ok'] = ok
    return res


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    img = pathlib.Path(args[0]).read_bytes()
    res = check(img)
    if '--extract' in args and res['mounts']:
        out = pathlib.Path(args[args.index('--extract') + 1])
        out.mkdir(parents=True, exist_ok=True)
        for f in res['files']:
            (out / f['name']).write_bytes(img[f['first'] * BLOCK:f['first'] * BLOCK + f['size']])
    print(json.dumps(res))
    sys.exit(0 if res['mounts'] and res['data_ok'] else 1)


if __name__ == '__main__':
    main()
