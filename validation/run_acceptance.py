#!/usr/bin/env python3
"""ProtoOS v1.0 acceptance runner: T1-T7 and N1-N5, 3 clean boots each.

Every scenario boot is a fresh qemu-system-riscv64 process (fresh RAM, same
ELF, SHA-256 checked before and after). Independent evidence per boot:
  * QEMU `-d int` trace: every architectural trap/interrupt with cause/epc/tval
  * GDB (validation/gdb_probe.py): PC, privilege, CSRs, registers, saved
    contexts, physical memory and an independent Sv39 page-table walk
  * QMP (T7): physical-memory sampling of a free-running Normal boot
UART output is recorded for correlation only and never decides a PASS alone.

Statuses: PASS / FAIL (violation observed) / INCONCLUSIVE (probe incomplete).
Usage: python3 validation/run_acceptance.py [--elf build/protoos.elf]
       [--tests T1,T3] [--reps 3] [--t7-seconds 20]
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
A_PATTERN, B_PATTERN = 0xa1a1a1a100000000, 0xb2b2b2b200000000
STATE = {1: 'READY', 2: 'RUNNING', 3: 'EXITED', 4: 'FAULTED', 5: 'SLEEPING'}
M64 = (1 << 64) - 1
TIMER = (1 << 63) | 5

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
}


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
        for prefix, text, base in (('a_', '__user_a_text', A_BASE), ('b_', '__user_b_text', B_BASE)):
            if name.startswith(prefix) and sym[text] <= addr < sym[text] + 0x1000:
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
def qemu_cmd(elf, args, rundir, gdb_port=None, qmp=None):
    cmd = [QEMU, '-machine', 'virt', '-accel', 'tcg', '-cpu', 'rv64', '-smp', '1',
           '-m', '128M', '-bios', FW, '-kernel', str(elf), '-display', 'none',
           '-monitor', 'none', '-serial', f'file:{rundir / "uart.log"}', '-nic', 'none',
           '-append', args, '-no-reboot', '-d', 'int,guest_errors',
           '-D', str(rundir / 'qemu-int.log')]
    if gdb_port:
        cmd += ['-S', '-gdb', f'tcp:127.0.0.1:{gdb_port}']
    if qmp:
        cmd += ['-qmp', f'unix:{qmp},server=on,wait=off']
    return cmd


def boot_with_gdb(elf, args, probe, rundir, sym, uva):
    port = free_port()
    cmd = qemu_cmd(elf, args, rundir, gdb_port=port)
    cfg = {'probe': probe, 'symbols': sym, 'user_va': uva, 'elf': str(elf), 'port': port,
           'observations': str(rundir / 'observations.json')}
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
    p = rundir / 'observations.json'
    return json.loads(p.read_text()) if p.exists() else {'steps': [], 'complete': False,
                                                         'error': 'no observations'}


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
        t0 = time.time()
        time.sleep(1.0)
        n = int(seconds // 5)
        for i in range(n + 1):
            if i:
                time.sleep(max(0.0, t0 + 1.0 + 5 * i - time.time()))
            s = {'host_elapsed': time.time() - t0}
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
    procs_addr = [sym['procs'] + i * offsets['sizeof_process'] for i in (0, 1)]
    env = {'elf_sha256': image, 'firmware': FW, 'firmware_sha256': sha256(FW),
           'qemu': tool_version([QEMU, '--version']), 'gdb': tool_version([GDB, '--version']),
           'zig': tool_version([os.path.expanduser('~/.protoos-venv/bin/zig'), 'version']),
           'host': f'{os.uname().sysname} {os.uname().release} {os.uname().machine}', 'python': sys.version.split()[0],
           'git_head': git_head(),
           'struct_offsets': offsets}
    (out / 'environment.json').write_text(json.dumps(env, indent=1))
    results = []
    for test in args.tests.split(','):
        for rep in range(1, args.reps + 1):
            parts = []
            for scenario, probe in MATRIX[test]:
                rel = pathlib.Path('raw') / test / f'run{rep}' / scenario.split('=')[1]
                # QEMU writes the -d int trace synchronously from the vCPU
                # thread; on a slow filesystem (WSL /mnt/c) that stretches
                # every trap past the 10 ms quantum. Record on local tmpfs,
                # then copy the finished boot directory into the evidence.
                rundir = work / rel
                rundir.mkdir(parents=True)
                t0 = time.time()
                if probe == 't7':
                    obs = boot_free_running(elf, scenario, rundir, sym, offsets, args.t7_seconds)
                else:
                    obs = boot_with_gdb(elf, scenario, probe, rundir, sym, uva)
                ctx = {'trace': parse_trace(rundir / 'qemu-int.log')
                       if (rundir / 'qemu-int.log').exists() else [],
                       'uart': (rundir / 'uart.log').read_text(errors='replace')
                       if (rundir / 'uart.log').exists() else '',
                       'sym': sym, 'uva': uva, 'sha': image, 'sha_after': sha256(elf),
                       'procs_addr': procs_addr, 't7_seconds': args.t7_seconds}
                status, items, note = judge(probe, obs, ctx)
                verdict = {'test': test, 'repetition': rep, 'scenario': scenario, 'probe': probe,
                           'status': status, 'note': note, 'elf_sha256': image,
                           'seconds': round(time.time() - t0, 2), 'checks': items,
                           'summary': obs.get('summary'),
                           'raw_sha256': {p.name: sha256(p) for p in sorted(rundir.iterdir())
                                          if p.is_file()}}
                (rundir / 'verdict.json').write_text(json.dumps(verdict, indent=1))
                (out / rel).mkdir(parents=True)
                for f in rundir.iterdir():     # plain copy: /mnt/c rejects copystat
                    shutil.copyfile(f, out / rel / f.name)
                parts.append(verdict)
                bad = [i['check'] for i in items if not i['ok']]
                print(f'{test} run{rep} {scenario:28s} {status:12s} '
                      f'{len(items) - len(bad)}/{len(items)} checks {note} {bad[:2]}', flush=True)
            st = [p['status'] for p in parts]
            rep_status = 'PASS' if all(s == 'PASS' for s in st) else (
                'FAIL' if 'FAIL' in st else 'INCONCLUSIVE')
            results.append({'test': test, 'repetition': rep, 'status': rep_status,
                            'boots': [{k: p[k] for k in ('scenario', 'status', 'note', 'seconds')}
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
