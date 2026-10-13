#!/usr/bin/env python3
"""Build the v1.4 program archives (cpio "newc", byte-reproducible) from the
program ELFs in <build>/user:

  programs.img        guess, primes, clock (released, the shell demo)
  tests.img           test programs of T18-T21 and N12
  badelf.img          N10: malformed program files, each made from
                      t_exit42.elf with ONE defect, plus the valid "ok"
  badarc/<name>.img   N11: malformed archives, one defect each
  images.json         what every image contains and the reason code the
                      kernel must give for each defect (src/elf.h,
                      src/archive.h)

Usage: mkimages.py <build dir>
"""
import json
import pathlib
import struct
import sys

PROGRAMS = ['guess', 'primes', 'clock']
TESTS = ['t_elf', 't_sbrk', 't_exit42', 't_spin', 't_sleepy', 't_reader', 't_fgbusy',
         'b_text', 'b_rodata', 'b_kernel', 'b_exec', 'b_stack', 'b_heap', 'b_fpu', 'b_csr']


def newc(files, mode=0o100555, trailer=True):
    """files: list of (name, bytes[, mode]). Fixed ino/uid/gid/mtime: the
    output depends only on names and contents."""
    out = bytearray()

    def entry(ino, name, data, m):
        nb = name.encode() + b'\0'
        hdr = '070701' + ''.join(f'{v:08X}' for v in (
            ino, m, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nb), 0))
        out.extend(hdr.encode() + nb)
        out.extend(b'\0' * (-len(out) % 4))
        out.extend(data)
        out.extend(b'\0' * (-len(out) % 4))

    for i, f in enumerate(files, start=1):
        entry(i, f[0], f[1], f[2] if len(f) > 2 else mode)
    if trailer:
        entry(0, 'TRAILER!!!', b'', 0)
    return bytes(out)


# ---------------------------------------------------------------- bad ELFs
def phdrs(elf):
    phoff, = struct.unpack_from('<Q', elf, 32)
    phnum, = struct.unpack_from('<H', elf, 56)
    return [phoff + 56 * i for i in range(phnum)]


def put(buf, fmt, off, val):
    struct.pack_into(fmt, buf, off, val)


def load_index(elf, flags):
    """Offset of the first PT_LOAD phdr with exactly these p_flags."""
    for off in phdrs(elf):
        t, f = struct.unpack_from('<II', elf, off)
        if t == 1 and f == flags:
            return off
    raise SystemExit(f'no PT_LOAD with flags {flags}')


def bad_elfs(ok):
    """name -> (bytes, reason code the kernel must report)."""
    text, ro, data = load_index(ok, 5), load_index(ok, 4), load_index(ok, 6)
    ro_vaddr, = struct.unpack_from('<Q', ok, ro + 16)
    text_vaddr, = struct.unpack_from('<Q', ok, text + 16)
    out = {}

    def mut(name, reason, *edits, cut=None):
        b = bytearray(ok)
        for fmt, off, val in edits:
            put(b, fmt, off, val)
        out[name] = (bytes(b[:cut] if cut is not None else b), reason)

    E = {'ident': -1, 'type': -2, 'flags': -3, 'phdrs': -4, 'segtype': -5, 'perm': -6,
         'range': -7, 'addr': -8, 'loads': -9, 'entry': -10, 'size': -11}
    mut('e_magic', E['ident'], ('<B', 1, ord('X')))
    mut('e_class32', E['ident'], ('<B', 4, 1))
    mut('e_bigend', E['ident'], ('<B', 5, 2))
    mut('e_osabi', E['ident'], ('<B', 7, 3))
    mut('e_dyn', E['type'], ('<H', 16, 3))
    mut('e_machine', E['type'], ('<H', 18, 62))
    mut('e_float', E['flags'], ('<I', 48, 0x4))
    mut('e_rvc', E['flags'], ('<I', 48, 0x1))
    mut('e_phentsize', E['phdrs'], ('<H', 54, 32))
    mut('e_phoff', E['phdrs'], ('<Q', 32, len(ok) - 8))
    mut('e_nophdr', E['phdrs'], ('<H', 56, 0))
    mut('e_trunc', E['phdrs'], cut=100)
    mut('e_interp', E['segtype'], ('<I', ro, 3))
    mut('e_dynamic', E['segtype'], ('<I', ro, 2))
    mut('e_tls', E['segtype'], ('<I', data, 7))
    mut('e_wx', E['perm'], ('<I', text + 4, 7))
    mut('e_noread', E['perm'], ('<I', text + 4, 1))
    mut('e_badflag', E['perm'], ('<I', ro + 4, 0x104))
    mut('e_memsz', E['range'], ('<Q', data + 40, 0), ('<Q', data + 32, 16))
    mut('e_offset', E['range'], ('<Q', text + 8, len(ok)))
    mut('e_low', E['addr'], ('<Q', text + 16, 0x10000))
    mut('e_kernel', E['addr'], ('<Q', data + 16, 0x80200000))
    mut('e_unalign', E['addr'], ('<Q', ro + 16, ro_vaddr + 0x10))
    mut('e_overlap', E['addr'], ('<Q', ro + 16, text_vaddr))
    mut('e_wrap', E['addr'], ('<Q', data + 40, 0xfffffffffffff000))
    mut('e_noload', E['loads'], *[('<I', o, 4) for o in (text, ro, data)])
    mut('e_entry', E['entry'], ('<Q', 24, ro_vaddr))
    mut('e_entry_odd', E['entry'], ('<Q', 24, text_vaddr + 2))
    mut('e_big', E['size'], ('<Q', data + 40, 0x80000))
    out['e_empty'] = (b'', E['ident'])
    return out


# ------------------------------------------------------------ bad archives
def bad_archives(ok):
    """name -> (archive bytes, reason code)."""
    good = newc([('ok', ok)])
    out = {}
    b = bytearray(good)
    b[5:6] = b'2'
    out['a_magic'] = (bytes(b), -2)
    b = bytearray(good)
    b[6 + 8 * 6 + 3:6 + 8 * 6 + 4] = b'G'          # filesize field
    out['a_hex'] = (bytes(b), -3)
    out['a_trunc'] = (good[:60], -3)
    out['a_name'] = (newc([('Ok', ok)]), -4)
    out['a_longname'] = (newc([('a' * 16, ok)]), -4)
    out['a_dir'] = (newc([('ok', b'', 0o040755)]), -5)
    b = bytearray(good)
    b[6 + 8 * 12:6 + 8 * 13] = b'00000001'         # check field
    out['a_check'] = (bytes(b), -5)
    b = bytearray(good)
    b[6 + 8 * 6:6 + 8 * 7] = f'{len(good) + 4096:08X}'.encode()
    out['a_data'] = (bytes(b), -6)
    out['a_many'] = (newc([(f'f{i}', b'x') for i in range(33)]), -7)
    out['a_dup'] = (newc([('ok', ok), ('ok', ok)]), -8)
    out['a_notrailer'] = (newc([('ok', ok)], trailer=False), -9)
    out['a_tail'] = (good + b'\0\0\0\0junk', -10)
    return out


def main():
    build = pathlib.Path(sys.argv[1])
    elf = {p.stem: p.read_bytes() for p in sorted((build / 'user').glob('*.elf'))}
    manifest = {'programs.img': PROGRAMS, 'tests.img': TESTS}
    (build / 'programs.img').write_bytes(newc([(n, elf[n]) for n in PROGRAMS]))
    (build / 'tests.img').write_bytes(newc([(n, elf[n]) for n in TESTS]))
    bad = bad_elfs(elf['t_exit42'])
    (build / 'badelf.img').write_bytes(newc([(n, b) for n, (b, _) in bad.items()] +
                                            [('ok', elf['t_exit42'])]))
    manifest['badelf.img'] = {n: r for n, (_, r) in bad.items()}
    arcdir = build / 'badarc'
    arcdir.mkdir(exist_ok=True)
    manifest['badarc'] = {}
    for n, (b, r) in bad_archives(elf['t_exit42']).items():
        (arcdir / f'{n}.img').write_bytes(b)
        manifest['badarc'][n] = r
    (build / 'images.json').write_text(json.dumps(manifest, indent=1) + '\n')


if __name__ == '__main__':
    main()
