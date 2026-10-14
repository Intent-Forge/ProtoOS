#!/usr/bin/env python3
"""ProtoOS acceptance runner, 3 clean boots per check: v1.0 T1-T7 and N1-N5,
v1.1 T8, T9, N6, v1.2 T10-T12, N7, N8, v1.3 T13-T17, N9, v1.4 T18-T22,
N10-N12 (v1.4 scenarios boot with a program archive, QEMU -initrd).

Every scenario boot is a fresh qemu-system-riscv64 process (fresh RAM, same
ELF, SHA-256 checked before and after). Independent evidence per boot:
  * QEMU `-d int` trace: every architectural trap/interrupt with cause/epc/tval
  * GDB (validation/gdb_probe.py): PC, privilege, CSRs, registers, saved
    contexts, physical memory and an independent Sv39 page-table walk
  * QMP (T7): physical-memory sampling of a free-running Normal boot
UART output is recorded for correlation only and never decides a PASS alone.

Statuses: PASS / FAIL (violation observed) / INCONCLUSIVE (probe incomplete).
Usage: python3 validation/run_acceptance.py [--elf build/protoos.elf]
       [--images build] [--tests T1,T3] [--reps 3] [--t7-seconds 20]
"""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
FW = os.environ.get('OPENSBI', '/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin')
QEMU = 'qemu-system-riscv64'
GDB = 'gdb-multiarch'
NM = 'riscv64-unknown-elf-nm'

A_BASE, B_BASE = 0x400000, 0x800000
A_RANGE, B_RANGE = (A_BASE, A_BASE + 0x4000), (B_BASE, B_BASE + 0x4000)
PROG_VA, PROG_PAGES = 0x4000000, 3          # v1.3 shared program pages
PROG_RANGE = (PROG_VA, PROG_VA + PROG_PAGES * 0x1000)
ELF_RANGE = (0x40000000, 0x40200000)        # v1.4 window of archive programs
POOL_START = 0x80400000
A_PATTERN, B_PATTERN = 0xa1a1a1a100000000, 0xb2b2b2b200000000
STATE = {1: 'READY', 2: 'RUNNING', 3: 'EXITED', 4: 'FAULTED', 5: 'SLEEPING', 6: 'BLOCKED'}
EXT = (1 << 63) | 9
M64 = (1 << 64) - 1
TIMER = (1 << 63) | 5

# v1.5 N13: the malformed disk images build-scripts/mkimages.py makes (one
# defect each; images.json gives the refusal code the kernel must report)
BAD_DISKS = ['sb_magic', 'sb_version', 'sb_crc', 'sb_small', 'sb_huge', 'sb_device', 'sb_reserved',
             'dir_magic', 'dir_crc', 'dir_reserved', 'entry_reserved', 'name_upper', 'name_pad',
             'name_long', 'dup', 'range_low', 'range_high', 'range_empty', 'size_big',
             'size_count', 'overlap', 'empty_garbage']

# test id -> list of (bootargs scenario, probe). One repetition = one clean
# boot per listed scenario.
MATRIX = {
    'T1': [('protoos.test=boot', 't1')],
    'T2': [('protoos.test=umode', 't2')],
    'T3': [('protoos.test=preempt', 't3')],
    'T4': [('protoos.test=getpid', 't4_getpid'), ('protoos.test=yield', 't4_yield'),
           ('protoos.test=exit', 't4_exit')],
    'T5': [('protoos.test=isolation', 'cross'), ('protoos.test=kernel-read', 'kernel_read')],
    'T6': [('protoos.test=fault', 't6')],
    'T7': [('protoos.mode=normal', 't7')],
    'N1': [('protoos.test=isolation', 'cross')],
    'N2': [('protoos.test=kernel-read', 'kernel_read')],
    'N3': [('protoos.test=csr', 'n3')],
    'N4': [('protoos.test=bad-syscall', 'n4')],
    'N5': [('protoos.test=idle', 'n5'), ('protoos.test=idle-fault', 'n5')],
    # v1.1 (docs/SPEC_V1_1.md)
    'T8': [('protoos.test=write', 't8')],
    'T9': [('protoos.test=sleep', 't9'), ('protoos.test=sleep-all', 't9_all')],
    'N6': [('protoos.test=write-bad', 'n6')],
    # v1.2 (docs/SPEC_V1_2.md)
    'T10': [('protoos.test=read', 't10')],
    'T11': [('protoos.test=uptime', 't11')],
    'T12': [('protoos.mode=shell', 't12')],
    'N7': [('protoos.test=read-bad', 'n7')],
    'N8': [('protoos.test=read-flood', 'n8')],
    # v1.3 (docs/SPEC_V1_3.md)
    'T13': [('protoos.test=mem', 't13')],
    'T14': [('protoos.test=spawn', 't14')],
    'T15': [('protoos.test=kill', 't15')],
    'T16': [('protoos.test=multi', 't16')],
    'T17': [('protoos.mode=shell', 't17')],
    'N9': [('protoos.test=spawn-bad', 'n9')],
    # v1.4 (docs/SPEC_V1_4.md): the third element is the program archive
    # given to QEMU as -initrd (from --images, built by build-wsl.sh)
    'T18': [('protoos.test=elf', 't18', 'tests.img')],
    'T19': [('protoos.test=sbrk', 't19', 'tests.img')],
    'T20': [('protoos.test=wait', 't20', 'tests.img')],
    'T21': [('protoos.test=ctrlc', 't21', 'tests.img')],
    'T22': [('protoos.mode=shell', 't22', 'programs.img')],
    'N10': [('protoos.test=elf-bad', 'n10', 'badelf.img')],
    'N11': [('protoos.test=archive-bad', 'n11', f'badarc/{n}.img') for n in (
        'a_magic', 'a_hex', 'a_trunc', 'a_name', 'a_longname', 'a_dir', 'a_check', 'a_data',
        'a_many', 'a_dup', 'a_notrailer', 'a_tail', 'a_big')],
    'N12': [('protoos.test=user-bad', 'n12', 'tests.img')],
    # v1.5 (docs/SPEC_V1_5.md): the fourth element is the disk: image (from
    # --images), QEMU mode (rw, noflush, ro, legacy; none: no disk at all),
    # 'share' = the same disk copy for every boot of one repetition, and the
    # test driver an AUTORUN scenario starts at boot.
    'T23': [('protoos.mode=shell', 't23', 'programs.img', {'disk': 'disk.img'})],
    'T24': [('protoos.run=t_fs', 'autorun', 'tests.img',
             {'disk': 'disks/blank.img', 'autorun': 't_fs'}),
            ('protoos.run=t_fsro', 'autorun', 'tests.img',
             {'disk': 'disks/ro.img', 'mode': 'noflush', 'autorun': 't_fsro'}),
            ('protoos.run=t_fsro', 'autorun', 'tests.img',
             {'disk': 'disks/ro.img', 'mode': 'ro', 'autorun': 't_fsro'})],
    'T25': [('protoos.run=t_pw', 'autorun', 'tests.img',
             {'disk': 'disks/blank.img', 'share': True, 'autorun': 't_pw'}),
            ('protoos.run=t_pr', 'autorun', 'tests.img',
             {'disk': 'disks/blank.img', 'share': True, 'autorun': 't_pr'})],
    'T26': [('protoos.mode=shell', 't26', 'programs.img', {'disk': 'disk.img'})],
    'T27': [('protoos.run=t_crash', 't27', 'tests.img', {'disk': 'disks/blank.img', 'share': True})],
    'N13': [('protoos.mode=shell', 'n13', 'programs.img', {'disk': f'disks/bad/{n}.img'})
            for n in BAD_DISKS] + [('protoos.mode=shell', 'n13', 'programs.img',
                                    {'disk': 'disks/fallback.img'})],
    'N14': [('protoos.run=t_fsbad', 'autorun', 'tests.img',
             {'disk': 'disks/crcbad.img', 'autorun': 't_fsbad', 'mem_check': True})],
    'N15': [('protoos.run=t_nodisk', 'autorun', 'tests.img', {'autorun': 't_nodisk'}),
            ('protoos.run=t_nodisk', 'autorun', 'tests.img',
             {'disk': 'disks/blank.img', 'mode': 'legacy', 'autorun': 't_nodisk'})],
}
# Scenarios whose probe types into the UART: the serial port is a UNIX
# socket (logged to uart.log as before) instead of a plain output file.
INPUT_SCENARIOS = {'protoos.test=read', 'protoos.mode=shell', 'protoos.test=read-bad',
                   'protoos.test=read-flood', 'protoos.test=ctrlc'}


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def tool_version(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines()[0]
    except (OSError, IndexError):
        return 'unavailable'


def git_head():
    """Read HEAD without git: WSL git refuses the Windows-owned checkout."""
    try:
        head = (ROOT / '.git/HEAD').read_text().strip()
        if not head.startswith('ref: '):
            return head
        ref = head[5:]
        loose = ROOT / '.git' / ref
        if loose.exists():
            return loose.read_text().strip()
        for line in (ROOT / '.git/packed-refs').read_text().splitlines():
            if line.endswith(' ' + ref):
                return line.split()[0]
    except OSError:
        pass
    return 'n/a'


def load_symbols(elf):
    out = subprocess.run([NM, '-n', str(elf)], capture_output=True, text=True, check=True)
    sym = {}
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            sym[parts[2]] = int(parts[0], 16)
    uva = {}
    for name, addr in sym.items():
        for prefix, text, base, size in (('a_', '__user_a_text', A_BASE, 0x1000),
                                         ('b_', '__user_b_text', B_BASE, 0x1000),
                                         ('p_', '__user_prog_text', PROG_VA, PROG_PAGES * 0x1000)):
            if name.startswith(prefix) and text in sym and sym[text] <= addr < sym[text] + size:
                uva[name] = base + addr - sym[text]
    return sym, uva


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def parse_trace(path):
    rx = re.compile(r'async:(\d), cause:([0-9a-f]+), epc:0x([0-9a-f]+), tval:0x([0-9a-f]+), desc=(\S+)')
    events = []
    for line in path.read_text(errors='replace').splitlines():
        m = rx.search(line)
        if m:
            events.append({'async': int(m[1]), 'cause': int(m[2], 16), 'epc': int(m[3], 16),
                           'tval': int(m[4], 16), 'desc': m[5]})
    return events


def in_range(v, rng):
    return rng[0] <= v < rng[1]


# ------------------------------------------------------------- one boot
def drive_args(disk):
    """v1.5: QEMU options for a disk {'path', 'mode'}. rw: virtio-mmio v2 with
    flush; noflush: write-through and no WCE, so no VIRTIO_BLK_F_FLUSH (both
    are needed: checked on QEMU 8.2.2); ro: readonly=on (VIRTIO_BLK_F_RO);
    legacy: the virt default, virtio-mmio version 1."""
    if not disk or not disk.get('path'):
        return []
    mode = disk.get('mode', 'rw')
    opt = {'noflush': ',cache=writethrough', 'ro': ',readonly=on'}.get(mode, '')
    out = ['-drive', f'if=none,file={disk["path"]},format=raw,id=hd0{opt}',
           '-device', 'virtio-blk-device,drive=hd0']
    if mode == 'noflush':
        out += ['-global', 'virtio-blk-device.config-wce=off']
    if mode != 'legacy':
        out += ['-global', 'virtio-mmio.force-legacy=false']
    return out


def qemu_cmd(elf, args, rundir, gdb_port=None, qmp=None, uart_sock=None, initrd=None,
             disk=None, trace=True):
    if uart_sock:
        serial = ['-chardev', f'socket,id=ser0,path={uart_sock},server=on,wait=off,'
                  f'logfile={rundir / "uart.log"}', '-serial', 'chardev:ser0']
    else:
        serial = ['-serial', f'file:{rundir / "uart.log"}']
    cmd = [QEMU, '-machine', 'virt', '-accel', 'tcg', '-cpu', 'rv64', '-smp', '1',
           '-m', '128M', '-bios', FW, '-kernel', str(elf)]
    if initrd:
        cmd += ['-initrd', str(initrd)]
    cmd += drive_args(disk)
    cmd += ['-display', 'none', '-monitor', 'none'] + serial + ['-nic', 'none',
           '-append', args, '-no-reboot']
    if trace:
        cmd += ['-d', 'int,guest_errors', '-D', str(rundir / 'qemu-int.log')]
    if gdb_port:
        cmd += ['-S', '-gdb', f'tcp:127.0.0.1:{gdb_port}']
    if qmp:
        cmd += ['-qmp', f'unix:{qmp},server=on,wait=off']
    return cmd


def boot_with_gdb(elf, args, probe, rundir, sym, uva, initrd=None, usym=None, disk=None):
    port = free_port()
    sock = f'/tmp/protoos-uart-{os.getpid()}-{time.time_ns()}.sock' if args in INPUT_SCENARIOS else None
    cmd = qemu_cmd(elf, args, rundir, gdb_port=port, uart_sock=sock, initrd=initrd, disk=disk)
    cfg = {'probe': probe, 'symbols': sym, 'user_va': uva, 'elf': str(elf), 'port': port,
           'observations': str(rundir / 'observations.json'), 'uart_sock': sock,
           'user_symbols': usym or {}, 'autorun': (disk or {}).get('autorun'),
           'mem_check': bool((disk or {}).get('mem_check'))}
    (rundir / 'probe-config.json').write_text(json.dumps(cfg, indent=1))
    (rundir / 'command.json').write_text(json.dumps({'qemu': cmd}, indent=1))
    env = dict(os.environ, PROTOOS_PROBE_CFG=str(rundir / 'probe-config.json'))
    vm = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        with (rundir / 'gdb.log').open('w') as log:
            try:
                subprocess.run([GDB, '-nx', '-batch', '-x', str(ROOT / 'validation/gdb_probe.py')],
                               env=env, stdout=log, stderr=subprocess.STDOUT, timeout=240)
            except subprocess.TimeoutExpired:
                log.write('\nHARNESS: gdb timeout\n')
    finally:
        vm.terminate()
        try:
            vm.wait(timeout=10)
        except subprocess.TimeoutExpired:
            vm.kill()
            vm.wait()
        if sock and os.path.exists(sock):
            os.unlink(sock)
    p = rundir / 'observations.json'
    return json.loads(p.read_text()) if p.exists() else {'steps': [], 'complete': False,
                                                         'error': 'no observations'}


def boot_crash_series(elf, args, rundir, initrd, disk, seed, kills=20):
    """v1.5 T27: boot `kills` times on the same disk copy, no debugger; each
    boot is killed (SIGKILL) at a random moment, from a seeded RNG, after the
    test driver started its write loop. After every kill: pfsck, and the
    files' generations against what the driver reported as saved."""
    import random
    sys.path.insert(0, str(ROOT / 'build-scripts'))
    import pfsck
    rng = random.Random(seed)
    steps = []
    for i in range(kills):
        bdir = rundir / f'kill{i:02d}'
        bdir.mkdir()
        cmd = qemu_cmd(elf, args, bdir, initrd=initrd, disk=disk, trace=False)
        (bdir / 'command.json').write_text(json.dumps({'qemu': cmd}, indent=1))
        delay = rng.uniform(0.2, 2.5)
        vm = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        started = False
        try:
            end = time.monotonic() + 60
            while time.monotonic() < end and not started:
                time.sleep(0.05)
                log = bdir / 'uart.log'
                started = log.exists() and 't_crash: starting' in log.read_text(errors='replace')
            time.sleep(delay)
        finally:
            vm.kill()
            vm.wait()
        uart = (bdir / 'uart.log').read_text(errors='replace') if (bdir / 'uart.log').exists() else ''
        # only whole lines: the kill can cut the last one ("saved g 12" of 123)
        saved = {f: [int(x) for x in re.findall(rf'^saved {f} (\d+)\r?\n', uart, re.M)]
                 for f in 'gh'}
        img = pathlib.Path(disk['path']).read_bytes()
        res = pfsck.check(img)
        files = {}
        for f in res['files']:
            data = img[f['first'] * 4096:f['first'] * 4096 + f['size']]
            gen = int.from_bytes(data[:8], 'little') if len(data) >= 8 else None
            files[f['name']] = {'size': f['size'], 'gen': gen, 'crc_ok': f['crc_ok'],
                                'consistent': gen is not None and data == crash_content(gen)}
        steps.append({'name': f'kill{i:02d}', 'started': started, 'delay': round(delay, 3),
                      'saved_last': {f: (v[-1] if v else None) for f, v in saved.items()},
                      'saved_count': {f: len(v) for f, v in saved.items()},
                      'start_gen': [int(x) for x in re.findall(
                          r't_crash: starting after generation (\d+)', uart)],
                      'pfsck': {k: res[k] for k in ('mounts', 'reject', 'slot', 'seq', 'data_ok')},
                      'files': files, 'disk_sha256': hashlib.sha256(img).hexdigest()})
    return {'probe': 't27', 'steps': steps, 'complete': True, 'error': None, 'seed': seed}


def crash_content(gen):
    """What user/tests/t_crash.c writes for generation gen."""
    n = 1000 + (gen % 7) * 9000
    body = bytes((i * 131 + (gen & 255) * 17 + 7) & 255 for i in range(n))
    return gen.to_bytes(8, 'little') + body[8:]


class Qmp:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.05)
        self.f = self.s.makefile('rw')
        json.loads(self.f.readline())
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            msg = json.loads(self.f.readline())
            if 'return' in msg or 'error' in msg:
                return msg

    def xp(self, addr):
        r = self.cmd('human-monitor-command', **{'command-line': f'xp /1gx {addr:#x}'})
        return int(r['return'].split(':')[1].strip(), 16)


def boot_free_running(elf, args, rundir, sym, offsets, seconds):
    """T7: no debugger, no -S. QEMU monitor (QMP) only reads physical memory."""
    sock = f'/tmp/protoos-qmp-{os.getpid()}-{time.time_ns()}.sock'
    cmd = qemu_cmd(elf, args, rundir, qmp=sock)
    (rundir / 'command.json').write_text(json.dumps({'qemu': cmd}, indent=1))
    vm = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    samples, error = [], None
    try:
        q = Qmp(sock)
        t0 = time.monotonic()
        time.sleep(1.0)
        n = int(seconds // 5)
        for i in range(n + 1):
            if i:
                time.sleep(max(0.0, t0 + 1.0 + 5 * i - time.monotonic()))
            s = {'host_elapsed': time.monotonic() - t0}
            for k in ('counter', 'pid', 'error'):
                off = {'counter': 0, 'pid': 8, 'error': 16}[k]
                s[f'a_{k}'] = q.xp(sym['user_a_data'] + off)
                s[f'b_{k}'] = q.xp(sym['user_b_data'] + off)
            for k in ('timer_ticks', 'switches', 'idle_entered'):
                s[k] = q.xp(sym['sched'] + offsets[f'sched.{k}'])
            for i_p in (0, 1):
                base = sym['procs'] + i_p * offsets['sizeof_process']
                for k in ('state', 'preemptions', 'syscalls'):
                    s[f'p{i_p}_{k}'] = q.xp(base + offsets[f'process.{k}'])
            samples.append(s)
    except Exception as e:
        error = f'{type(e).__name__}: {e}'
    finally:
        vm.terminate()
        try:
            vm.wait(timeout=10)
        except subprocess.TimeoutExpired:
            vm.kill()
            vm.wait()
        if os.path.exists(sock):
            os.unlink(sock)
    obs = {'probe': 't7', 'samples': samples, 'complete': error is None and bool(samples),
           'error': error, 'seconds': seconds}
    (rundir / 'observations.json').write_text(json.dumps(obs, indent=1))
    return obs


def struct_offsets(elf):
    exprs = {
        'sizeof_process': 'sizeof(struct process)',
        'process.state': '&((struct process *)0)->state',
        'process.preemptions': '&((struct process *)0)->preemptions',
        'process.syscalls': '&((struct process *)0)->syscalls',
        'sched.timer_ticks': '&((struct sched_stats *)0)->timer_ticks',
        'sched.switches': '&((struct sched_stats *)0)->switches',
        'sched.idle_entered': '&((struct sched_stats *)0)->idle_entered',
    }
    args = [GDB, '-nx', '-batch', '-ex', f'file {elf}']
    for e in exprs.values():
        args += ['-ex', f'printf "%lu\\n", (unsigned long)({e})']
    out = subprocess.run(args, capture_output=True, text=True, check=True).stdout.split()
    return {k: int(v) for k, v in zip(exprs, out[-len(exprs):])}


# --------------------------------------------------------------- oracles
class Checks:
    def __init__(self):
        self.items = []

    def __call__(self, name, ok, detail=''):
        self.items.append({'check': name, 'ok': bool(ok), 'detail': str(detail)})
        return bool(ok)


def steps(obs):
    return {s['name']: s for s in obs.get('steps', [])}


def words_from_hex(h):
    raw = bytes.fromhex(h)
    return [int.from_bytes(raw[i:i + 8], 'little') for i in range(0, len(raw), 8)]


def common_checks(c, ctx):
    ev, uart = ctx['trace'], ctx['uart']
    kernel = (ctx['sym']['__kernel_start'], ctx['sym']['__image_end'])
    bad = [e for e in ev if not e['async'] and in_range(e['epc'], kernel) and e['cause'] != 9]
    c('no S-mode exception except SBI ecall (trace)', not bad, bad[:3])
    c('no KERNEL_PANIC on UART', 'KERNEL_PANIC' not in uart)
    c('ELF unchanged during run', ctx['sha_after'] == ctx['sha'], ctx['sha_after'])
    if ctx.get('image'):
        c('program archive unchanged during run', ctx['image_sha_after'] == ctx['image_sha'],
          ctx['image_sha_after'])


def u_events(ev, rng, **kw):
    return [e for e in ev if in_range(e['epc'], rng) and all(e[k] == v for k, v in kw.items())]


def check_cpu_user(c, label, cpu_, satp, rng):
    c(f'{label}: privilege U (priv=0)', cpu_['priv'] == 0, cpu_['priv'])
    c(f'{label}: satp is the process root', cpu_['satp'] == satp, hex(cpu_['satp']))
    c(f'{label}: pc inside its user text', in_range(cpu_['pc'], rng), hex(cpu_['pc']))


def oracle_t1(c, obs, ctx):
    st = steps(obs)
    a, e, k, h = st['attach'], st['kernel_entry'], st['kernel_main'], st['halt']
    c('reset state observed in M-mode before firmware (priv=3)', a['cpu']['priv'] == 3, a['cpu'])
    c('kernel entry reached', e['hit'])
    c('entry pc == ELF _start == 0x80200000', e['cpu']['pc'] == ctx['sym']['_start'] == 0x80200000,
      hex(e['cpu']['pc']))
    c('entry privilege S (priv=1) after OpenSBI handoff', e['cpu']['priv'] == 1, e['cpu']['priv'])
    c('OpenSBI handoff args: hart 0, DTB pointer', e['a0'] == 0 and 0x80000000 <= e['a1'] < 0x88000000,
      (e['a0'], hex(e['a1'])))
    c('kernel_main executes in S-mode', k['hit'] and k['cpu']['priv'] == 1, k['cpu']['priv'])
    c('halt reached in S-mode', h['hit'] and h['cpu']['priv'] == 1)
    s = h['cpu']['satp']
    c('Sv39 active in S-mode (satp.MODE=8, kernel root)', s >> 60 == 8 and s == h['satps']['kernel'],
      hex(s))
    c('boot_phase == 3 (READY path)', h['boot_phase'] == 3, h['boot_phase'])
    wt, wr, ws = h['walk_text'], h['walk_rodata'], h['walk_stack']
    c('PT walk: kernel text R+X, no U, no W', wt['mapped'] and set('RX') <= set(wt['flags'])
      and 'U' not in wt['flags'] and 'W' not in wt['flags'], wt.get('flags'))
    c('PT walk: kernel rodata R only, no U', wr['mapped'] and 'W' not in wr['flags']
      and 'X' not in wr['flags'] and 'U' not in wr['flags'], wr.get('flags'))
    c('PT walk: kernel stack R+W, no U', ws['mapped'] and set('RW') <= set(ws['flags'])
      and 'U' not in ws['flags'], ws.get('flags'))
    c('PT walk: kernel stack guard page unmapped', not h['walk_guard']['mapped'])
    c('PT walk: user pages absent from kernel root', not h['walk_user_a']['mapped'])
    c('UART correlates (banner + READY)', 'BOOT_FOUNDATION_READY' in ctx['uart'])


def oracle_t2(c, obs, ctx):
    st = steps(obs)
    s = st['A_in_user_1']['satps']
    c('distinct page-table roots A/B', s['A'] != s['B'] and s['A'] >> 60 == 8 == s['B'] >> 60,
      (hex(s['A']), hex(s['B'])))
    for rnd in (1, 2):
        a, b = st[f'A_in_user_{rnd}'], st[f'B_in_user_{rnd}']
        c(f'round {rnd}: A reached', a['hit'])
        c(f'round {rnd}: B reached', b['hit'])
        check_cpu_user(c, f'A{rnd}', a['cpu'], s['A'], A_RANGE)
        check_cpu_user(c, f'B{rnd}', b['cpu'], s['B'], B_RANGE)
        c(f'A{rnd}: sp in A stack', A_BASE + 0x3000 <= a['gprs']['x2'] <= A_BASE + 0x4000,
          hex(a['gprs']['x2']))
        c(f'B{rnd}: sp in B stack', B_BASE + 0x3000 <= b['gprs']['x2'] <= B_BASE + 0x4000,
          hex(b['gprs']['x2']))
        c(f'A{rnd}: s2..s10 carry A pattern', all(a['gprs'][f'x{18 + k}'] == A_PATTERN + 2 + k
                                               for k in range(9)))
        c(f'B{rnd}: s2..s10 carry B pattern', all(b['gprs'][f'x{18 + k}'] == B_PATTERN + 2 + k
                                               for k in range(9)))
        c(f'A{rnd}: saved B context belongs to B', in_range(a['other_frame']['sepc'], B_RANGE)
          or a['other_frame']['sepc'] == 0, hex(a['other_frame']['sepc']))
        c(f'B{rnd}: saved A context belongs to A', in_range(b['other_frame']['sepc'], A_RANGE),
          hex(b['other_frame']['sepc']))
    a1, a2 = st['A_in_user_1']['words_a'][0], st['A_in_user_2']['words_a'][0]
    b1, b2 = st['B_in_user_1']['words_b'][0], st['B_in_user_2']['words_b'][0]
    c('A counter (physical) advances', a2 > a1, (a1, a2))
    c('B counter (physical) advances', b2 > b1, (b1, b2))
    c('trace: timer interrupts taken from both U ranges',
      u_events(ctx['trace'], A_RANGE, cause=5) and u_events(ctx['trace'], B_RANGE, cause=5))


def oracle_t3(c, obs, ctx):
    st = steps(obs)
    pre, b, res = st['A_preempted'], st['B_trapped'], st['A_resumed']
    f = pre['frame']
    c('A preempted by S timer interrupt (scause=0x8000000000000005)', pre['hit']
      and f['scause'] == TIMER, hex(f['scause']))
    c('interrupted A pc is user code', in_range(f['sepc'], A_RANGE), hex(f['sepc']))
    c('A made no syscall (no yield)', pre['proc_a']['syscalls'] == 0)
    c('trace: async cause 5 at the saved A epc', any(e['async'] and e['cause'] == 5 and
                                                       e['epc'] == f['sepc'] for e in ctx['trace']))
    c('trace: no user ecall at all in this scenario', not [e for e in ctx['trace'] if e['cause'] == 8
                                                            and not e['async']])
    fb = b['frame_b']
    c('B traps before A executes again', b['first'] == 'B', b['first'])
    c('B trap came from U-mode (saved sstatus.SPP=0) at B user pc',
      not fb['sstatus'] >> 8 & 1 and in_range(fb['sepc'], B_RANGE), (hex(fb['sstatus']), hex(fb['sepc'])))
    c('B executed in between (B dispatched again since A was preempted)',
      b['proc_b']['dispatches'] > pre['proc_b']['dispatches'])
    c('A not dispatched while B ran', b['proc_a']['dispatches'] == pre['proc_a']['dispatches'])
    c('B trap taken under B root', b['cpu']['satp'] == b['satps']['B'], hex(b['cpu']['satp']))
    c('A resumed', res['hit'])
    check_cpu_user(c, 'A resumed', res['cpu'], res['satps']['A'], A_RANGE)
    c('A resumed exactly at interrupted pc (no skipped instruction)', res['cpu']['pc'] == f['sepc'],
      (hex(res['cpu']['pc']), hex(f['sepc'])))
    diff = [k for k in res['gprs'] if res['gprs'][k] != f[k]]
    c('all 31 GPRs of A restored bit-exactly', not diff, diff)
    c('B was preempted in between (>= 2 timer ticks)', res['sched']['timer_ticks']
      >= pre['sched']['timer_ticks'] + 2, (pre['sched']['timer_ticks'], res['sched']['timer_ticks']))


def oracle_t4_getpid(c, obs, ctx):
    st = steps(obs)
    for who, pid, rng, ret in (('A', 1, A_RANGE, 'a_getpid_ret'), ('B', 2, B_RANGE, 'b_getpid_ret')):
        e, r = st[f'{who}_ecall'], st[f'{who}_returned']
        c(f'{who}: ecall trap scause=8 (U ecall)', e['hit'] and e['frame']['scause'] == 8)
        c(f'{who}: a7=172 getpid', e['frame']['x17'] == 172)
        c(f'{who}: trap handled in S-mode', e['cpu']['priv'] == 1)
        c(f'{who}: returns to ecall+4 in U-mode', r['hit'] and r['cpu']['pc'] == e['frame']['sepc'] + 4
          == ctx['uva'][ret] and r['cpu']['priv'] == 0, hex(r['cpu']['pc']))
        c(f'{who}: getpid() == {pid}', r['gprs']['x10'] == pid, r['gprs']['x10'])
        c(f'{who}: trace user_ecall at the ecall pc', u_events(ctx['trace'], rng, cause=8,
                                                                epc=e['frame']['sepc']))
    a = st['after']
    c('stored results: A pid 1/1, B pid 2/2 (physical)', a['words_a'][1] == 1 and a['words_a'][6] == 1
      and a['words_b'][1] == 2 and a['words_b'][6] == 2, (a['words_a'][1], a['words_b'][1]))
    c('no self-detected error', a['words_a'][2] == 0 and a['words_b'][2] == 0)


def oracle_t4_yield(c, obs, ctx):
    st = steps(obs)
    e, b, r, a = st['A_ecall'], st['B_runs'], st['A_resumed'], st['after']
    c('A ecall trap, a7=124 yield', e['hit'] and e['frame']['scause'] == 8 and e['frame']['x17'] == 124)
    c('B runs right after A yielded', b['hit'])
    check_cpu_user(c, 'B', b['cpu'], b['satps']['B'], B_RANGE)
    c('switch caused by yield, not the timer (tick count unchanged)',
      b['sched']['timer_ticks'] == e['sched']['timer_ticks'],
      (e['sched']['timer_ticks'], b['sched']['timer_ticks']))
    c('A resumes after its ecall with a0 == 0', r['hit'] and r['cpu']['pc'] == ctx['uva']['a_yield_ret']
      and r['gprs']['x10'] == 0 and r['cpu']['priv'] == 0)
    c('A resumes under its own satp', r['cpu']['satp'] == r['satps']['A'])
    c('both processes kept yielding (yield counters > 1)', a['procs'][0]['yields'] > 1
      and a['procs'][1]['yields'] > 1, (a['procs'][0]['yields'], a['procs'][1]['yields']))
    c('no self-detected error', a['words_a'][2] == 0 and a['words_b'][2] == 0)


def oracle_t4_exit(c, obs, ctx):
    st = steps(obs)
    e, a = st['A_exit_ecall'], st['after']
    c('A ecall trap, a7=93 exit, a0=42', e['hit'] and e['frame']['scause'] == 8
      and e['frame']['x17'] == 93 and e['frame']['x10'] == 42)
    c('A did its 1000 iterations before exit', e['words_a'][0] == 1000, e['words_a'][0])
    for w in ('wait1', 'wait2'):
        c(f'{w}: A never dispatched again / post-exit code never ran',
          a[w]['reached'] and a[w]['watch_hit'] is None, a[w])
    c('A state EXITED, exit_code 42', a['procs'][0]['state'] == 3 and a['procs'][0]['exit_code'] == 42)
    c('A counter frozen at 1000', a['first']['words_a'][0] == a['words_a'][0] == 1000)
    c('B keeps running (counter advances)', a['words_b'][0] > a['first']['words_b'][0],
      (a['first']['words_b'][0], a['words_b'][0]))
    c('trace: exactly one user ecall from A', len(u_events(ctx['trace'], A_RANGE, cause=8)) == 1)


def canary_ok(words):
    return all(words[256 + i] == B_PATTERN ^ i for i in range(256))


def oracle_cross(c, obs, ctx):
    st = steps(obs)
    pre, f, a = st['before_store'], st['fault'], st['after']
    s = pre['satps']
    target = pre['gprs']['x5']
    c('attacker A in U-mode at the store', pre['hit'] and pre['cpu']['priv'] == 0
      and pre['cpu']['satp'] == s['A'])
    c('distinct Sv39 roots A/B/kernel', len({s['A'], s['B'], s['kernel']}) == 3)
    c('target VA is B data page', target == B_BASE + 0x1800, hex(target))
    c('PT walk: target unmapped in A root', not pre['walk_a_target']['mapped'])
    wb = pre['walk_b_target']
    c('PT walk: target mapped in B root, U+R+W, PA in B data page', wb['mapped']
      and set('URW') <= set(wb['flags']) and wb['pa'] == ctx['sym']['user_b_data'] + 0x800,
      wb.get('flags'))
    wk = pre['walk_a_kernel']
    c('PT walk: kernel pages in A root carry U=0', wk['mapped'] and 'U' not in wk['flags'],
      wk.get('flags'))
    c('PT walk: A own data mapped U', pre['walk_a_own']['mapped'] and 'U' in pre['walk_a_own']['flags'])
    bw = words_from_hex(pre['b_data'])
    c('B canary written before the attack', canary_ok(bw))
    c('store page fault (scause=15) on the target', f['hit'] and f['frame']['scause'] == 15
      and f['frame']['stval'] == target and f['frame']['sepc'] == ctx['uva']['a_cross_write_store'])
    c('fault attributed to A', f['p'] == ctx['procs_addr'][0], hex(f['p']))
    c('trace: store_page_fault from A at the store pc', u_events(ctx['trace'], A_RANGE, cause=15,
                                                                   tval=target))
    c('B data page bytes 8..4095 unchanged (physical)', pre['b_data'][16:] == f['b_data'][16:]
      == a['b_data'][16:])
    c('B stack page unchanged (physical)', pre['b_stack'] == f['b_stack'])
    c('no 0x4141.. value anywhere in B data', 0x4141414141414141 not in words_from_hex(a['b_data']))
    for w in ('wait1', 'wait2'):
        c(f'{w}: A never dispatched again', a[w]['reached'] and a[w]['watch_hit'] is None)
    c('A FAULTED, B alive', a['procs'][0]['state'] == 4 and a['procs'][1]['state'] in (1, 2))
    c('A stage marker shows store never completed', a['words_a'][5] == 1, a['words_a'][5])
    c('B continues (counter advances)', a['words_b'][0] > a['words_b_first'][0])


def oracle_kernel_read(c, obs, ctx):
    st = steps(obs)
    pre, f, a = st['before_load'], st['fault'], st['after']
    secret = ctx['sym']['kernel_secret']
    c('A in U-mode at the kernel load', pre['hit'] and pre['cpu']['priv'] == 0
      and pre['cpu']['satp'] == pre['satps']['A'])
    c('target is kernel_secret', pre['gprs']['x5'] == secret, hex(pre['gprs']['x5']))
    w = pre['walk_a_secret']
    c('PT walk: kernel page present in A root with U=0', w['mapped'] and 'U' not in w['flags']
      and 'R' in w['flags'], w.get('flags'))
    c('load page fault (scause=13) at the load', f['hit'] and f['frame']['scause'] == 13
      and f['frame']['stval'] == secret and f['frame']['sepc'] == ctx['uva']['a_kernel_read_load'])
    c('no data returned: a1 keeps sentinel', f['frame']['x11'] == 0x5a5a5a5a5a5a5a5a,
      hex(f['frame']['x11']))
    c('secret never reached user memory', a['words_a'][4] == 0 and pre['secret_phys'] not in a['words_a'])
    c('trace: load_page_fault from A', u_events(ctx['trace'], A_RANGE, cause=13, tval=secret))
    c('A FAULTED, B alive and advancing', a['procs'][0]['state'] == 4 and a['procs'][1]['state']
      in (1, 2) and a['words_b'][0] > a['words_b_first'][0])


def oracle_t6(c, obs, ctx):
    st = steps(obs)
    pre, f, r, a = st['before_store'], st['fault'], st['B_resumed'], st['after']
    wt = pre['walk_a_target']
    c('A in U-mode at the faulting store', pre['hit'] and pre['cpu']['priv'] == 0)
    c('PT walk: target is A text, R+X+U, no W', wt['mapped'] and 'W' not in wt['flags']
      and set('RXU') <= set(wt['flags']), wt.get('flags'))
    c('real user page fault (scause=15) at the store', f['hit'] and f['frame']['scause'] == 15
      and f['frame']['sepc'] == ctx['uva']['a_fault_store'])
    c('trace: store_page_fault from A', u_events(ctx['trace'], A_RANGE, cause=15))
    c('fault attributed to A', f['p'] == ctx['procs_addr'][0])
    c('B resumes after the fault', r['hit'])
    check_cpu_user(c, 'B resumed', r['cpu'], r['satps']['B'], B_RANGE)
    diff = [k for k in r['gprs'] if r['gprs'][k] != r['frame_b'][k]]
    c('B context restored bit-exactly', not diff and r['cpu']['pc'] == r['frame_b']['sepc'], diff)
    for w in ('wait1', 'wait2'):
        c(f'{w}: A never dispatched again', a[w]['reached'] and a[w]['watch_hit'] is None)
    c('A FAULTED; B READY/RUNNING', a['procs'][0]['state'] == 4 and a['procs'][1]['state'] in (1, 2))
    c('B keeps executing (counter advances)', a['words_b'][0] > a['words_b_first'][0])
    c('B memory intact except its own counter', pre['b_data'][16:] == a['b_data'][16:]
      and pre['b_stack'] == a['b_stack'])
    c('B self-check found no corruption', a['words_b'][2] == 0)


def oracle_t7(c, obs, ctx):
    smp = obs['samples']
    want = ctx['t7_seconds'] // 5 + 1
    c(f'sampled the free-running Normal boot every 5 s ({want} samples)', len(smp) == want >= 3,
      len(smp))
    dur = smp[-1]['host_elapsed'] - smp[0]['host_elapsed'] if smp else 0
    c(f'observation window >= {ctx["t7_seconds"] - 1} s', dur >= ctx['t7_seconds'] - 1, round(dur, 2))
    for i in range(1, len(smp)):
        p, q = smp[i - 1], smp[i]
        c(f'interval {i}: A and B counters advance', q['a_counter'] > p['a_counter']
          and q['b_counter'] > p['b_counter'])
        c(f'interval {i}: timer switches advance', q['switches'] > p['switches']
          and q['timer_ticks'] > p['timer_ticks'])
        c(f'interval {i}: A and B preempted by timer', q['p0_preemptions'] > p['p0_preemptions']
          and q['p1_preemptions'] > p['p1_preemptions'])
    last = smp[-1] if smp else {}
    c('no process EXITED/FAULTED, kernel not idle', last.get('p0_state') in (1, 2)
      and last.get('p1_state') in (1, 2) and last.get('idle_entered') == 0)
    c('no syscalls at all (no external/cooperative control)', last.get('p0_syscalls') == 0
      and last.get('p1_syscalls') == 0)
    c('no self-detected context corruption', last.get('a_error') == 0 and last.get('b_error') == 0)
    ev = ctx['trace']
    ta, tb = u_events(ev, A_RANGE, cause=5), u_events(ev, B_RANGE, cause=5)
    c('trace: timer interrupts from A and from B user code', ta and tb, (len(ta), len(tb)))
    c('trace: zero user ecalls / user faults', not [e for e in ev if not e['async'] and
                                                    (in_range(e['epc'], A_RANGE) or in_range(e['epc'], B_RANGE))])
    obs['summary'] = {'duration_s': round(dur, 2), 'switches': last.get('switches'),
                      'timer_ticks': last.get('timer_ticks'), 'trace_timer_A': len(ta),
                      'trace_timer_B': len(tb), 'a_counter': last.get('a_counter'),
                      'b_counter': last.get('b_counter')}


def oracle_n3(c, obs, ctx):
    st = steps(obs)
    pre, f, a = st['before_csr'], st['fault'], st['after']
    c('A in U-mode at csrw satp', pre['hit'] and pre['cpu']['priv'] == 0)
    c('illegal instruction trap (scause=2) at the csrw', f['hit'] and f['frame']['scause'] == 2
      and f['frame']['sepc'] == ctx['uva']['a_csr_write'])
    c('stval holds the csrw satp encoding', f['frame']['stval'] & 0xfff0707f == 0x18001073,
      hex(f['frame']['stval']))
    c('satp unchanged by the attempt (still A root)', f['cpu']['satp'] == pre['satps']['A'],
      hex(f['cpu']['satp']))
    c('trace: illegal_instruction from A', u_events(ctx['trace'], A_RANGE, cause=2))
    c('A FAULTED, B alive and advancing', a['procs'][0]['state'] == 4
      and a['procs'][1]['state'] in (1, 2) and a['words_b'][0] > a['words_b_first'][0])


def oracle_n4(c, obs, ctx):
    st = steps(obs)
    e, r, a = st['A_ecall'], st['A_returned'], st['after']
    c('ecall trap with a7=999', e['hit'] and e['frame']['scause'] == 8 and e['frame']['x17'] == 999)
    c('returns to ecall+4 in U-mode with a0 = -38', r['hit'] and r['cpu']['priv'] == 0
      and r['cpu']['pc'] == e['frame']['sepc'] + 4 and r['gprs']['x10'] == (-38) & M64,
      hex(r['gprs']['x10']))
    c('rejection counted once, kernel healthy', a['procs'][0]['rejected_syscalls'] == 1
      and a['wait1']['reached'] and a['wait2']['reached'])
    c('A keeps running (worker counter advances)', a['words_a'][0] > a['first']['words_a'][0])
    c('B keeps running (counter advances)', a['words_b'][0] > a['first']['words_b'][0])
    c('both still READY/RUNNING', a['procs'][0]['state'] in (1, 2) and a['procs'][1]['state'] in (1, 2))
    c('A stored -38 and passed its own check', a['words_a'][4] == (-38) & M64 and a['words_a'][5] == 1
      and a['words_a'][2] == 0)


def oracle_n5(c, obs, ctx):
    st = steps(obs)
    i, w = st['idle_entry'], st['idle_window']
    states = [p['state'] for p in i['procs']]
    c('idle entered', i['hit'] and i['cpu']['priv'] == 1)
    c('no READY/RUNNING process at idle', all(s in (3, 4) for s in states),
      [STATE.get(s) for s in states])
    c('no trap/dispatch/sret for 3 s after idle', w['watch_hit'] is None, w['watch_hit'])
    c('parked in S-mode halt loop', w['cpu']['priv'] == 1 and in_range(
        w['cpu']['pc'], (ctx['sym']['halt'], ctx['sym']['halt'] + 0x40)), hex(w['cpu']['pc']))
    c('interrupt enables cleared (sie=0)', w['cpu']['sie'] == 0)
    c('kernel root installed (no process context loaded)', w['cpu']['satp'] == w['satps']['kernel'])
    last_u = max([n for n, e in enumerate(ctx['trace']) if in_range(e['epc'], A_RANGE)
                  or in_range(e['epc'], B_RANGE)] or [-1])
    tail = [e for e in ctx['trace'][last_u + 1:] if e['desc'] in ('s_timer', 'user_ecall')]
    c('trace: no user-level event after the last process ended', not tail, tail[:3])


ORACLES = {
    't1': oracle_t1, 't2': oracle_t2, 't3': oracle_t3, 't4_getpid': oracle_t4_getpid,
    't4_yield': oracle_t4_yield, 't4_exit': oracle_t4_exit, 'cross': oracle_cross,
    'kernel_read': oracle_kernel_read, 't6': oracle_t6, 't7': oracle_t7,
    'n3': oracle_n3, 'n4': oracle_n4, 'n5': oracle_n5,
}


# ------------------------------------------------------------- v1.1 oracles
T8_MSG = 'ProtoOS T8: hello from user mode A'
SUM = 1 << 18


def oracle_t8(c, obs, ctx):
    st = steps(obs)
    n = len(T8_MSG) + 1
    for k in (1, 2):
        e, r = st[f'write{k}_ecall'], st[f'write{k}_ret']
        f = e['frame']
        c(f'write {k}: ecall trap a7=64, fd=1, len={n}', e['hit'] and f['scause'] == 8
          and f['x17'] == 64 and f['x10'] == 1 and f['x12'] == n, (f['x17'], f['x10'], f['x12']))
        for w in ('walk_first', 'walk_last'):
            c(f'write {k}: buffer {w[5:]} byte is U+R in A root (PT walk)', e[w]['mapped']
              and set('UR') <= set(e[w]['flags']), e[w].get('flags'))
        c(f'write {k}: returns len to ecall+4 in U-mode', r['hit'] and r['cpu']['priv'] == 0
          and r['cpu']['pc'] == ctx['uva']['a_write_ret'] and r['gprs']['x10'] == n,
          hex(r['gprs']['x10']))
        c(f'write {k}: SUM=0 back in U-mode', not r['cpu']['sstatus'] & SUM,
          hex(r['cpu']['sstatus']))
    c('UART: message printed exactly twice', ctx['uart'].count(T8_MSG) == 2,
      ctx['uart'].count(T8_MSG))
    a = st['after']
    c('kernel counted 2 writes, 2*len bytes', a['procs'][0]['writes'] == 2
      and a['procs'][0]['bytes_written'] == 2 * n)
    c('A continues (worker counter advances), no error', a['words_a'][0] > a['words_a_first'][0]
      and a['words_a'][2] == 0 and a['words_a'][5] == 1)
    c('trace: exactly two user ecalls from A', len(u_events(ctx['trace'], A_RANGE, cause=8)) == 2)


N6_EXPECT = [-9, -14, -14, -14, -22, -14, -14, len('N6 ok') + 1]
N6_NAMES = ['wrong fd', 'kernel page', "B's page", 'crosses into guard page', 'len > 256',
            'NULL', 'address wrap-around', 'valid write after rejections']


def oracle_n6(c, obs, ctx):
    st = steps(obs)
    d, a = st['done'], st['after']
    c('A reached the end of the sequence in U-mode', d['hit'] and d['cpu']['priv'] == 0)
    for k, (name, want) in enumerate(zip(N6_NAMES, N6_EXPECT)):
        c(f'{name}: returns {want}', d['results'][k] == want & M64, hex(d['results'][k]))
    c('PT walk: kernel page U=0, B page and guard unmapped in A root',
      d['walk_secret']['mapped'] and 'U' not in d['walk_secret']['flags']
      and not d['walk_b_data']['mapped'] and not d['walk_guard']['mapped'])
    uart = ctx['uart']
    i, j = uart.find('N6-begin'), uart.find('N6 ok')
    between = ([ln for ln in uart[i + len('N6-begin'):j].splitlines()
                if ln.strip() and not ln.startswith('SCHED')] if 0 <= i < j else ['markers missing'])
    c('nothing printed for the rejected calls', not between, between[:3])
    c('only the 2 valid writes were counted', d['proc_a']['writes'] == 2
      and d['proc_a']['bytes_written'] == len('N6-begin\n') + len('N6 ok\n'))
    c('A not faulted, keeps running; B keeps running', a['procs'][0]['state'] in (1, 2)
      and a['words_a'][0] > a['words_a_first'][0] and a['words_b'][0] > a['words_b_first'][0])
    c('trace: no exception from A other than ecall',
      not [e for e in u_events(ctx['trace'], A_RANGE) if not e['async'] and e['cause'] != 8])


def oracle_t9(c, obs, ctx):
    st = steps(obs)
    e, b, w = st['sleep_ecall'], st['B_running'], st['A_woke']
    c('sleep ecall from A: a7=1001, ms=200', e['hit'] and e['frame']['scause'] == 8
      and e['frame']['x17'] == 1001 and e['frame']['x10'] == 200)
    c('B runs in U-mode under its own root while A sleeps', b['hit'] and b['cpu']['priv'] == 0
      and b['cpu']['satp'] == b['satps']['B'])
    pa = b['proc_a']
    c('A is SLEEPING while B runs', pa['state'] == 5, STATE.get(pa['state']))
    tb = w['timebase']
    need = 200 * tb // 1000
    c('wake time = sleep start + 200 ms', pa['wake_time'] == pa['sleep_start'] + need)
    c('A woke at its next instruction in U-mode with a0 = 0', w['hit'] and w['cpu']['priv'] == 0
      and w['cpu']['pc'] == ctx['uva']['a_sleep_ret'] and w['gprs']['x10'] == 0
      and w['cpu']['satp'] == w['satps']['A'])
    slept = w['proc_a']['last_dispatch_time'] - pa['sleep_start']
    quantum = w['sched']['quantum_ticks']
    c('slept >= 200 ms and <= 200 ms + 2 quanta (kernel time stamps)',
      need <= slept <= need + 2 * quantum, f'{slept / tb * 1000:.2f} ms')
    c('A dispatched exactly once since it went to sleep',
      w['proc_a']['dispatches'] == e['proc_a']['dispatches'] + 1,
      (e['proc_a']['dispatches'], w['proc_a']['dispatches']))
    c('B dispatched in between', w['proc_b']['dispatches'] > e['proc_b']['dispatches'])


def oracle_t9_all(c, obs, ctx):
    st = steps(obs)
    w, a = st['wait'], st['after']
    c('kernel waits only when both processes sleep', w['hit'] and w['cpu']['priv'] == 1
      and [p['state'] for p in w['procs']] == [5, 5], [STATE.get(p['state']) for p in w['procs']])
    c('wait deadline = earliest wake-up', w['wake'] == min(p['wake_time'] for p in w['procs']))
    c('no idle, fault or panic within 2 s', a['watch_hit'] is None, a['watch_hit'])
    c('both processes keep making progress', a['words_a'][0] > a['words_a_first'][0]
      and a['words_b'][0] > a['words_b_first'][0],
      (a['words_a_first'][0], a['words_a'][0], a['words_b_first'][0], a['words_b'][0]))
    c('kernel waited repeatedly, never idle', a['sched']['waits'] > w['sched']['waits']
      and a['sched']['idle_entered'] == 0)
    c('no exception from user code (trace)', not [e for e in ctx['trace'] if not e['async']
      and (in_range(e['epc'], A_RANGE) or in_range(e['epc'], B_RANGE)) and e['cause'] != 8])


ORACLES.update({'t8': oracle_t8, 'n6': oracle_n6, 't9': oracle_t9, 't9_all': oracle_t9_all})


# ------------------------------------------------------------- v1.2 oracles
def user_exceptions(trace, rng):
    return [e for e in u_events(trace, rng) if not e['async'] and e['cause'] != 8]


def in_user(va):
    """User code: A's or B's own text, (v1.3) the shared program pages or
    (v1.4) the window of archive programs."""
    return (in_range(va, A_RANGE) or in_range(va, B_RANGE) or in_range(va, PROG_RANGE)
            or in_range(va, ELF_RANGE))


def ext_from_user(trace):
    return [e for e in trace if e['async'] and e['cause'] == 9 and in_user(e['epc'])]


def oracle_t10(c, obs, ctx):
    st = steps(obs)
    e, b = st['read_ecall'], st['blocked']
    f = e['frame']
    c('read ecall from A: a7=63, fd=0, len=16', e['hit'] and f['scause'] == 8 and f['x17'] == 63
      and f['x10'] == 0 and f['x12'] == 16, (f['x17'], f['x10'], f['x12']))
    c('buffer first/last byte U+R+W in A root (PT walk)',
      all(e[w]['mapped'] and set('URW') <= set(e[w]['flags']) for w in ('walk_first', 'walk_last')))
    w, pa = b['wait'], b['proc_a']
    c('no input: A not dispatched for 30 timer ticks', w['reached'] and w['watch_hit'] is None, w)
    c('A is BLOCKED with one pending 16-byte read', pa['state'] == 6 and pa['read_pending'] == 1
      and pa['read_len'] == 16 and pa['read_va'] == f['x11'], STATE.get(pa['state']))
    c('A dispatch count unchanged while blocked', pa['dispatches'] == e['proc_a']['dispatches'])
    c('B kept running meanwhile (counter advances)', b['words_b'][0] > e['words_b'][0])
    c('nothing received before the harness typed', b['input']['received'] == 0)
    for k, sent, total in ((1, b'abc', 3), (2, b'XY', 5)):
        r = st[f'read_ret{k}']
        data = bytes.fromhex(r['data'])
        c(f'read {k}: returns {len(sent)} at ecall+4 in U-mode under A root', r['hit']
          and r['cpu']['priv'] == 0 and r['cpu']['pc'] == ctx['uva']['a_read_ret']
          and r['gprs']['x10'] == len(sent) and r['cpu']['satp'] == r['satps']['A'],
          hex(r['gprs']['x10']))
        c(f'read {k}: A buffer (physical) holds the typed bytes', data[total - len(sent):total] == sent,
          data[:total])
        c(f'read {k}: SUM=0 back in U-mode', not r['cpu']['sstatus'] & SUM, hex(r['cpu']['sstatus']))
        c(f'read {k}: kernel counted reads/bytes', r['proc_a']['reads'] == k
          and r['proc_a']['bytes_read'] == total)
    r2 = st['read_ret2']
    c('kernel: external interrupts serviced, 5 bytes, none dropped or left',
      r2['sched']['ext_irqs'] >= 1 and r2['input']['received'] == 5
      and r2['input']['dropped'] == 0 and r2['input']['count'] == 0, r2['input'])
    c('trace: S external interrupts (cause 9) taken from U-mode', ext_from_user(ctx['trace']),
      len(ext_from_user(ctx['trace'])))
    c('trace: no exception from user code other than ecall',
      not [e for e in ctx['trace'] if not e['async'] and e['cause'] != 8 and in_user(e['epc'])])


def oracle_t11(c, obs, ctx):
    st = steps(obs)
    r1, r2, a = st['ret1'], st['ret2'], st['after']
    u1, u2 = r1['gprs']['x10'], r2['gprs']['x10']
    tb, boot = r1['timebase'], r1['boot_time']
    quantum_us = r2['sched']['quantum_ticks'] * 1e6 / tb
    c('uptime returned in U-mode at both call sites', r1['hit'] and r2['hit']
      and r1['cpu']['priv'] == 0 == r2['cpu']['priv']
      and r1['cpu']['pc'] == ctx['uva']['a_uptime_ret1'] and r2['cpu']['pc'] == ctx['uva']['a_uptime_ret2'])
    pa = r2['proc_a']
    sleep_us = (pa['sleep_start'] - boot) * 1e6 / tb
    wake_us = (pa['last_dispatch_time'] - boot) * 1e6 / tb
    c('first value = kernel time since boot at the call (<= 1 quantum before sleep start)',
      0 < u1 and 0 <= sleep_us - u1 <= quantum_us + 100, f'{sleep_us - u1:.1f} us')
    c('second value = kernel time after the wake-up (<= 1 quantum after dispatch)',
      0 <= u2 - wake_us <= quantum_us + 100, f'{u2 - wake_us:.1f} us')
    c('grows by >= 100 ms and <= 100 ms + 2 quanta across sleep(100)',
      100000 <= u2 - u1 <= 100000 + 2 * quantum_us + 100, f'{(u2 - u1) / 1000:.2f} ms')
    c('A kept both values and continues without error', a['results'] == [u1, u2]
      and a['words_a'][5] == 1 and a['words_a'][2] == 0 and a['wait']['reached'])


SHELL_SCRIPT = ['help', 'ps', 'uptime', 'echo hello from the test', 'attack', 'ps', 'uptime']
PS_RX = re.compile(r'^pid (\d) ([AB]) (\w+) preempted (\d+) syscalls (\d+)$')


def oracle_t12(c, obs, ctx):
    st = steps(obs)
    uart = ctx['uart'].replace('\r', '')
    c('shell started: banner and prompt', 'ProtoOS shell' in uart and st['first_prompt']['ok'])
    for idle in ('idle_1', 'idle_2'):
        w, pa = st[idle]['wait'], st[idle]['proc_a']
        c(f'{idle}: at the prompt A is BLOCKED and not dispatched for 50 ticks',
          w['reached'] and w['watch_hit'] is None and pa['state'] == 6, (w, STATE.get(pa['state'])))
    ok = [st[f'cmd_{k}']['ok'] for k in range(1, len(SHELL_SCRIPT) + 1)]
    c('every command finished with a new prompt', all(ok) and uart.count('proto> ') == len(SHELL_SCRIPT) + 1,
      (ok, uart.count('proto> ')))
    blocks = [b.split('\n') for b in uart.split('proto> ')[1:]]
    c('each typed command was echoed', all(blocks[k][0] == cmd for k, cmd in enumerate(SHELL_SCRIPT)),
      [b[0] for b in blocks])
    out = {k: blocks[k][1:] for k in range(len(SHELL_SCRIPT))}
    c('help lists the commands', out[0][:1] == ['commands: help, ps, uptime, echo <text>, attack'], out[0][:1])
    ps = []
    for k in (1, 5):
        rows = [PS_RX.match(x) for x in out[k][:2]]
        ps.append(rows)
        c(f'ps #{len(ps)}: pid 1 A RUNNING, pid 2 B READY', all(rows)
          and rows[0].group(1, 2, 3) == ('1', 'A', 'RUNNING')
          and rows[1].group(1, 2, 3) == ('2', 'B', 'READY'), out[k][:2])
    if all(ps[0]) and all(ps[1]):
        c('ps: B was preempted again between the two ps (B keeps running)',
          int(ps[1][1][4]) > int(ps[0][1][4]), (ps[0][1][4], ps[1][1][4]))
        c('ps: the shell made more system calls', int(ps[1][0][5]) > int(ps[0][0][5]))
    ups = [re.match(r'^uptime: (\d+)\.(\d\d) s$', (out[k] or [''])[0]) for k in (2, 6)]
    c('uptime printed twice and grows', all(ups) and float('.'.join(ups[1].groups()))
      > float('.'.join(ups[0].groups())), [out[2][:1], out[6][:1]])
    c('echo prints its argument', out[3][:1] == ['hello from the test'], out[3][:1])
    c('attack: the kernel refuses with -14', out[4][:2] == [
        "attack: asking the kernel to print 8 bytes of B's memory...",
        "kernel answered -14: access denied, B's memory is not mapped for A"], out[4][:2])
    last = st['idle_2']
    c('no process faulted: A BLOCKED, B READY/RUNNING', last['proc_a']['state'] == 6
      and last['proc_b']['state'] in (1, 2) and 'FAULT' not in uart)
    typed = sum(len(x) + 1 for x in SHELL_SCRIPT)
    c(f'kernel received exactly the {typed} typed bytes, none dropped',
      last['input']['received'] == typed and last['input']['dropped'] == 0, last['input'])
    c('trace: S external interrupts (cause 9) taken from U-mode', ext_from_user(ctx['trace']))
    c('trace: no exception from user code other than ecall',
      not [e for e in ctx['trace'] if not e['async'] and e['cause'] != 8 and in_user(e['epc'])])


N7_EXPECT = [-9, -14, -14, -14, -14, -22, -14, -14, 0]
N7_NAMES = ['wrong fd', 'kernel page', "B's page", "A's own read-only text page",
            'crosses into guard page', 'len > 256', 'NULL', 'address wrap-around', 'len 0']


def oracle_n7(c, obs, ctx):
    st = steps(obs)
    r, b, d, a = st['rejected'], st['blocked'], st['done'], st['after']
    c('A passed all rejected calls in U-mode', r['hit'] and r['cpu']['priv'] == 0)
    for k, (name, want) in enumerate(zip(N7_NAMES, N7_EXPECT)):
        c(f'{name}: returns {want}', r['results'][k] == want & M64, hex(r['results'][k]))
    c('PT walk: kernel page U=0, text page not W, B page and guard unmapped in A root',
      r['walk_secret']['mapped'] and 'U' not in r['walk_secret']['flags']
      and r['walk_text']['mapped'] and 'W' not in r['walk_text']['flags']
      and not r['walk_b_data']['mapped'] and not r['walk_guard']['mapped'])
    c('rejected calls neither blocked nor consumed input', r['proc_a']['reads'] == 0
      and r['proc_a']['read_pending'] == 0 and r['proc_a']['state'] in (1, 2)
      and r['input']['received'] == 0)
    c('the valid read blocks (BLOCKED, not dispatched)', b['hit'] and b['wait']['watch_hit'] is None
      and b['proc_a']['state'] == 6)
    c('then returns 1 with the typed byte', d['hit'] and d['result9'] == 1
      and bytes.fromhex(d['data'])[:1] == b'z', (d['result9'], d['data']))
    c('A not faulted, keeps running; B keeps running', a['procs'][0]['state'] in (1, 2)
      and a['words_a'][0] > a['words_a_first'][0] and a['words_b'][0] > a['words_b_first'][0])
    c('trace: no exception from A other than ecall', not user_exceptions(ctx['trace'], A_RANGE))


FLOOD = bytes(65 + i % 26 for i in range(300))


def oracle_n8(c, obs, ctx):
    st = steps(obs)
    dr, b, d, a = st['drained'], st['blocked'], st['done'], st['after']
    c('all 300 flooded bytes were taken from the UART by interrupts', dr['hit']
      and dr['input']['received'] == 300, dr['input'])
    c('buffer kept 128 (its capacity), dropped the other 172', dr['input']['dropped'] == 172)
    c('first read returned 128', dr['result0'] == 128, dr['result0'])
    c('A got exactly the first 128 bytes, in order', bytes.fromhex(dr['data'])[:128] == FLOOD[:128])
    c('buffer empty after the read', dr['input']['count'] == 0)
    c('next read blocks (BLOCKED, not dispatched)', b['hit'] and b['wait']['watch_hit'] is None
      and b['proc_a']['state'] == 6)
    c('a byte typed after the flood is delivered (returns 1, "Z")', d['hit'] and d['result1'] == 1
      and bytes.fromhex(d['data'])[128:129] == b'Z' and d['input']['dropped'] == 172)
    c('no process faulted; B keeps running', all(p['state'] in (1, 2, 6) for p in a['procs'])
      and a['words_b'][0] > a['words_b_first'][0])
    c('trace: no exception from user code other than ecall',
      not [e for e in ctx['trace'] if not e['async'] and e['cause'] != 8 and in_user(e['epc'])])


ORACLES.update({'t10': oracle_t10, 't11': oracle_t11, 't12': oracle_t12, 'n7': oracle_n7,
                'n8': oracle_n8})


# ------------------------------------------------------------- v1.3 oracles
PROG = {'worker': 1, 'hello': 2, 'counter': 3, 'crash': 4, 'memcheck': 5}
SLOT2_BASE = 0xC00000
VM_PROC_PAGES = 5
NEG = lambda v: v & M64           # noqa: E731 (two's complement as the kernel returns it)


def pages_ok(c, label, p, mem, ctx):
    pages = p['pages']
    end = mem['base'] + mem['pages'] * 4096
    c(f'{label}: {VM_PROC_PAGES} distinct page-aligned pages inside the pool, outside the kernel image',
      p['npages'] == VM_PROC_PAGES and len(set(pages)) == VM_PROC_PAGES
      and all(pa % 4096 == 0 and mem['base'] <= pa < end for pa in pages)
      and all(not ctx['sym']['__kernel_start'] <= pa < ctx['sym']['__image_end'] for pa in pages)
      and mem['base'] >= ctx['sym']['__image_end'], [hex(x) for x in pages])


def oracle_t13(c, obs, ctx):
    st = steps(obs)
    a1, a2, d = st['after1'], st['after2'], st['done']
    r = d['results']
    base = r[0]
    for tag, s in (('instance 1', a1), ('instance 2', a2)):
        p = s['proc']
        c(f'{tag}: memcheck running in slot C with its own pid', s['hit'] and p['prog'] == PROG['memcheck']
          and p['state'] in (1, 2, 5) and p['pid'] == s['results'][1 if tag.endswith('1') else 3])
        pages_ok(c, tag, p, s['mem'], ctx)
        c(f'{tag}: allocator counts 5 pages in use (free = baseline - 5), bitmap bits set',
          s['mem']['free'] == base - VM_PROC_PAGES and s['bits'] == [1] * VM_PROC_PAGES,
          (s['mem']['free'], base, s['bits']))
        c(f'{tag}: its data and stack pages arrived zeroed (memcheck result 1)',
          s['data']['w2'] == 1 and s['data']['w3'] == 1, hex(s['data']['w2']))
        c(f'{tag}: it then dirtied both pages (0xa5 in physical memory)',
          s['data']['a5'] == 512 - 8 and s['stack']['a5'] == 512, (s['data']['a5'], s['stack']['a5']))
    p1, p2 = a1['proc']['pages'], a2['proc']['pages']
    c('instance 2 got the physical data and stack pages instance 1 had dirtied',
      p1[3] == p2[3] and p1[4] == p2[4], ([hex(x) for x in p1[3:]], [hex(x) for x in p2[3:]]))
    c('kills returned 0', r[2] == 0 and r[4] == 0, r)
    m = d['mem']
    c('after both kills every page is back: free = baseline, 10 allocations = 10 frees',
      r[5] == base == m['free'] and m['allocs'] == m['frees'] == 2 * VM_PROC_PAGES, (r[5], base, m))
    c('ended process owns no pages', d['proc']['npages'] == 0 and d['proc']['state'] == 3)
    c('trace: memcheck ran in U-mode from the program pages (sleep ecalls there)',
      any(not e['async'] and e['cause'] == 8 and in_range(e['epc'], PROG_RANGE) for e in ctx['trace']))


def oracle_t14(c, obs, ctx):
    st = steps(obs)
    s, run, a = st['spawned'], st['C_running'], st['after']
    p, r = s['proc'], s['results']
    c('spawn returned pid 3; free pages dropped by exactly 5', s['hit'] and r[1] == 3 and p['pid'] == 3
      and r[2] == r[0] - VM_PROC_PAGES, r)
    c('slot C: worker program, READY/RUNNING', p['prog'] == PROG['worker'] and p['state'] in (1, 2))
    pages_ok(c, 'slot C', p, s['mem'], ctx)
    sat = p['satp']
    c('own Sv39 root, distinct from kernel, A and B', sat >> 60 == 8 and sat not in s['satps'].values()
      and (sat & ((1 << 44) - 1)) << 12 == p['pages'][0], hex(sat))
    c('runs in U-mode under its own root, in the program pages', run['hit'] and run['cpu']['priv'] == 0
      and run['cpu']['satp'] == sat and in_range(run['cpu']['pc'], PROG_RANGE), hex(run['cpu']['pc']))
    c('sp in its own stack page, s11 = its data page',
      SLOT2_BASE + 0x3000 <= run['gprs']['x2'] <= SLOT2_BASE + 0x4000
      and run['gprs']['x27'] == SLOT2_BASE + 0x1000, (hex(run['gprs']['x2']), hex(run['gprs']['x27'])))
    wd, ws = s['walk_data'], s['walk_stack']
    c('PT walk: data and stack U+R+W, backed by its own pages', wd['mapped'] and ws['mapped']
      and set('URW') <= set(wd['flags']) and set('URW') <= set(ws['flags'])
      and wd['pa'] == p['pages'][3] + 0 and ws['pa'] == p['pages'][4], (wd.get('flags'), ws.get('flags')))
    c('PT walk: program page U+R+X, not writable; no private text page',
      s['walk_prog']['mapped'] and set('URX') <= set(s['walk_prog']['flags'])
      and 'W' not in s['walk_prog']['flags'] and not s['walk_text']['mapped'])
    c("PT walk: A's and B's data unmapped; kernel data and UART not user-accessible",
      not s['walk_a_data']['mapped'] and not s['walk_b_data']['mapped']
      and 'U' not in s['walk_secret']['flags'] and 'U' not in s['walk_uart']['flags'])
    alias = s['walk_pool_alias']
    c('its pages are reachable only through the kernel map (2 MiB leaf, U=0) at their PA',
      alias['mapped'] and alias['leaf_level'] == 1 and 'U' not in alias['flags'], alias.get('flags'))
    c("its data page is not mapped in A's or B's root", not s['a_sees_c']['mapped']
      and not s['b_sees_c']['mapped'])
    c('preempted by the timer and keeps counting', a['wait']['reached'] and a['proc']['preemptions'] > 0
      and a['counter'] > run['counter'], (a['proc']['preemptions'], run['counter'], a['counter']))
    c('B keeps running', a['words_b'][0] > run['words_b'][0])
    c('trace: timer interrupts taken from the program pages',
      any(e['async'] and e['cause'] == 5 and in_range(e['epc'], PROG_RANGE) for e in ctx['trace']))


def oracle_t15(c, obs, ctx):
    st = steps(obs)
    k, q, rs, d = st['killed'], st['quiet'], st['respawned'], st['done']
    r = d['results']
    p = k['proc']
    c('kill returned 0; the process is EXITED with code 137 and marked killed', k['hit'] and r[2] == 0
      and p['state'] == 3 and p['exit_code'] == 137 and p['killed'] == 1)
    c('all 5 of its pages are back (bits clear, free = baseline)', p['npages'] == 0
      and k['bits'] == [0] * VM_PROC_PAGES and r[3] == r[0] == k['mem']['free'], (k['bits'], r[:4]))
    c('never dispatched again (15 timer ticks)', q['wait']['reached'] and q['wait']['watch_hit'] is None,
      q['wait'])
    n = rs['proc']
    c('respawn reused slot C with a new pid and the same physical pages', rs['hit'] and n['pid'] == r[4]
      and n['pid'] != p['pid'] and sorted(n['pages']) == sorted(p['pages']) and rs['bits'] == [1] * 5,
      (n['pid'], [hex(x) for x in n['pages']]))
    c('second kill returned 0', r[5] == 0, r[5])
    h = d['proc']
    c('hello ran, printed and exited by itself (code 0, not killed), pages reclaimed',
      d['hit'] and h['pid'] == r[6] and h['state'] == 3 and h['exit_code'] == 0 and h['killed'] == 0
      and h['npages'] == 0 and f'hello from pid {r[6]}' in ctx['uart'], (h['pid'], h['state']))
    m = d['mem']
    c('at the end free = baseline and allocations = frees (15)', r[7] == r[0] == m['free']
      and m['allocs'] == m['frees'] == 3 * VM_PROC_PAGES, (r, m))
    c('trace: no exception from user code',
      not [e for e in ctx['trace'] if not e['async'] and e['cause'] != 8 and in_user(e['epc'])])


def oracle_t16(c, obs, ctx):
    st = steps(obs)
    f, d, a = st['fault'], st['done'], st['after']
    r = d['results']
    fr = f['frame']
    c('the crash program faulted: store page fault at its store, target = B data VA',
      f['hit'] and fr['scause'] == 15 and fr['sepc'] == ctx['uva']['p_crash_store']
      and fr['stval'] == 0x801000 and f['proc']['prog'] == PROG['crash'], (hex(fr['scause']), hex(fr['stval'])))
    c("B's data page was not written (no 0x43 pattern)", all(w != 0x4343434343434343 for w in f['b_data']))
    nd = st['next_dispatch']
    ready = [(f['slot'] + k) % 8 for k in range(1, 9) if nd['states'][(f['slot'] + k) % 8] == 1]
    c('v1.3.1: after the fault Round Robin continues with the next READY slot after the crash',
      nd['hit'] and ready and nd['slot'] == ready[0], (f['slot'], nd['slot'], nd['states']))
    crash = [p for p in d['procs'] if p['prog'] == PROG['crash']]
    c('crash slot FAULTED and its pages reclaimed', len(crash) == 1 and crash[0]['state'] == 4
      and crash[0]['npages'] == 0)
    workers = [(i, p) for i, p in enumerate(a['procs']) if p['prog'] == PROG['worker']]
    c('three workers alive with their own pages', len(workers) == 3
      and all(p['state'] in (1, 2) and p['npages'] == 5 for _, p in workers))
    c('every worker keeps counting after the crash', all(a['counters'][i] > d['counters'][i]
                                                          for i, _ in workers),
      [(d['counters'][i], a['counters'][i]) for i, _ in workers])
    c('B keeps running; A alive', a['words_b'][0] > d['words_b'][0] and a['procs'][0]['state'] in (1, 2, 5))
    c('no other fault within 30 ticks', a['wait']['reached'] and a['wait']['watch_hit'] is None)
    c('free pages = baseline - 15 (three workers)', r[5] == r[0] - 3 * VM_PROC_PAGES == a['mem']['free'], r)
    faults = [e for e in ctx['trace'] if not e['async'] and e['cause'] != 8 and in_user(e['epc'])]
    c('trace: exactly one user exception, the crash store', len(faults) == 1
      and faults[0]['epc'] == ctx['uva']['p_crash_store'] and faults[0]['cause'] == 15, faults[:3])
    c('UART: crash announcement and FAULT line', 'writes into pid 2' in ctx['uart']
      and 'stval=0x0000000000801000 -> FAULTED' in ctx['uart'])


def oracle_n9(c, obs, ctx):
    d = steps(obs)['done']
    r = d['results']
    want = [(1, 'spawn of program 0', NEG(-22)), (2, 'spawn of unknown program 99', NEG(-22)),
            (3, 'kill of a pid that does not exist', NEG(-3)), (4, 'kill of itself', NEG(-1)),
            (5, 'kill of boot process B', NEG(-1)), (6, 'spawn with every slot taken', NEG(-11)),
            (7, 'spawns that succeeded before (6 free slots)', 6),
            (8, 'kill of an already killed pid', NEG(-3)), (9, 'spawn/kill cycles done', 200),
            (10, 'failed cycles', 0)]
    c('A reached the end in U-mode', d['hit'])
    for k, name, v in want:
        c(f'{name}: {v if v < 1 << 63 else v - (1 << 64)}', r[k] == v, hex(r[k]))
    m, s = d['mem'], d['sched']
    c('no page leaked: free = baseline, allocations = frees', r[11] == r[0] == m['free']
      and m['allocs'] == m['frees'] == (6 + 200) * VM_PROC_PAGES, (r[0], r[11], m))
    c('kernel counters: 206 spawned, 206 killed and reclaimed', s['spawned'] == s['killed']
      == s['reclaimed'] == 206, s)
    c('no run-time process alive, boot processes intact', all(p['state'] not in (1, 2, 5, 6)
      for p in d['procs'][2:]) and d['procs'][1]['state'] in (1, 2))


T17_SCRIPT = ['mem', 'run worker', 'run hello', 'ps', 'kill 3', 'run crash', 'ps', 'mem']
PS13_RX = re.compile(r'^pid (\d+) ([A-H]) (\w+) preempted (\d+) syscalls (\d+)(?: (\w+))?$')


def oracle_t17(c, obs, ctx):
    st = steps(obs)
    uart = ctx['uart'].replace('\r', '')
    lines = uart.split('\n')
    end = st['end']
    c('every command finished with a new prompt', all(st[f'cmd_{k}']['ok'] for k in range(1, 9))
      and uart.count('proto> ') == len(T17_SCRIPT) + 1)
    mems = [tuple(map(int, m.groups())) for m in re.finditer(r'free pages: (\d+) of (\d+)', uart)]
    c('mem printed twice; all pages free at start and again at the end', len(mems) == 2
      and mems[0][0] == mems[0][1] == mems[1][0] == end['mem']['pages'], mems)
    c('run worker -> started pid 3; run hello -> started pid 4; run crash -> started pid 5',
      all(f'started pid {n}' in uart for n in (3, 4, 5)))
    c('hello printed its line and exited', 'hello from pid 4' in uart
      and re.search(r'EXIT [A-H]\(pid=4\) code=0', uart))
    pss, cur = [], None
    for ln in lines:
        if ln.endswith('proto> ps') or ln == 'ps':
            cur = []
            pss.append(cur)
        elif cur is not None:
            m = PS13_RX.match(ln)
            if m:
                cur.append(m.groups())
            elif ln.startswith('proto> '):
                cur = None
    c('ps printed twice', len(pss) == 2, len(pss))
    if len(pss) == 2:
        one = {int(g[0]): g for g in pss[0]}
        two = {int(g[0]): g for g in pss[1]}
        c('ps #1: shell RUNNING, B READY, pid 3 worker alive', one.get(1, ())[2:3] == ('RUNNING',)
          and one.get(2, ())[2:3] == ('READY',) and one.get(3, ('',) * 6)[5] == 'worker'
          and one[3][2] in ('READY', 'RUNNING'), pss[0])
        c('ps #2: pid 5 crash FAULTED, pid 3 gone or EXITED', two.get(5, ('',) * 6)[5] == 'crash'
          and two[5][2] == 'FAULTED' and (3 not in two or two[3][2] == 'EXITED'), pss[1])
    c('kill 3 returned the 5 pages', 'killed pid 3, 5 pages returned' in uart)
    c('crash announced itself and the kernel stopped only it',
      'crash: pid 5 writes into pid 2' in uart and re.search(r'FAULT [A-H]\(pid=5\) scause=0x0+f ', uart)
      and len(re.findall(r'FAULT [A-H]\(pid=', uart)) == 1)
    procs = end['procs']
    c('at the end: shell BLOCKED at its prompt, B READY/RUNNING, no run-time process owns pages',
      procs[0]['state'] == 6 and procs[1]['state'] in (1, 2)
      and all(p['npages'] == 0 for p in procs[2:]) and end['mem']['free'] == end['mem']['pages'])
    typed = sum(len(x) + 1 for x in T17_SCRIPT)
    c(f'kernel received exactly the {typed} typed bytes', end['input']['received'] == typed
      and end['input']['dropped'] == 0)
    faults = [e for e in ctx['trace'] if not e['async'] and e['cause'] != 8 and in_user(e['epc'])]
    c('trace: the only user exception is the crash store', len(faults) == 1
      and faults[0]['epc'] == ctx['uva']['p_crash_store'], faults[:3])


ORACLES.update({'t13': oracle_t13, 't14': oracle_t14, 't15': oracle_t15, 't16': oracle_t16,
                'n9': oracle_n9, 't17': oracle_t17})


# ------------------------------------------------------------- v1.4 oracles
def load_v14(images, work, out, tests):
    """v1.4: copy the program archives the selected tests boot with (SHA-256
    recorded), read images.json, and read the symbols and program headers of
    every program ELF here, independently of the kernel's own checker."""
    need = sorted({e[2] for t in tests for e in MATRIX[t] if len(e) > 2} |
                  {e[3]['disk'] for t in tests for e in MATRIX[t] if len(e) > 3 and 'disk' in e[3]})
    res = {'images_sha256': {}, 'work_images': work / 'images', 'manifest': {}, 'usym': {},
           'uelf': {}}
    if not need:
        return res
    for rel in need + ['images.json']:
        # v1.5: disk images (1 MiB each, rebuilt byte-identically from the
        # source) are recorded by SHA-256 only, not copied into the evidence
        is_disk = rel == 'disk.img' or rel.startswith('disks/')
        for dst in (work / 'images' / rel,) + (() if is_disk else (out / 'images' / rel,)):
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(images / rel, dst)
        res['images_sha256'][rel] = sha256(images / rel)
    res['manifest'] = json.loads((images / 'images.json').read_text())
    for elf in sorted((images / 'user').glob('*.elf')):
        nm = subprocess.run([NM, str(elf)], capture_output=True, text=True, check=True).stdout
        res['usym'][elf.stem] = {x[2]: int(x[0], 16) for x in map(str.split, nm.splitlines())
                                 if len(x) == 3}
        res['uelf'][elf.stem] = elf.read_bytes()
    return res


def elf_loads(data):
    """PT_LOAD headers and entry of an ELF64 file, read with struct."""
    phoff, = struct.unpack_from('<Q', data, 32)
    phnum, = struct.unpack_from('<H', data, 56)
    segs = []
    for i in range(phnum):
        t, f, off, va, _pa, fs, ms, _al = struct.unpack_from('<IIQQQQQQ', data, phoff + 56 * i)
        if t == 1:
            segs.append({'flags': f, 'offset': off, 'vaddr': va, 'filesz': fs, 'memsz': ms})
    return struct.unpack_from('<Q', data, 24)[0], segs


def expected_pages(data):
    """VA -> (exact 4 KiB content, p_flags) of every page the file loads:
    file bytes, then zeros (.bss and the tail of the last page)."""
    pages = {}
    for s in elf_loads(data)[1]:
        for off in range(0, s['memsz'], 4096):
            chunk = data[s['offset'] + off:s['offset'] + min(off + 4096, s['filesz'])] \
                if off < s['filesz'] else b''
            pages[s['vaddr'] + off] = ((chunk + bytes(4096))[:4096], s['flags'])
    return pages


def image_end(data):
    return max(s['vaddr'] + -(-s['memsz'] // 4096) * 4096 for s in elf_loads(data)[1])


def flags_of(w):
    return set(w.get('flags', ''))


PTE_FOR = {5: set('VRXUA'), 4: set('VRUA'), 6: set('VRWUAD')}   # p_flags -> exact leaf bits
STACK_VAS = range(0x401fc000, 0x40200000, 0x1000)
PAGE_LIMIT = 64


def proc_pages_ok(c, label, p, mem, ctx, archive=None):
    end = mem['base'] + mem['pages'] * 4096
    arc = (archive['start'] & ~0xfff, (archive['end'] + 0xfff) & ~0xfff) if archive else (0, 0)
    c(f'{label}: {p["npages"]} distinct page-aligned pool pages, outside the kernel image and the archive',
      len(p['pages']) == p['npages'] == len(set(p['pages']))
      and all(pa % 4096 == 0 and mem['base'] <= pa < end and not in_range(pa, arc)
              and not ctx['sym']['__kernel_start'] <= pa < ctx['sym']['__image_end']
              for pa in p['pages']), [hex(x) for x in p['pages'][:4]])


def user_faults(trace):
    return [e for e in trace if not e['async'] and e['cause'] != 8 and in_user(e['epc'])]


def oracle_t18(c, obs, ctx):
    st = steps(obs)
    cp, d = st['checkpoint'], st['done']
    r, p, mem, a = d['results'], cp['proc'], cp['mem'], cp['archive']
    data = ctx['uelf']['t_elf']
    pages = expected_pages(data)
    want_pages = 3 + len(pages) + 4
    c('t_elf stopped at its checkpoint in U-mode, under its own root',
      cp['hit'] and cp['cpu']['priv'] == 0 and cp['cpu']['satp'] == p['satp']
      and cp['cpu']['pc'] == ctx['usym']['t_elf']['t_elf_checkpoint'], hex(cp['cpu']['pc']))
    c('run() returned pid 3 = its getpid() (a0 at the checkpoint); program id 16 = archive file 0',
      r[1] == 3 == p['pid'] == cp['gprs']['x10'] and p['prog'] == 16, (r[1], p['pid'], p['prog']))
    c(f'it owns {want_pages} pages: 3 tables + {len(pages)} segment pages (ELF headers) + 4 stack',
      p['npages'] == want_pages, p['npages'])
    proc_pages_ok(c, 't_elf', p, mem, ctx, a)
    c('allocator: its pages are marked in use, free = baseline - its pages',
      cp['bits'] == [1] * want_pages and mem['free'] == r[0] - want_pages, (mem['free'], r[0]))
    root = (p['satp'] & ((1 << 44) - 1)) << 12
    c('own Sv39 root (its first page), not the kernel, A or B root', p['satp'] >> 60 == 8
      and root == p['pages'][0] and p['satp'] not in cp['satps'].values())
    t = cp['tables']
    pte_pa = lambda e: (e >> 10) << 12  # noqa: E731
    l1, l0 = t.get('l1', []), t.get('l0', [])
    c('window tables: root -> its own level-1 table -> its own level-0 table (non-leaf, '
      'no U); the level-1 table holds nothing else', t['root_entry'] & 0xff == 1
      and pte_pa(t['root_entry']) == p['pages'][1] and len(l1) == 512 and l1[0] & 0xff == 1
      and pte_pa(l1[0]) == p['pages'][2] and not any(l1[1:]) and len(l0) == 512,
      hex(t['root_entry']))
    want = {va: PTE_FOR[fl] for va, (_, fl) in pages.items()}
    want.update({va: PTE_FOR[6] for va in STACK_VAS})
    got = {ELF_RANGE[0] + 4096 * i: e for i, e in enumerate(l0) if e}
    bits = lambda e: {b for i, b in enumerate('VRWXUGAD') if e >> i & 1}  # noqa: E731
    wrong = [(hex(va), hex(e)) for va, e in sorted(got.items())
             if va not in want or bits(e) != want[va] or e & ~((1 << 54) - 1) or e & 0x300]
    c('all 512 entries of the window: exactly the segment pages (with exactly their ELF '
      'permissions: code R X, constants R, data R W, never W+X) and the 4 stack pages R W U; '
      'nothing else (heap empty, guard 0x401fb000 unmapped)',
      set(got) == set(want) and not wrong, (wrong[:3], sorted(set(want) ^ set(got))[:3]))
    leaves = [pte_pa(e) for e in got.values()]
    c('one to one: the window maps each of its own non-table pages exactly once, and never '
      'one of its page tables', sorted(leaves) == sorted(p['pages'][3:])
      and len(set(leaves)) == len(leaves), [hex(x) for x in leaves[:4]])
    bad = [hex(va) for va, (content, _) in sorted(pages.items())
           if cp['contents'].get(hex(va)) != hashlib.sha256(content).hexdigest()]
    c('every segment page holds exactly the file bytes (then zeros): SHA-256 of the '
      'physical page = the page computed from the ELF file', not bad, bad[:3])
    c("PT walk: A's and B's data unmapped; kernel data, UART, archive and pool kernel-only",
      not cp['walk_a_data']['mapped'] and not cp['walk_b_data']['mapped']
      and 'U' not in flags_of(cp['walk_secret']) and 'U' not in flags_of(cp['walk_uart'])
      and cp['walk_archive']['mapped'] and 'U' not in flags_of(cp['walk_archive'])
      and cp['walk_pool_alias']['leaf_level'] == 1 and 'U' not in flags_of(cp['walk_pool_alias']))
    c('PT walk: shared program pages R X U, not W', set('URX') <= flags_of(cp['walk_prog'])
      and 'W' not in flags_of(cp['walk_prog']))
    c("no part of the window's 1 GiB is mapped in A's, B's or the kernel's root (root entry 0)",
      cp['other_roots'] == {'kernel': 0, 'A': 0, 'B': 0}, cp['other_roots'])
    n_arc = len(cp['archive_bits'])
    c(f'archive: {n_arc} pages reserved (bitmap), {ctx["manifest"]["tests.img"].__len__()} programs',
      cp['archive_bits'] == [1] * n_arc and mem['reserved'] == n_arc and a['found'] == 3
      and a['pa'] == a['start'] and a['count'] == len(ctx['manifest']['tests.img'])
      and a['end'] - a['start'] == ctx['image_size'], (mem['reserved'], a))
    c('t_elf passed all its own checks: wait() returned 0', r[2] == 0, r[2])
    c('all its pages came back (free = baseline)', r[3] == r[0], (r[0], r[3]))
    c('t_exit42 started as pid 4, wait() returned 42, pages back', r[4] == 4 and r[5] == 42
      and r[6] == r[0], r[4:7])
    c('sbrk from A (no heap): -22', r[7] == NEG(-22), hex(r[7]))
    c('progs(0) = size and name of t_elf; progs(99) = -2; foreign buffer = -14',
      r[8] == len(data) and bytes.fromhex(d['progs_name']).rstrip(b'\0') == b't_elf'
      and r[9] == NEG(-2) and r[10] == NEG(-14), (r[8], d['progs_name'], hex(r[9]), hex(r[10])))
    m = d['mem']
    c('allocations = frees at the end; slot owns nothing', m['allocs'] == m['frees']
      and d['proc']['npages'] == 0 and m['free'] == r[0])
    c('UART: t_elf reported success, exit code 0', 't_elf: all checks passed' in ctx['uart']
      and re.search(r'EXIT C\(pid=3\) code=0', ctx['uart']))
    c('trace: no user exception other than ecall', not user_faults(ctx['trace']))
    c('trace: ecalls from the ELF window (the C programs ran in U-mode)',
      any(not e['async'] and e['cause'] == 8 and in_range(e['epc'], ELF_RANGE) for e in ctx['trace']))


def oracle_t19(c, obs, ctx):
    st = steps(obs)
    c1, c2, d = st['cp1'], st['cp2'], st['done']
    r = d['results']
    data = ctx['uelf']['t_sbrk']
    base = image_end(data)
    n0 = 3 + len(expected_pages(data)) + 4
    p1 = c1['proc']
    c('checkpoint 1: heap starts at the page after the image (= _end); break = base + 10000',
      c1['hit'] and p1['heap_base'] == base == ctx['usym']['t_sbrk']['_end']
      and p1['brk'] == base + 10000 == c1['gprs']['x10'], (hex(p1['heap_base']), hex(p1['brk'])))
    h = c1['heap']
    c('3 heap pages mapped R W U (not X), its own pages; the next two unmapped',
      all(h[hex(base + k * 4096)]['mapped'] and flags_of(h[hex(base + k * 4096)]) == PTE_FOR[6]
          and (h[hex(base + k * 4096)]['pa'] & ~0xfff) in p1['pages'] for k in range(3))
      and not h[hex(base + 3 * 4096)]['mapped'] and not h[hex(base + 4 * 4096)]['mapped'])
    hb = bytes.fromhex(c1['heap_bytes'])
    c('heap pages hold what it wrote (10000 bytes) and zeros after', hb[:10000] == bytes(
        (i * 7 + 1) & 0xff for i in range(10000)) and hb[10000:] == bytes(len(hb) - 10000))
    c(f'pages owned: {n0} + 3 heap; free = baseline - that', p1['npages'] == n0 + 3
      and c1['mem']['free'] == r[0] - n0 - 3, (p1['npages'], c1['mem']['free'], r[0]))
    p2 = c2['proc']
    top = (c2['brk'] + 0xfff) & ~0xfff
    c(f'checkpoint 2: heap grew until refused at exactly {PAGE_LIMIT} pages owned',
      c2['hit'] and p2['npages'] == PAGE_LIMIT and c2['steps'] == PAGE_LIMIT - n0 - 3
      and c2['brk'] == p2['brk'] == base + 10000 + c2['steps'] * 4096, (p2['npages'], c2['steps']))
    hw = c2['heap']
    c('every heap page up to the break mapped R W U, the page after it unmapped',
      all(hw[hex(va)]['mapped'] and flags_of(hw[hex(va)]) == PTE_FOR[6]
          and (hw[hex(va)]['pa'] & ~0xfff) in p2['pages'] for va in range(base, top, 4096))
      and not hw[hex(top)]['mapped'])
    c('guard page below the stack unmapped; stack mapped', not c2['guard']['mapped']
      and all(x['mapped'] for x in c2['stack'].values()))
    proc_pages_ok(c, 't_sbrk at the limit', p2, c2['mem'], ctx)
    c('allocator: all its pages marked, free = baseline - 64', c2['bits'] == [1] * PAGE_LIMIT
      and c2['mem']['free'] == r[0] - PAGE_LIMIT)
    c('t_sbrk passed all its own checks (zeroed pages, -22, -12, break unchanged on refusal)',
      r[2] == 0, r[2])
    m = d['mem']
    c('all 64 pages came back: free = baseline, allocations = frees', r[3] == r[0] == m['free']
      and m['allocs'] == m['frees'] and d['proc']['npages'] == 0, (r, m))
    c('UART: t_sbrk reported success', 't_sbrk: all checks passed' in ctx['uart'])
    c('trace: no user exception other than ecall', not user_faults(ctx['trace']))


def oracle_t20(c, obs, ctx):
    st = steps(obs)
    we, bl, rt, d = st['wait_ecall'], st['blocked'], st['returned'], st['done']
    r = d['results']
    want = [(2, 'wait for t_exit42', 42), (4, 'wait for b_text (faulted)', 139),
            (6, 'kill of t_spin', 0), (7, 'wait for the killed t_spin (already ended)', 137),
            (9, 'wait for t_exit42 that ended before', 42), (10, 'wait for a pid that never existed', NEG(-3)),
            (11, 'wait for itself', NEG(-1)), (12, 'wait with an unknown flag', NEG(-22)),
            (13, 'foreground wait for boot process B', NEG(-1)), (15, 'wait for t_sleepy', 7)]
    for k, name, v in want:
        c(f'{name}: {v if v < 1 << 63 else v - (1 << 64)}', r[k] == v, hex(r[k]))
    c('run() returned pids 3, 4, 5, 6, 7', [r[1], r[3], r[5], r[8], r[14]] == [3, 4, 5, 6, 7],
      [r[1], r[3], r[5], r[8], r[14]])
    f = we['frame']
    c('A called wait(pid of t_sleepy, 0)', we['hit'] and f['x10'] == r[14] and f['x11'] == 0)
    pa = bl['proc_a']
    c('A BLOCKED waiting for it (wait_pid), not dispatched for 10 timer ticks',
      bl['wait']['reached'] and bl['wait']['watch_hit'] is None and pa['state'] == 6
      and pa['wait_pid'] == r[14] and pa['read_pending'] == 0, (pa['state'], pa['wait_pid']))
    sleepy = [p for p in bl['procs'] if p['pid'] == r[14]]
    c('meanwhile t_sleepy is alive (SLEEPING)', len(sleepy) == 1 and sleepy[0]['state'] == 5)
    ra = rt['proc_a']
    c('A resumed after t_sleepy ended, with 7; one more completed wait',
      rt['hit'] and rt['results'][15] == 7 and ra['wait_pid'] == 0
      and ra['waits_done'] == we['proc_a']['waits_done'] + 1)
    c('UART: t_sleepy exited with 7', re.search(r'EXIT [A-H]\(pid=7\) code=7', ctx['uart']))
    m = d['mem']
    c('every page came back: free = baseline, allocations = frees', r[16] == r[0] == m['free']
      and m['allocs'] == m['frees'] and all(p['npages'] == 0 for p in d['procs'][2:]))
    faults = user_faults(ctx['trace'])
    c("trace: the only user exception is b_text's store into its code", len(faults) == 1
      and faults[0]['cause'] == 15 and faults[0]['tval'] == 0x40000000, faults[:3])


def oracle_t21(c, obs, ctx):
    st = steps(obs)
    e1, arm, a1, e2 = st['fg1_ecall'], st['fg1_armed'], st['after1'], st['fg2_ecall']
    ans, a2, pr, d = st['reader_answered'], st['after2'], st['plain_read'], st['done']
    r = d['results']
    bypid = lambda procs, pid: [p for p in procs if p['pid'] == pid and p['state']]  # noqa: E731
    c('A: wait(t_spin, WAIT_FOREGROUND)', e1['hit'] and e1['frame']['x10'] == r[2]
      and e1['frame']['x11'] == 1)
    s, i = arm['sched'], arm['input']
    c('foreground armed: fg_pid = that t_spin, fg_waiter = A, Ctrl-C armed in the input path',
      s['fg_pid'] == r[2] and s['fg_waiter'] == 1 and i['intr_armed'] == 1, (s['fg_pid'], i))
    c('A BLOCKED, not dispatched for 60 ticks', arm['wait']['reached']
      and arm['wait']['watch_hit'] is None and arm['procs'][0]['state'] == 6)
    busy = bypid(arm['procs'], r[3])
    c('meanwhile a second foreground wait (t_fgbusy) was refused (-16): it exited with 16',
      busy and busy[0]['state'] == 3 and busy[0]['exit_code'] == 16 and r[5] == 16, r[5])
    run_, it = st['fg_running'], st['intr']
    c('Ctrl-C typed while the foreground t_spin itself was executing: U-mode, its root installed',
      run_['hit'] and run_['cpu']['priv'] == 0 and run_['cpu']['satp'] == run_['fg_satp']
      and run_['cpu']['pc'] == ctx['usym']['t_spin']['t_spin_step'] and run_['state'] == 2)
    c('the kernel handled it on the interrupt from that process: at console_intr() the '
      'current process is the foreground one, RUNNING, its root installed, trapped by the '
      'external interrupt (cause 9) inside its code',
      it['hit'] and it['current'] == it['fg_frame'] and it['fg_state'] == 2
      and it['cpu']['satp'] == run_['fg_satp'] and it['fg_scause'] == (1 << 63) | 9
      and in_range(it['fg_sepc'], ELF_RANGE) and it['input']['intr_pending'] == 1,
      (hex(it['current']), hex(it['fg_frame']), it['fg_state'], hex(it['fg_scause'])))
    fg = bypid(a1['procs'], r[2])
    c('Ctrl-C: wait returned 130; t_spin EXITED by Ctrl-C (code 130, killed), pages back',
      a1['hit'] and a1['results'][4] == 130 and fg and fg[0]['state'] == 3
      and fg[0]['exit_code'] == 130 and fg[0]['killed'] == 1 and fg[0]['npages'] == 0)
    c('Ctrl-C was not input: consumed by the kernel, foreground disarmed',
      a1['input']['intrs'] == 1 and a1['input']['count'] == 0 and a1['input']['intr_armed'] == 0
      and a1['sched']['fg_pid'] == 0 and a1['sched']['interrupted'] == 1, a1['input'])
    bg = bypid(a1['procs'], r[1])
    c('the background t_spin was not touched', bg and bg[0]['state'] in (1, 2))
    rd = bypid(ans['procs'], r[6])
    c('second foreground program t_reader answered the typed line and is BLOCKED in read()',
      e2['hit'] and e2['frame']['x10'] == r[6] and ans['ok'] and rd and rd[0]['state'] == 6
      and rd[0]['read_pending'] == 1 and ans['wait']['watch_hit'] is None)
    rd2 = bypid(a2['procs'], r[6])
    c('Ctrl-C while it was blocked: wait returned 130, reader EXITED (130), read cancelled, pages back',
      a2['hit'] and a2['results'][7] == 130 and rd2 and rd2[0]['state'] == 3
      and rd2[0]['exit_code'] == 130 and rd2[0]['read_pending'] == 0 and rd2[0]['npages'] == 0
      and a2['sched']['interrupted'] == 2 and a2['input']['intrs'] == 2)
    c('without a foreground wait Ctrl-C is an ordinary byte: A read 1 byte = 0x03',
      pr['hit'] and pr['input']['intr_armed'] == 0 and r[8] == 1
      and bytes.fromhex(d['data'])[:1] == b'\x03' and d['input']['intrs'] == 2)
    c('background t_spin then killed (0), wait 137', r[9] == 0 and r[10] == 137)
    m, inp_ = d['mem'], d['input']
    c('kernel received exactly the 7 typed bytes, none dropped, none left', inp_['received'] == 7
      and inp_['dropped'] == 0 and inp_['count'] == 0, inp_)
    c('every page came back: free = baseline, allocations = frees', r[11] == r[0] == m['free']
      and m['allocs'] == m['frees'])
    c('UART: the reader echoed the line', 'got: abc' in ctx['uart'])
    c('trace: no user exception other than ecall', not user_faults(ctx['trace']))


T22_PROGRAMS = ['guess', 'primes', 'clock']


def oracle_t22(c, obs, ctx):
    st = steps(obs)
    uart = ctx['uart'].replace('\r', '')
    end = st['end']
    first = st['first_prompt']
    total = first['mem']['pages'] - first['mem']['reserved']
    # v1.5: programs.img also has files, cat, note and rm: the count comes
    # from images.json (3 in v1.4, 7 since v1.5); the check is otherwise the same
    n = len(ctx['manifest']['programs.img'])
    c(f'archive with the {n} programs loaded; its pages reserved',
      re.search(rf'ARCHIVE \d+ bytes at 0x[0-9a-f]+, \d+ pages reserved: {n} programs', uart)
      and first['archive']['count'] == n and first['archive_bits'] == [1] * len(first['archive_bits'])
      and first['mem']['reserved'] == len(first['archive_bits']))
    blocks = {}
    for b in uart.split('proto> ')[1:]:
        lines = b.split('\n')
        blocks.setdefault(lines[0], []).append(lines[1:])
    ls = blocks.get('ls', [[]])[0]
    sizes = [len(ctx['uelf'][n]) for n in T22_PROGRAMS]
    c('ls lists guess, primes, clock with their file sizes',
      ls[:3] == [f'{n}  {s} bytes' for n, s in zip(T22_PROGRAMS, sizes)], ls[:3])
    pr = blocks.get('run primes', [[]])[0]
    c('run primes: the program printed the right primes and count, exit 0', pr[:2] == [
        'Primes below 100: 2 3 5 7 11 13 17 19 23 29 31 37 41 43 47 53 59 61 67 71 73 79 83 89 97',
        'There are 1229 primes below 10000 (sieve of 10000 bytes on the heap).']
      and re.search(r'EXIT C\(pid=3\) code=0', uart), pr[:3])
    g = st['guess']
    m = re.search(r'Correct! The number was (\d+)\. Tries: (\d+)', uart)
    c('typed ahead with the command, "50" reached the game, not the shell (answered by the '
      'game; no "unknown command")', g['guesses'][:1] == [50] and len(g['answers']) >= 1
      and 'unknown command' not in uart and re.search(r'Your guess: 50\n(Too small\.|Too big\.|'
                                                      r'Correct!)', uart), g['guesses'][:2])
    c('run guess: the binary search found the number; the program counted the tries',
      g['ok'] and g['answers'] and g['answers'][-1] == 'Correct!' and m
      and int(m[1]) == g['guesses'][-1] and int(m[2]) == len(g['guesses']), (g['guesses'], g['answers']))
    cl = blocks.get('run clock', [[]])[0]
    ups = [float(x) for x in re.findall(r'^uptime (\d+\.\d\d) s$', '\n'.join(cl), re.M)]
    c('run clock: at least 3 lines one second apart, then ^C and a new prompt',
      len(ups) >= 3 and all(0.9 <= b - a <= 1.3 for a, b in zip(ups, ups[1:])) and '^C' in cl
      and st['clock_stopped']['ok'], (ups, cl[-2:]))
    clock = [p for p in st['clock_stopped']['procs'] if p['pid'] == 5]
    c('Ctrl-C ended clock (pid 5): EXITED 130, killed, pages back; the shell waited in the foreground',
      clock and clock[0]['state'] == 3 and clock[0]['exit_code'] == 130 and clock[0]['killed'] == 1
      and clock[0]['npages'] == 0 and st['clock_stopped']['sched']['interrupted'] == 1
      and st['clock_running']['sched']['fg_pid'] == 5 and st['clock_running']['sched']['fg_waiter'] == 1)
    c('run clock & -> started pid 6 (background, no wait)', 'started pid 6' in uart)
    after = uart.split('run clock &', 1)[-1]        # background output interleaves from here
    c('ps shows the background program by name', re.search(
        r'^pid 6 C (SLEEPING|READY|RUNNING) preempted \d+ syscalls \d+ clock$', after, re.M))
    clock_pages = 3 + len(expected_pages(ctx['uelf']['clock'])) + 4
    c(f'kill 6 returned its {clock_pages} pages', f'killed pid 6, {clock_pages} pages returned' in uart)
    c('run nosuch -> not found message', re.search(r'^run: no program nosuch \(try ls\)$', after, re.M))
    mems = [tuple(map(int, x)) for x in re.findall(r'free pages: (\d+) of (\d+)', uart)]
    c('mem at the end: all pages free', mems and mems[-1][0] == mems[-1][1] == total, mems)
    procs = end['procs']
    c('at the end: shell BLOCKED at its prompt, B running, no run-time process owns pages',
      procs[0]['state'] == 6 and procs[1]['state'] in (1, 2)
      and all(p['npages'] == 0 for p in procs[2:]) and end['mem']['free'] == total
      and end['wait']['watch_hit'] is None)
    c('every command ended with a new prompt (10 prompts)', uart.count('proto> ') == 10 and all(
        s_['ok'] for s_ in obs['steps'] if s_['name'].startswith('cmd ')), uart.count('proto> '))
    c('trace: no user exception other than ecall', not user_faults(ctx['trace']))


def oracle_n10(c, obs, ctx):
    d = steps(obs)['done']
    r, m, s = d['results'], d['mem'], d['sched']
    bad = list(ctx['manifest']['badelf.img'].items())
    c(f'A tried all {len(bad)} malformed files', d['hit'] and r[1] == len(bad), r[1])
    wrong = [(n, hex(r[2 + k])) for k, (n, _) in enumerate(bad) if r[2 + k] != NEG(-8)]
    c('run() refused every one with -8', not wrong, wrong[:4])
    reasons = dict(re.findall(r'RUN refused name=(\w+) reason=-(\d+)', ctx['uart']))
    wrong = [(n, reasons.get(n), -code) for n, code in bad if reasons.get(n) != str(-code)]
    c('each for the defect it was built with (kernel reason = images.json)', not wrong, wrong[:4])
    rf = steps(obs)['refused']
    c('checked before allocation: across the 30 refusals the allocator handed out and took '
      'back nothing (allocations, frees and free pages unchanged), nothing started',
      rf['hit'] and rf['mem']['allocs'] == rf['mem_before']['allocs']
      and rf['mem']['frees'] == rf['mem_before']['frees']
      and rf['mem']['free'] == rf['mem_before']['free'] and rf['sched']['loaded'] == 0
      and rf['sched']['load_errors'] == len(bad) and rf['results'][1] == len(bad),
      (rf['mem_before'], rf['mem']))
    c('in all: loads refused = 30, the valid file loaded once, allocations = frees',
      s['load_errors'] == len(bad) and s['loaded'] == 1 and m['allocs'] == m['frees']
      and s['spawned'] == 0, (s['load_errors'], s['loaded'], m))
    want = [(64, 'name not in the archive', NEG(-2)), (65, 'empty name', NEG(-22)),
            (66, 'name longer than 15', NEG(-22)), (67, "name in B's memory", NEG(-14)),
            (68, 'name in kernel memory', NEG(-14)), (69, 'the valid file still starts (pid 3)', 3),
            (70, '... and exits with 42', 42)]
    for k, name, v in want:
        c(f'{name}: {v if v < 1 << 63 else v - (1 << 64)}', r[k] == v, hex(r[k]))
    c('free = baseline at the end', r[71] == r[0] == m['free'], (r[0], r[71]))
    c('no run-time process alive', all(p['state'] in (0, 3, 4) for p in d['procs'][2:]))
    c('trace: no user exception other than ecall', not user_faults(ctx['trace']))


def oracle_n11(c, obs, ctx):
    st = steps(obs)
    d, a = st['done'], st['after']
    stem = pathlib.Path(ctx['image']).stem
    code = ctx['manifest']['badarc'][stem]
    r, m, arc = d['results'], d['mem'], d['archive']
    line = re.search(r'ARCHIVE (\d+) bytes at 0x([0-9a-f]+), (\d+) pages reserved: rejected code=-(\d+)',
                     ctx['uart'])
    pages = len(d['archive_bits'])
    c(f'{stem}: kernel rejected the archive with reason {code}', line
      and int(line[4]) == -code and int(line[1]) == ctx['image_size'], line and line.groups())
    c('no programs: progs(0) = -2, run("ok") = -2', d['hit'] and arc['count'] == 0
      and arc['pa'] == 0 and r[2] == NEG(-2) and r[3] == NEG(-2), (hex(r[2]), hex(r[3])))
    c('its pages stay reserved and out of the allocator', d['archive_bits'] == [1] * pages
      and m['reserved'] == pages == int(line[3] if line else -1) and r[1] == m['pages'] - pages
      and r[0] == m['free'])
    c("archive memory not user-accessible in A's root", 'U' not in flags_of(d['walk_a_archive']))
    c('the system keeps running: B counts on', a['wait']['reached']
      and a['words_b'][0] > d['words_b'][0])


BAD_USER = ['b_text', 'b_rodata', 'b_kernel', 'b_exec', 'b_stack', 'b_heap', 'b_fpu', 'b_csr']


def oracle_n12(c, obs, ctx):
    st = steps(obs)
    d, a = st['done'], st['after']
    r, m = d['results'], d['mem']
    u = ctx['usym']
    c('A ran all 8 programs', d['hit'] and r[1] == len(BAD_USER), r[1])
    c('each started (pids 3..10) and wait() reported 139 (faulted) for each',
      [r[2 + 2 * k] for k in range(8)] == list(range(3, 11))
      and [r[3 + 2 * k] for k in range(8)] == [139] * 8, r[2:18])
    faults = user_faults(ctx['trace'])
    text = lambda e: 0x40000000 <= e['epc'] < 0x40001000  # noqa: E731
    want = [
        ('store into its own code', lambda e: e['cause'] == 15 and e['tval'] == u['b_text']['_start']),
        ('store into its constants', lambda e: e['cause'] == 15
         and e['tval'] == u['b_rodata']['b_rodata_constant']),
        ('load from the kernel', lambda e: e['cause'] == 13 and e['tval'] == 0x80200000),
        ('jump into its data', lambda e: e['cause'] == 12 and e['epc'] == e['tval']
         == u['b_exec']['b_exec_code']),
        ('stack overflow into the guard page', lambda e: e['cause'] == 15
         and 0x401fb000 <= e['tval'] < 0x401fc000),
        ('store past its heap', lambda e: e['cause'] == 15 and e['tval'] == u['b_heap']['_end'] + 4096),
        ('floating-point instruction (FP off)', lambda e: e['cause'] == 2 and text(e)),
        ('write to satp', lambda e: e['cause'] == 2 and text(e)),
    ]
    c('trace: exactly 8 user exceptions', len(faults) == 8, len(faults))
    for k, (name, ok) in enumerate(want):
        e = faults[k] if k < len(faults) else None
        c(f'{BAD_USER[k]}: {name}', e is not None and ok(e) and (k == 3 or text(e)), e)
    c('UART: 8 FAULT lines, one per program', len(re.findall(r'^FAULT [A-H]\(pid=', ctx['uart'], re.M)) == 8)
    c('every page came back: free = baseline, allocations = frees', r[40] == r[0] == m['free']
      and m['allocs'] == m['frees'] and all(p['npages'] == 0 for p in d['procs'][2:]))
    c('A and B unaffected: B keeps counting', a['wait']['reached'] and a['words_b'][0] > d['words_b'][0]
      and a['procs'][0]['state'] in (1, 2, 5) and a['procs'][1]['state'] in (1, 2))


ORACLES.update({'t18': oracle_t18, 't19': oracle_t19, 't20': oracle_t20, 't21': oracle_t21,
                't22': oracle_t22, 'n10': oracle_n10, 'n11': oracle_n11, 'n12': oracle_n12})


# ------------------------------------------------------------------ v1.5
VIRTIO_F_VERSION_1, BLK_F_RO, BLK_F_FLUSH = 1 << 32, 1 << 5, 1 << 9


def pat(n, tag):
    """The test data pattern (user/include/ptest.h)."""
    return bytes((i * 131 + tag * 17 + 7) & 255 for i in range(n))


def expect_results(c, label, got, want):
    """want: {index: value}; values compared as the kernel's 64-bit words."""
    bad = {k: (got[k] - (1 << 64) if got[k] >> 63 else got[k]) for k, v in want.items()
           if got[k] != v & M64}
    c(f'{label}: every result as specified ({len(want)} values)', not bad, bad)
    c(f'{label}: the driver ran to the end (r[63] = 0x600d)', got[63] == 0x600d, hex(got[63]))


def queue_ok(c, label, v, kernel):
    """The virtqueue: every request completed, descriptors on kernel buffers only."""
    n = (v['reads'] + v['writes'] + v['flushes']) & 0xffff
    c(f'{label}: every request completed (avail = used = requests)',
      v['avail_idx'] == v['used_seen'] == v['vq_avail_idx'] == v['vq_used_idx'] == n,
      {k: v[k] for k in ('avail_idx', 'used_seen', 'vq_avail_idx', 'vq_used_idx', 'reads',
                         'writes', 'flushes')})
    bufs = set(v['buffers'][k] for k in ('req', 'bounce', 'status'))
    c(f'{label}: descriptors point only at the request header, the bounce buffer and the status byte',
      all(d['addr'] in bufs or (d['addr'] == 0 and d['len'] == 0) for d in v['desc'])
      and all(kernel[0] <= b < kernel[1] for b in bufs), v['desc'])


def pfsck_files(c, label, ctx, want, seq=None):
    """want: {name: bytes}: the disk image after the boot, read by pfsck."""
    p = ctx['disk']['pfsck']
    c(f'{label}: pfsck: the image mounts and every file is intact', p['mounts'] and p['data_ok'],
      {k: p.get(k) for k in ('reject', 'slot', 'seq', 'data_ok')})
    got = {n: hashlib.sha256(d).hexdigest() for n, d in ctx['disk']['data'].items()}
    exp = {n: hashlib.sha256(d).hexdigest() for n, d in want.items()}
    c(f'{label}: pfsck: exactly the expected files with the expected content', got == exp,
      {'got': sorted(got), 'want': sorted(exp)})
    if seq is not None:
        c(f'{label}: pfsck: directory seq {seq}', p['seq'] == seq, p['seq'])


def exit_data(c, obs):
    s = steps(obs)['exit']
    c('the test driver exited and its results were read through its own page table', s['hit'],
      s.get('autorun_pid'))
    return s


T24_SIZES = [0, 1, 4095, 4096, 4097, 131072]


def oracle_t_fs(c, obs, ctx):
    s = exit_data(c, obs)
    want = {k: 0 for k in range(6)}
    want.update({6 + k: n for k, n in enumerate(T24_SIZES)})
    want.update({12 + k: 1 for k in range(6)})
    want.update({18: -27, 19: 97, 20: 1, 21: 0, 22: 100, 23: 1, 24: 6, 25: sum(T24_SIZES),
                 26: 0, 27: 3000, 28: 1, 29: 0, 30: 0, 31: -2, 32: -2, 33: 6, 34: -28, 35: 6,
                 36: 57, 37: -28, 38: -2, 39: 0, 40: 57, 41: 6,
                 42: 0 + 1 + 4095 + 4096 + 3000 + 131072})
    expect_results(c, 't_fs', s['results'], want)
    c('t_fs exit code 0', s['exit_code'] == 0, s['exit_code'])
    fs = s['disk']['fs']
    # 135 successful changes: 6 writes, 1 replace, tmp written and deleted,
    # 6 big files written and deleted, 57 empty files written and deleted
    c('kernel: 135 directory updates (seq 136, now copy B), no failure',
      fs['seq'] == 136 and fs['slot'] == 2 and fs['files'] == 6 and not fs['failed']
      and s['disk']['mounted'] == 1, fs)
    want_files = {f's{k}': pat(n, k + 1) for k, n in enumerate(T24_SIZES)}
    want_files['s4'] = pat(3000, 99)
    pfsck_files(c, 'T24', ctx, want_files, seq=136)
    c('UART: the empty disk was found', 'DISK 256 blocks, 0 files' in ctx['uart'])
    v = s['vblk']
    c('driver: writes and flushes went to the device', v['writes'] > 0 and v['flushes'] > 0,
      {k: v[k] for k in ('reads', 'writes', 'flushes')})
    queue_ok(c, 'driver', v, (ctx['sym']['__kernel_start'], ctx['sym']['__kernel_end']))


def oracle_t_fsro(c, obs, ctx):
    s = exit_data(c, obs)
    expect_results(c, 't_fsro', s['results'], {0: 5000, 1: 1, 2: -30, 3: -30, 4: -30, 5: 5000,
                                                6: 5000, 7: 1, 8: 1})
    v, mode = s['vblk'], ctx['spec']['mode']
    if mode == 'noflush':
        c('noflush disk: VIRTIO_BLK_F_FLUSH not offered nor negotiated, RO not set',
          not v['device_features'] & BLK_F_FLUSH and not v['features'] & BLK_F_FLUSH
          and not v['features'] & BLK_F_RO, hex(v['device_features']))
        c('UART: no flush, read-only', ': 2048 sectors, no flush, read-only' in ctx['uart'])
    else:
        c('readonly=on: VIRTIO_BLK_F_RO negotiated', v['features'] & BLK_F_RO, hex(v['features']))
        c('UART: read-only', ', read-only' in ctx['uart'])
    c('driver: not one write or flush reached the device', v['writes'] == 0 and v['flushes'] == 0,
      {k: v[k] for k in ('reads', 'writes', 'flushes')})
    d = ctx['disk']
    c('the disk image is byte-identical after the boot', d['sha_after'] == d['sha_before'],
      d['sha_after'])
    pfsck_files(c, 'T24 read-only', ctx, {'ro.txt': pat(5000, 5)}, seq=1)


def oracle_t_pw(c, obs, ctx):
    s = exit_data(c, obs)
    expect_results(c, 't_pw', s['results'], {0: 0, 1: 0, 2: 0, 3: 0, 4: 0, 5: 2})
    pfsck_files(c, 'T25 after boot 1', ctx, {'p1': pat(200, 4), 'p3': pat(131072, 3)}, seq=6)


def oracle_t_pr(c, obs, ctx):
    s = exit_data(c, obs)
    expect_results(c, 't_pr', s['results'], {0: 2, 1: 200 + 131072, 2: 200, 3: 1, 4: 131072,
                                              5: 1, 6: -2})
    c('boot 2 mounted what boot 1 left: seq 6 (5 changes), 2 files',
      s['disk']['fs']['seq'] == 6 and s['disk']['fs']['files'] == 2, s['disk']['fs'])
    d = ctx['disk']
    c('boot 2 only read: the image is byte-identical after it', d['sha_after'] == d['sha_before'])
    pfsck_files(c, 'T25 after boot 2', ctx, {'p1': pat(200, 4), 'p3': pat(131072, 3)}, seq=6)


def oracle_t23(c, obs, ctx):
    s = steps(obs)['first_prompt']
    v, d = s['vblk'], s['disk']
    src = ctx['disk']['pfsck']
    c('the shell came up with the disk', s['ok'])
    line = f'DISK virtio-blk at {v["base"]:#018x}: {v["capacity"]} sectors, flush, read-write'
    c('UART: the device line', line in ctx['uart'], line)
    c('UART: blocks and files match the image',
      f'DISK {src["blocks"]} blocks, {len(src["files"])} files' in ctx['uart'])
    c('DT: the 8 virtio-mmio transports of QEMU virt were found', d['boot']['virtio_count'] == 8,
      [hex(x) for x in d['boot']['virtio']])
    c('the device in use is a DT transport', v['base'] in d['boot']['virtio'], hex(v['base']))
    m = v.get('mmio', {})
    c('its registers (read physically): magic "virt", version 2, block device, DRIVER_OK',
      m.get('magic') == 0x74726976 and m.get('version') == 2 and m.get('device_id') == 2
      and m.get('status') == 0xf, m)
    c('capacity = image file size / 512', v['capacity'] == ctx['disk']['size'] // 512,
      (v['capacity'], ctx['disk']['size']))
    c('negotiated exactly VERSION_1 and FLUSH', v['features'] == VIRTIO_F_VERSION_1 | BLK_F_FLUSH,
      hex(v['features']))
    c('driver ready, queue of 8', v['state'] == 1 and v['qsize'] == 8, v)
    c('mount read exactly the superblock and both directory copies, wrote nothing',
      v['reads'] == 3 and v['writes'] == 0 and v['flushes'] == 0)
    queue_ok(c, 'driver', v, s['kernel_range'])
    fs = d['fs']
    c('kernel mounted copy A, seq 1, 256 blocks', d['mounted'] == 1 and fs['slot'] == 1
      and fs['seq'] == 1 and fs['blocks'] == src['blocks'], fs)
    want = [{k: f[k] for k in ('slot', 'name', 'size', 'first', 'count', 'crc')} for f in src['files']]
    c('kernel directory = pfsck directory of the image', fs['entries'] == want, fs['entries'])
    bad = {b: {k: w.get('flags') for k, w in ws.items()} for b, ws in s['walks'].items()
           if not all(w['mapped'] and 'U' not in w['flags'] for w in ws.values())}
    c('every transport page is mapped U=0 in the kernel, A and B roots (PT walk)', not bad, bad)
    c('the image is unchanged', ctx['disk']['sha_after'] == ctx['disk']['sha_before'])


def uart_blocks(uart):
    """command -> list of output lines, per shell prompt."""
    out = {}
    for b in uart.replace('\r', '').split('proto> ')[1:]:
        lines = b.split('\n')
        out.setdefault(lines[0], []).append(lines[1:])
    return out


def oracle_t26(c, obs, ctx):
    st = steps(obs)
    c('every command came back to the prompt', all(s.get('ok') for n, s in st.items()
                                                   if n.startswith('cmd ')))
    b = uart_blocks(ctx['uart'])
    hello = len(ctx['uelf']['hello']) if 'hello' in ctx['uelf'] else None
    c('run hello world: the program from the disk got its argument',
      b.get('run hello world', [[]])[0][:1] == ['Hello from the disk! Your argument: world'],
      b.get('run hello world'))
    procs = st['cmd run hello world']['procs']
    dp = [p for p in procs[2:] if p['prog'] >= 256]
    c('kernel: it was started from disk entry 0 with argument "world", exit 0',
      dp and dp[0]['prog'] == 256 and dp[0]['arg'] == 'world' and dp[0]['arglen'] == 5
      and dp[0]['state'] == 3 and dp[0]['exit_code'] == 0, dp[:1])
    files = b.get('run files', [[], []])
    c('run files, before and after', files[0][:1] == [f'hello  {hello} bytes'] and
      files[1][:2] == [f'hello  {hello} bytes', 'memo  23 bytes'], files)
    notes = ctx['uart'].replace('\r', '')
    c('note saved memo (23 bytes) and tmp (8 bytes); rm removed tmp',
      'saved memo: 23 bytes' in notes and 'saved tmp: 8 bytes' in notes and
      b.get('run rm tmp', [[]])[0][:1] == ['removed tmp'])
    c('run cat memo printed it', b.get('run cat memo', [[]])[0][:2] == ['first line', 'second line'])
    c('note memo with an empty first line saved nothing (memo kept, no directory update)',
      'note: nothing typed, nothing saved' in notes and notes.count('saved memo: 23 bytes') == 1)
    c('run cat nosuch: no such file', b.get('run cat nosuch', [[]])[0][:1] == ['cat: no such file'])
    fs = st['end']['disk']['fs']
    c('kernel: 3 directory updates (seq 4), files hello and memo',
      fs['seq'] == 4 and [e['name'] for e in fs['entries']] == ['hello', 'memo'], fs)
    pfsck_files(c, 'T26', ctx, {'hello': ctx['uelf']['hello'],
                                'memo': b'first line\nsecond line\n'}, seq=4)


def oracle_t27(c, obs, ctx):
    ks = obs['steps']
    c('20 boots, each killed while the driver ran (seeded)', len(ks) == 20 and all(k['started'] for k in ks),
      obs.get('seed'))
    bad = [k['name'] for k in ks if not (k['pfsck']['mounts'] and k['pfsck']['data_ok'])]
    c('after every kill: pfsck mounts the image, every file intact', not bad, bad)
    incon = [k['name'] for k in ks for f in k['files'].values() if not f['consistent']]
    c('after every kill: every file is one whole generation (old or new, never mixed)', not incon,
      incon)
    lost = []
    prev = 0
    for k in ks:
        g = k['files'].get('g', {}).get('gen') or 0
        h = k['files'].get('h', {}).get('gen') or 0
        last = k['saved_last']['g']
        if last is not None and not last <= g <= last + 1:
            lost.append((k['name'], 'g', g, last))
        lh = k['saved_last']['h']
        if lh is not None and not lh <= h <= lh + 1:
            lost.append((k['name'], 'h', h, lh))
        if not (h == g or h == g - 1 or (g == 0 and h == 0)):
            lost.append((k['name'], 'h vs g', h, g))
        if k['start_gen'][:1] != [prev]:
            lost.append((k['name'], 'start', k['start_gen'], prev))
        prev = g
    c('a write reported saved survived the kill; the next boot went on from it', not lost, lost)
    c('the series made progress (generations saved)', prev > 0 and
      sum(k['saved_count']['g'] for k in ks) >= 20, prev)


def oracle_n13(c, obs, ctx):
    s = steps(obs)['first_prompt']
    d, name = s['disk'], pathlib.Path(ctx['spec']['disk']).stem
    c('the system came up (shell prompt) with this disk', s['ok'])
    if name == 'fallback':
        c('copy A damaged, copy B valid: B mounted', d['mounted'] == 1 and d['fs']['slot'] == 2
          and d['fs']['reject_a'] == 12 and d['fs']['reject_b'] == 0, d)
        c('UART: 1 file', 'DISK 256 blocks, 1 files' in ctx['uart'])
        c('pfsck agrees', ctx['disk']['pfsck']['slot'] == 2)
    else:
        code = ctx['manifest']['disks']['disks/bad'][name]
        c(f'refused with code {code} (kernel state)', d['mounted'] == 0 and d['reject'] == code, d)
        c(f'UART: DISK rejected code=-{code}', f'DISK rejected code=-{code}\n' in
          ctx['uart'].replace('\r', ''))
        c('pfsck refuses it with the same code', ctx['disk']['pfsck']['reject'] == code,
          ctx['disk']['pfsck']['reject'])
        c('the device itself was fine (driver ready)', s['vblk']['state'] == 1)
    c('the image is unchanged', ctx['disk']['sha_after'] == ctx['disk']['sha_before'])


def oracle_t_fsbad(c, obs, ctx):
    s = exit_data(c, obs)
    r = s['results']
    want = dict(zip(range(1, 12), [-22, -22, -22, -14, -14, -14, -14, -27, -22, -14, -14]))
    want.update({12: 3000, 13: 1, 14: -5, 15: -5, 16: 3000, 17: -8, 18: -5, 19: -2, 20: 4,
                 21: 0, 22: 3, 23: r[0]})
    expect_results(c, 't_fsbad', r, want)
    c('no page leaked (free pages before = after the bad calls)', r[23] == r[0], (r[0], r[23]))
    first = steps(obs).get('first_meminfo', {})
    free0, free1 = first.get('mem', {}).get('free'), s['mem']['free']
    c('GDB: allocator free-page counter at the first meminfo of the driver = at its exit',
      first.get('hit') and free0 == free1 == r[0], (free0, free1, r[0]))
    fs = s['disk']['fs']
    c('the file system did not stop: CRC damage is EIO for that file only',
      not fs['failed'] and s['disk']['mounted'] == 1, fs)
    c('the broken ELF from the disk was refused by elf_check', s['sched']['load_errors'] >= 1,
      s['sched'].get('last_load_error'))
    p = ctx['disk']['pfsck']
    bad = sorted(f['name'] for f in p['files'] if not f['crc_ok'])
    c('pfsck: the same two files damaged (dmg, dmgprog), the rest intact, "ok" added',
      bad == ['dmg', 'dmgprog'] and ctx['disk']['data'].get('ok') == b'abc' and
      sorted(ctx['disk']['data']) == ['badelf', 'dmg', 'dmgprog', 'good', 'ok'],
      (bad, sorted(ctx['disk']['data'])))


def oracle_t_nodisk(c, obs, ctx):
    s = exit_data(c, obs)
    expect_results(c, 't_nodisk', s['results'], {0: -19, 1: -19, 2: -19, 3: -19, 4: -2, 5: 0,
                                                  6: 1, 7: 42})
    c('kernel: no disk mounted', s['disk']['mounted'] == 0, s['disk'])
    lines = [x for x in ctx['uart'].replace('\r', '').split('\n') if x.startswith('DISK')]
    if ctx['spec'].get('mode') == 'legacy':
        v = s['vblk']
        c('legacy device refused cleanly', v['state'] == 3 and v['legacy_base'] in
          s['disk']['boot']['virtio'] and v['base'] == 0, v)
        c('UART: exactly the refusal line',
          lines == [f'DISK refused: legacy virtio-mmio block device at {v["legacy_base"]:#018x}'], lines)
        c('the image is unchanged', ctx['disk']['sha_after'] == ctx['disk']['sha_before'])
    else:
        c('no disk: no driver state, no DISK line (as v1.4)', s['vblk']['state'] == 0 and not lines,
          lines)


ORACLES.update({'t23': oracle_t23, 't_fs': oracle_t_fs, 't_fsro': oracle_t_fsro,
                't_pw': oracle_t_pw, 't_pr': oracle_t_pr, 't26': oracle_t26, 't27': oracle_t27,
                'n13': oracle_n13, 't_fsbad': oracle_t_fsbad, 't_nodisk': oracle_t_nodisk})


def judge(probe, obs, ctx):
    c = Checks()
    if not obs.get('complete'):
        return 'INCONCLUSIVE', c.items, f'probe incomplete: {obs.get("error")}'
    try:
        ORACLES[probe](c, obs, ctx)
        common_checks(c, ctx)
    except (KeyError, IndexError, TypeError) as e:
        return 'INCONCLUSIVE', c.items, f'missing observation: {type(e).__name__}: {e}'
    status = 'PASS' if all(i['ok'] for i in c.items) else 'FAIL'
    return status, c.items, ''


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', default=str(ROOT / 'build/protoos.elf'))
    ap.add_argument('--images', default=str(ROOT / 'build'),
                    help='v1.4 program archives, user/*.elf and images.json (build-wsl.sh)')
    ap.add_argument('--tests', default=','.join(MATRIX))
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--t7-seconds', type=int, default=20)
    ap.add_argument('--out', default=str(ROOT / 'evidence/acceptance'))
    args = ap.parse_args()
    elf = pathlib.Path(args.elf).resolve()
    image = sha256(elf)
    run_id = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    out = pathlib.Path(args.out) / run_id
    out.mkdir(parents=True)
    shutil.copyfile(elf, out / 'protoos.elf')
    work = pathlib.Path(tempfile.mkdtemp(prefix=f'protoos-acceptance-{run_id}-'))
    elf_src, elf = elf, work / 'protoos.elf'
    shutil.copyfile(elf_src, elf)
    sym, uva = load_symbols(elf)
    offsets = struct_offsets(elf)
    images = pathlib.Path(args.images).resolve()
    v14 = load_v14(images, work, out, args.tests.split(','))
    procs_addr = [sym['procs'] + i * offsets['sizeof_process'] for i in (0, 1)]
    env = {'elf_sha256': image, 'firmware': FW, 'firmware_sha256': sha256(FW),
           'qemu': tool_version([QEMU, '--version']), 'gdb': tool_version([GDB, '--version']),
           'zig': tool_version([os.path.expanduser('~/.protoos-venv/bin/zig'), 'version']),
           'host': f'{os.uname().sysname} {os.uname().release} {os.uname().machine}', 'python': sys.version.split()[0],
           'git_head': git_head(),
           'struct_offsets': offsets, 'images': v14['images_sha256']}
    (out / 'environment.json').write_text(json.dumps(env, indent=1))
    results = []
    for test in args.tests.split(','):
        for rep in range(1, args.reps + 1):
            parts = []
            shared = {}                 # v1.5: the disk copy of this repetition
            for scenario, probe, *img in MATRIX[test]:
                spec = img[1] if len(img) > 1 else None
                img = img[0] if img else None
                name = scenario.split('=')[1] + (f'+{pathlib.Path(img).stem}' if img else '')
                if spec:
                    name += f'+{pathlib.Path(spec.get("disk", "nodisk")).stem}'
                    name += f'-{spec["mode"]}' if spec.get('mode') else ''
                rel = pathlib.Path('raw') / test / f'run{rep}' / name
                initrd = v14['work_images'] / img if img else None
                # QEMU writes the -d int trace synchronously from the vCPU
                # thread; on a slow filesystem (WSL /mnt/c) that stretches
                # every trap past the 10 ms quantum. Record on local tmpfs,
                # then copy the finished boot directory into the evidence.
                rundir = work / rel
                rundir.mkdir(parents=True)
                # v1.5: a fresh copy of the disk image for this boot (or the
                # repetition's shared copy); it stays in the work directory,
                # the evidence keeps its SHA-256 and pfsck's verdict.
                disk = dict(spec) if spec else None      # no 'path': no disk at all
                if spec and spec.get('disk'):
                    src = v14['work_images'] / spec['disk']
                    path = shared.get('path') if spec.get('share') else None
                    if not path:
                        path = (work / 'disks' / test / f'run{rep}' /
                                ('shared' if spec.get('share') else name) / 'disk.img')
                        path.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(src, path)
                        if spec.get('share'):
                            shared['path'] = path
                    disk = dict(spec, path=str(path))
                disk_before = sha256(disk['path']) if disk and disk.get('path') else None
                t0 = time.monotonic()
                if probe == 't7':
                    obs = boot_free_running(elf, scenario, rundir, sym, offsets, args.t7_seconds)
                elif probe == 't27':
                    obs = boot_crash_series(elf, scenario, rundir, initrd, disk, seed=1505 + rep)
                else:
                    obs = boot_with_gdb(elf, scenario, probe, rundir, sym, uva, initrd,
                                        v14['usym'], disk)
                disk_ctx = None
                if disk and disk.get('path'):
                    sys.path.insert(0, str(ROOT / 'build-scripts'))
                    import pfsck
                    raw = pathlib.Path(disk['path']).read_bytes()
                    disk_ctx = {'spec': spec, 'sha_source': v14['images_sha256'].get(spec['disk']),
                                'sha_before': disk_before, 'sha_after': sha256(disk['path']),
                                'pfsck': pfsck.check(raw), 'data': {}, 'size': len(raw)}
                    for f in disk_ctx['pfsck']['files']:
                        disk_ctx['data'][f['name']] = raw[f['first'] * 4096:
                                                          f['first'] * 4096 + f['size']]
                    (rundir / 'pfsck.json').write_text(json.dumps(disk_ctx['pfsck'], indent=1))
                ctx = {'trace': parse_trace(rundir / 'qemu-int.log')
                       if (rundir / 'qemu-int.log').exists() else [],
                       'uart': (rundir / 'uart.log').read_text(errors='replace')
                       if (rundir / 'uart.log').exists() else '',
                       'sym': sym, 'uva': uva, 'sha': image, 'sha_after': sha256(elf),
                       'procs_addr': procs_addr, 't7_seconds': args.t7_seconds,
                       'image': img, 'image_sha': v14['images_sha256'].get(img),
                       'image_sha_after': sha256(initrd) if initrd else None,
                       'image_size': initrd.stat().st_size if initrd else None,
                       'manifest': v14['manifest'], 'usym': v14['usym'], 'uelf': v14['uelf'],
                       'disk': disk_ctx, 'spec': spec or {}, 'test': test}
                status, items, note = judge((spec or {}).get('autorun') or probe, obs, ctx)
                verdict = {'test': test, 'repetition': rep, 'scenario': scenario, 'probe': probe,
                           'program_archive': img,
                           'disk': ({k: v for k, v in disk_ctx.items() if k != 'data'}
                                    if disk_ctx else None),
                           'status': status, 'note': note, 'elf_sha256': image,
                           'seconds': round(time.monotonic() - t0, 2), 'checks': items,
                           'summary': obs.get('summary'),
                           'raw_sha256': {p.name: sha256(p) for p in sorted(rundir.iterdir())
                                          if p.is_file()}}
                (rundir / 'verdict.json').write_text(json.dumps(verdict, indent=1))
                (out / rel).mkdir(parents=True)
                for f in sorted(rundir.rglob('*')):     # plain copy: /mnt/c rejects copystat
                    dst = out / rel / f.relative_to(rundir)
                    if f.is_dir():
                        dst.mkdir(parents=True, exist_ok=True)
                    else:
                        shutil.copyfile(f, dst)
                parts.append(verdict)
                bad = [i['check'] for i in items if not i['ok']]
                print(f'{test} run{rep} {name:28s} {status:12s} '
                      f'{len(items) - len(bad)}/{len(items)} checks {note} {bad[:2]}', flush=True)
            st = [p['status'] for p in parts]
            rep_status = 'PASS' if all(s == 'PASS' for s in st) else (
                'FAIL' if 'FAIL' in st else 'INCONCLUSIVE')
            results.append({'test': test, 'repetition': rep, 'status': rep_status,
                            'boots': [{k: p[k] for k in ('scenario', 'program_archive', 'status', 'note',
                                              'seconds')}
                                      for p in parts]})
    per_test = {}
    for t in args.tests.split(','):
        st = [r['status'] for r in results if r['test'] == t]
        per_test[t] = 'PASS' if len(st) >= 3 and all(s == 'PASS' for s in st) else (
            'FAIL' if 'FAIL' in st else ('INCONCLUSIVE' if st else 'NOT_RUN'))
    summary = {'run_id': run_id, 'elf_sha256': image, 'repetitions': args.reps,
               'per_test': per_test, 'all_pass': all(v == 'PASS' for v in per_test.values()),
               'environment': env, 'results': results}
    (out / 'RESULTS.json').write_text(json.dumps(summary, indent=1))
    print(json.dumps({'run_id': run_id, 'per_test': per_test, 'all_pass': summary['all_pass']}))
    return 0 if summary['all_pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
