#!/usr/bin/env python3
"""v1.5 host test: pfsck.py (written from the spec) and the kernel's PFS
mount (src/pfs.c, run on the host by test_pfs) must agree on every fuzzed
superblock/directory image test_pfs dumped: refused with the same code, or
mounted with the same copy, seq and entries. Also checks every image that
build-scripts/mkimages.py made against images.json.

Usage: test_pfsck_agree.py <dump dir> [<build dir>]"""
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'build-scripts'))
import pfsck  # noqa: E402

dump = pathlib.Path(sys.argv[1])
n = mounted = 0
for line in (dump / 'manifest.txt').read_text().splitlines():
    f, dev, r, *rest = line.split()
    res = pfsck.check((dump / f).read_bytes(), device_blocks=int(dev))
    if int(r):
        assert not res['mounts'] and res['reject'] == int(r), (f, r, res)
    else:
        slot, seq, *ents = rest
        assert res['mounts'] and res['slot'] == int(slot) and res['seq'] == int(seq), (f, rest, res)
        mine = [f"{e['name']}:{e['size']}:{e['first']}:{e['count']}:{e['crc']}" for e in res['files']]
        assert mine == ents, (f, ents, mine)
        mounted += 1
    n += 1
assert n == 4000, n
out = {'fuzz_images': n, 'mounted': mounted, 'rejected': n - mounted}
if len(sys.argv) > 2:
    build = pathlib.Path(sys.argv[2])
    man = json.loads((build / 'images.json').read_text())['disks']
    for name, code in man['disks/bad'].items():
        res = pfsck.check((build / 'disks' / 'bad' / f'{name}.img').read_bytes())
        assert not res['mounts'] and res['reject'] == code, (name, code, res)
    for img in ('disk.img', 'disks/blank.img', 'disks/ro.img'):
        res = pfsck.check((build / img).read_bytes())
        assert res['mounts'] and res['data_ok'] and res['slot'] == 1, (img, res)
    res = pfsck.check((build / 'disks/fallback.img').read_bytes())
    assert res['mounts'] and res['slot'] == 2 and len(res['files']) == 1, res
    res = pfsck.check((build / 'disks/crcbad.img').read_bytes())
    bad = sorted(f['name'] for f in res['files'] if not f['crc_ok'])
    assert res['mounts'] and bad == ['dmg', 'dmgprog'], res
    out['build_images'] = len(man['disks/bad']) + 5
print(json.dumps(out))
print('PFSCK_AGREE_PASS')
