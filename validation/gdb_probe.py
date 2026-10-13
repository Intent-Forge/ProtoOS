"""ProtoOS acceptance probe, executed INSIDE gdb-multiarch (gdb Python API).

Observation only: the probe sets breakpoints and reads registers, CSRs,
virtual and physical memory. It never writes guest memory or registers and
never performs any kernel function (scheduling, ecall handling, MMU setup).

Configuration comes from the JSON file named by PROTOOS_PROBE_CFG:
  probe         probe name (see PROBES)
  symbols       name -> address (kernel symbols are physical = virtual)
  user_va       name -> user virtual address of user.S labels
  observations  output JSON path (rewritten after every step)
  uart_sock     (v1.2 input scenarios) UNIX socket of the guest UART; the
                probe types into it like a terminal user would
"""
import gdb
import json
import os
import signal
import socket
import threading
import time

CFG = json.load(open(os.environ['PROTOOS_PROBE_CFG']))
SYM = CFG['symbols']
UVA = CFG['user_va']
OUT = CFG['observations']
M64 = (1 << 64) - 1
TIMER_CAUSE = (1 << 63) | 5
GPR = ['ra', 'sp', 'gp', 'tp', 't0', 't1', 't2', 's0', 's1', 'a0', 'a1', 'a2',
       'a3', 'a4', 'a5', 'a6', 'a7', 's2', 's3', 's4', 's5', 's6', 's7', 's8',
       's9', 's10', 's11', 't3', 't4', 't5', 't6']          # x1..x31
FRAME_A = '((unsigned long)&procs[0])'
FRAME_B = '((unsigned long)&procs[1])'

obs = {'probe': CFG['probe'], 'steps': [], 'complete': False, 'error': None}


def flush():
    tmp = OUT + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(obs, f, indent=1)
    os.replace(tmp, OUT)


def ev(expr):
    """Evaluate a kernel expression over PHYSICAL memory. QEMU translates
    debugger accesses with the current privilege, so kernel pages (U=0) are
    unreadable while the CPU is in U-mode; the kernel is identity mapped,
    so its symbol addresses are its physical addresses."""
    gdb.execute('maintenance packet Qqemu.PhyMemMode:1', to_string=True)
    try:
        return int(gdb.parse_and_eval(expr)) & M64
    finally:
        gdb.execute('maintenance packet Qqemu.PhyMemMode:0', to_string=True)


def reg(name):
    return int(gdb.parse_and_eval('$' + name)) & M64


def cpu():
    out = {}
    for r in ('pc', 'priv', 'satp', 'sstatus', 'scause', 'sepc', 'stval',
              'sscratch', 'sie', 'stvec'):
        out[r] = reg(r)
    return out


def gprs():
    return {f'x{i + 1}': reg(n) for i, n in enumerate(GPR)}


def frame(i):
    f = {f'x{k}': ev(f'procs[{i}].frame.x[{k}]') for k in range(1, 32)}
    for c in ('sepc', 'sstatus', 'scause', 'stval'):
        f[c] = ev(f'procs[{i}].frame.{c}')
    return f


def proc(i):
    out = {k: ev(f'procs[{i}].{k}') for k in (
        'pid', 'state', 'satp', 'dispatches', 'preemptions', 'yields',
        'syscalls', 'rejected_syscalls', 'exit_code', 'fault_cause',
        'fault_epc', 'fault_tval', 'writes', 'bytes_written', 'sleeps', 'sleep_start',
        'wake_time', 'last_dispatch_time', 'reads', 'bytes_read', 'read_pending',
        'read_va', 'read_len', 'block_start', 'prog', 'killed', 'npages')}
    out['pages'] = [ev(f'procs[{i}].pages[{k}]') for k in range(5)]
    out['addr'] = ev(f'(unsigned long)&procs[{i}]')
    return out


def mem_state():
    """v1.3 page allocator state, plus the bitmap bit of chosen pages."""
    return {k: ev(f'mem.{k}') for k in ('base', 'pages', 'free', 'allocs', 'frees')}


def page_bits(pages):
    out = []
    for pa in pages:
        n = (pa - SYM_POOL) // 4096
        out.append(ev(f'mem.bitmap[{n // 64}]') >> (n % 64) & 1 if pa >= SYM_POOL else None)
    return out


SYM_POOL = 0x80400000


def sched():
    return {k: ev(f'sched.{k}') for k in (
        'quantum_ticks', 'timer_ticks', 'switches', 'idle_entered', 'verbose', 'waits',
        'ext_irqs', 'next_pid', 'spawned', 'killed', 'reclaimed')}


def inp():
    """Kernel input ring state (v1.2)."""
    return {k: ev(f'input.{k}') for k in ('count', 'head', 'irqs', 'received', 'dropped',
                                           'spurious')}


class Uart:
    """Client end of the guest UART (QEMU chardev socket). A background
    thread drains guest output so the guest never blocks on a full socket."""

    def __init__(self):
        self.buf = bytearray()
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(200):
            try:
                self.s.connect(CFG['uart_sock'])
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError('cannot connect to the UART socket')
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self):
        while True:
            try:
                data = self.s.recv(4096)
            except OSError:
                return
            if not data:
                return
            self.buf.extend(data)

    def send(self, data):
        self.s.sendall(data)

    def text(self):
        return bytes(self.buf).decode('ascii', 'replace')


def phys(addr, size):
    gdb.execute('maintenance packet Qqemu.PhyMemMode:1', to_string=True)
    try:
        return bytes(gdb.selected_inferior().read_memory(addr, size))
    finally:
        gdb.execute('maintenance packet Qqemu.PhyMemMode:0', to_string=True)


def phys_u64(addr):
    return int.from_bytes(phys(addr, 8), 'little')


def user_words(name):
    """First 8 words of a user data page, read physically."""
    raw = phys(SYM[name], 64)
    return [int.from_bytes(raw[i:i + 8], 'little') for i in range(0, 64, 8)]


PTE_BITS = 'VRWXUGAD'


def walk(satp, va):
    """Independent Sv39 walk over physical memory (Privileged ISA 1.12 §10.3.2)."""
    table = (satp & ((1 << 44) - 1)) << 12
    out = {'satp': satp, 'va': va, 'mode': satp >> 60, 'levels': []}
    for level in (2, 1, 0):
        vpn = (va >> (12 + 9 * level)) & 0x1ff
        pte_addr = table + 8 * vpn
        pte = phys_u64(pte_addr)
        out['levels'].append({'level': level, 'pte_addr': pte_addr, 'pte': pte})
        if not pte & 1:
            out['mapped'] = False
            return out
        if pte & 0xa:   # R or X: leaf
            out.update(mapped=True, leaf_level=level, pte=pte,
                       flags=''.join(b for i, b in enumerate(PTE_BITS) if pte >> i & 1),
                       pa=((pte >> 10) << 12) | (va & 0xfff))
            return out
        table = (pte >> 10) << 12
    out['mapped'] = False
    return out


def step(name, **data):
    data['name'] = name
    data['host_time'] = time.time()
    obs['steps'].append(data)
    flush()


def clear():
    for b in gdb.breakpoints() or ():
        b.delete()


def bp(addr, cond=None):
    b = gdb.Breakpoint(f'*{addr:#x}', internal=False)
    if cond:
        b.condition = cond
    return b


def run_to(*bps, wall=30.0):
    """Continue until one of bps stops the target or `wall` seconds pass.
    Returns the breakpoint that was hit, or None on the wall-clock limit."""
    before = {b.number: b.hit_count for b in bps}
    timer = threading.Timer(wall, lambda: os.kill(os.getpid(), signal.SIGINT))
    timer.start()
    try:
        gdb.execute('continue', to_string=True)
    except (KeyboardInterrupt, gdb.error):
        pass
    finally:
        timer.cancel()
    for b in bps:
        if b.hit_count != before[b.number]:
            return b
    return None


def stop_at(addr, cond=None, wall=30.0):
    clear()
    b = bp(addr, cond)
    hit = run_to(b, wall=wall)
    return hit is not None


def trap_cond(frame, cause, a7=None):
    c = f'$a0 == {frame} && *(unsigned long *)($a0 + 272) == {cause:#x}'
    if a7 is not None:
        c += f' && *(unsigned long *)($a0 + 136) == {a7}'
    return c


def wait_ticks(n, watch=()):
    """Let the guest run for n timer ticks; report any watched breakpoint hit."""
    target = ev('sched.timer_ticks') + n
    clear()
    w = bp(SYM['trap_from_user'], f'sched.timer_ticks >= {target}')
    others = [(name, bp(addr, cond)) for name, addr, cond in watch]
    hit = run_to(w, *[b for _, b in others], wall=60.0)
    if hit is w:
        return {'reached': True, 'watch_hit': None}
    for name, b in others:
        if hit is b:
            return {'reached': False, 'watch_hit': name, 'cpu': cpu()}
    return {'reached': False, 'watch_hit': None}


def satps():
    return {'kernel': ev('vm_kernel_satp'), 'A': ev('vm_proc_satp[0]'),
            'B': ev('vm_proc_satp[1]')}


# ----------------------------------------------------------------- probes
def probe_t1():
    step('attach', cpu=cpu())
    step('kernel_entry', hit=stop_at(SYM['_start']), cpu=cpu(), a0=reg('a0'), a1=reg('a1'))
    step('kernel_main', hit=stop_at(SYM['kernel_main']), cpu=cpu())
    hit = stop_at(SYM['halt'])
    s = reg('satp')
    step('halt', hit=hit, cpu=cpu(), boot_phase=ev('boot_phase'), satps=satps(),
         walk_text=walk(s, SYM['_start']), walk_rodata=walk(s, SYM['kernel_secret']),
         walk_stack=walk(s, SYM['boot_stack_top'] - 8),
         walk_guard=walk(s, SYM['kernel_stack_guard']),
         walk_user_a=walk(s, UVA['a_worker']), walk_uart=walk(s, 0x10000000))


def probe_t2():
    for rnd in (1, 2):
        for who, loop, data in (('A', 'a_worker_loop', 'user_a_data'),
                                ('B', 'b_worker_loop', 'user_b_data')):
            hit = stop_at(UVA[loop])
            step(f'{who}_in_user_{rnd}', hit=hit, cpu=cpu(), gprs=gprs(), satps=satps(),
                 procs=[proc(0), proc(1)], other_frame=frame(1 if who == 'A' else 0),
                 words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))


def probe_t3():
    step('A_running', hit=stop_at(UVA['a_worker_loop']), cpu=cpu())
    hit = stop_at(SYM['trap_from_user'], trap_cond(FRAME_A, TIMER_CAUSE))
    saved = frame(0)
    step('A_preempted', hit=hit, cpu=cpu(), frame=saved, proc_a=proc(0), proc_b=proc(1),
         sched=sched())
    # Arm BOTH watchpoints now: the first instruction A would execute again
    # (its saved pc) and B's next trap. Any A execution must pass the first.
    clear()
    b_trap = bp(SYM['trap_from_user'], f'$a0 == {FRAME_B}')
    a_resume = bp(saved['sepc'])
    first = run_to(b_trap, a_resume)
    step('B_trapped', first='B' if first is b_trap else ('A' if first is a_resume else None),
         cpu=cpu(), frame_b=frame(1), proc_a=proc(0), proc_b=proc(1), sched=sched(),
         satps=satps())
    b_trap.delete()
    hit = run_to(a_resume) if first is b_trap else None
    step('A_resumed', hit=hit is not None, cpu=cpu(), gprs=gprs(), sched=sched(),
         proc_a=proc(0), satps=satps())


def probe_t4_getpid():
    for who, frm, ret, data in (('A', FRAME_A, 'a_getpid_ret', 'user_a_data'),
                                ('B', FRAME_B, 'b_getpid_ret', 'user_b_data')):
        idx = 0 if who == 'A' else 1
        hit = stop_at(SYM['trap_from_user'], trap_cond(frm, 8))
        step(f'{who}_ecall', hit=hit, cpu=cpu(), frame=frame(idx))
        hit = stop_at(UVA[ret])
        step(f'{who}_returned', hit=hit, cpu=cpu(), gprs=gprs(), satps=satps())
    w = wait_ticks(20)
    step('after', wait=w, procs=[proc(0), proc(1)],
         words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))


def probe_t4_yield():
    hit = stop_at(SYM['trap_from_user'], trap_cond(FRAME_A, 8))
    step('A_ecall', hit=hit, cpu=cpu(), frame=frame(0), sched=sched())
    hit = stop_at(UVA['b_yield'])
    step('B_runs', hit=hit, cpu=cpu(), sched=sched(), satps=satps())
    hit = stop_at(UVA['a_yield_ret'])
    step('A_resumed', hit=hit, cpu=cpu(), gprs=gprs(), sched=sched(), satps=satps())
    w = wait_ticks(10)
    step('after', wait=w, procs=[proc(0), proc(1)], sched=sched(),
         words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))


def probe_t4_exit():
    hit = stop_at(SYM['trap_from_user'], trap_cond(FRAME_A, 8, a7=93))
    step('A_exit_ecall', hit=hit, cpu=cpu(), frame=frame(0),
         words_a=user_words('user_a_data'))
    watch = (('a_exit_after', UVA['a_exit_after'], None),
             ('dispatch_A', SYM['sched_dispatch'], f'$a0 == {FRAME_A}'))
    w1 = wait_ticks(30, watch)
    s1 = dict(procs=[proc(0), proc(1)], words_a=user_words('user_a_data'),
              words_b=user_words('user_b_data'))
    w2 = wait_ticks(30, watch)
    step('after', wait1=w1, wait2=w2, first=s1, procs=[proc(0), proc(1)],
         words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))


def probe_cross():
    hit = stop_at(UVA['a_cross_write_store'])
    s = satps()
    target = reg('t0')
    step('before_store', hit=hit, cpu=cpu(), gprs=gprs(), satps=s,
         walk_a_target=walk(s['A'], target), walk_b_target=walk(s['B'], target),
         walk_a_kernel=walk(s['A'], SYM['kernel_secret']),
         walk_a_own=walk(s['A'], UVA['a_worker']),
         b_data=phys(SYM['user_b_data'], 4096).hex(),
         b_stack=phys(SYM['user_b_stack'], 4096).hex(), procs=[proc(0), proc(1)])
    hit = stop_at(SYM['proc_fault'])
    step('fault', hit=hit, cpu=cpu(), p=reg('a0'), frame=frame(0),
         b_data=phys(SYM['user_b_data'], 4096).hex(),
         b_stack=phys(SYM['user_b_stack'], 4096).hex())
    watch = (('dispatch_A', SYM['sched_dispatch'], f'$a0 == {FRAME_A}'),)
    w1 = wait_ticks(20, watch)
    b1 = user_words('user_b_data')
    w2 = wait_ticks(20, watch)
    step('after', wait1=w1, wait2=w2, procs=[proc(0), proc(1)], words_b_first=b1,
         words_a=user_words('user_a_data'), words_b=user_words('user_b_data'),
         b_data=phys(SYM['user_b_data'], 4096).hex())


def probe_kernel_read():
    hit = stop_at(UVA['a_kernel_read_load'])
    s = satps()
    step('before_load', hit=hit, cpu=cpu(), gprs=gprs(), satps=s,
         walk_a_secret=walk(s['A'], reg('t0')),
         secret_phys=phys_u64(SYM['kernel_secret']))
    hit = stop_at(SYM['proc_fault'])
    step('fault', hit=hit, cpu=cpu(), p=reg('a0'), frame=frame(0))
    w = wait_ticks(20)
    b1 = user_words('user_b_data')
    w2 = wait_ticks(20)
    step('after', wait1=w, wait2=w2, procs=[proc(0), proc(1)], words_b_first=b1,
         words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))


def probe_t6():
    hit = stop_at(UVA['a_fault_store'])
    s = satps()
    step('before_store', hit=hit, cpu=cpu(), gprs=gprs(), satps=s,
         walk_a_target=walk(s['A'], reg('t0')), frame_b=frame(1), proc_b=proc(1),
         b_data=phys(SYM['user_b_data'], 4096).hex(),
         b_stack=phys(SYM['user_b_stack'], 4096).hex())
    hit = stop_at(SYM['proc_fault'])
    step('fault', hit=hit, cpu=cpu(), p=reg('a0'), frame=frame(0))
    saved_b = frame(1)
    hit = stop_at(saved_b['sepc'])
    step('B_resumed', hit=hit, cpu=cpu(), gprs=gprs(), frame_b=saved_b, satps=s)
    watch = (('dispatch_A', SYM['sched_dispatch'], f'$a0 == {FRAME_A}'),)
    w1 = wait_ticks(20, watch)
    b1 = user_words('user_b_data')
    w2 = wait_ticks(20, watch)
    step('after', wait1=w1, wait2=w2, procs=[proc(0), proc(1)], words_b_first=b1,
         words_b=user_words('user_b_data'),
         b_data=phys(SYM['user_b_data'], 4096).hex(),
         b_stack=phys(SYM['user_b_stack'], 4096).hex())


def probe_n3():
    hit = stop_at(UVA['a_csr_write'])
    step('before_csr', hit=hit, cpu=cpu(), gprs=gprs(), satps=satps())
    hit = stop_at(SYM['proc_fault'])
    step('fault', hit=hit, cpu=cpu(), p=reg('a0'), frame=frame(0))
    w = wait_ticks(20)
    b1 = user_words('user_b_data')
    w2 = wait_ticks(20)
    step('after', wait1=w, wait2=w2, procs=[proc(0), proc(1)], words_b_first=b1,
         words_b=user_words('user_b_data'))


def probe_n4():
    hit = stop_at(SYM['trap_from_user'], trap_cond(FRAME_A, 8, a7=999))
    step('A_ecall', hit=hit, cpu=cpu(), frame=frame(0))
    hit = stop_at(UVA['a_bad_syscall_ret'])
    step('A_returned', hit=hit, cpu=cpu(), gprs=gprs(), satps=satps())
    w = wait_ticks(20)
    s1 = dict(words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))
    w2 = wait_ticks(20)
    step('after', wait1=w, wait2=w2, first=s1, procs=[proc(0), proc(1)], sched=sched(),
         words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))


def probe_n5():
    hit = stop_at(SYM['sched_idle'], wall=60.0)
    step('idle_entry', hit=hit, cpu=cpu(), procs=[proc(0), proc(1)], sched=sched())
    clear()
    watch = [bp(SYM['user_return']), bp(SYM['trap_entry']), bp(SYM['sched_dispatch'])]
    hit = run_to(*watch, wall=3.0)
    step('idle_window', watch_hit=None if hit is None else hit.location, cpu=cpu(),
         sched=sched(), satps=satps(), procs=[proc(0), proc(1)])


PROBES = {
    't1': probe_t1, 't2': probe_t2, 't3': probe_t3,
    't4_getpid': probe_t4_getpid, 't4_yield': probe_t4_yield, 't4_exit': probe_t4_exit,
    'cross': probe_cross, 'kernel_read': probe_kernel_read, 't6': probe_t6,
    'n3': probe_n3, 'n4': probe_n4, 'n5': probe_n5,
}


# ------------------------------------------------------------ v1.1 probes
def probe_t8():
    for k in (1, 2):
        hit = stop_at(SYM['trap_from_user'], trap_cond(FRAME_A, 8, a7=64))
        s = satps()             # read after the kernel built its page tables
        fr = frame(0)
        step(f'write{k}_ecall', hit=hit, cpu=cpu(), frame=fr, satps=s,
             walk_first=walk(s['A'], fr['x11']),
             walk_last=walk(s['A'], fr['x11'] + max(fr['x12'], 1) - 1))
        hit = stop_at(UVA['a_write_ret'])
        step(f'write{k}_ret', hit=hit, cpu=cpu(), gprs=gprs(), proc_a=proc(0))
    w = wait_ticks(20)
    a1 = user_words('user_a_data')
    w2 = wait_ticks(20)
    step('after', wait1=w, wait2=w2, procs=[proc(0), proc(1)], words_a_first=a1,
         words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))


def probe_n6():
    hit = stop_at(UVA['a_write_bad_done'])
    s = satps()
    step('done', hit=hit, cpu=cpu(), satps=s, proc_a=proc(0),
         results=[phys_u64(SYM['user_a_data'] + 0x40 + 8 * k) for k in range(8)],
         walk_secret=walk(s['A'], SYM['kernel_secret']),
         walk_b_data=walk(s['A'], 0x801000), walk_guard=walk(s['A'], 0x402000))
    w = wait_ticks(20)
    a1, b1 = user_words('user_a_data'), user_words('user_b_data')
    w2 = wait_ticks(20)
    step('after', wait1=w, wait2=w2, procs=[proc(0), proc(1)], words_a_first=a1,
         words_b_first=b1, words_a=user_words('user_a_data'),
         words_b=user_words('user_b_data'))


def probe_t9():
    hit = stop_at(SYM['trap_from_user'], trap_cond(FRAME_A, 8, a7=1001))
    step('sleep_ecall', hit=hit, cpu=cpu(), frame=frame(0), proc_a=proc(0), proc_b=proc(1))
    hit = stop_at(UVA['b_worker_loop'])
    step('B_running', hit=hit, cpu=cpu(), proc_a=proc(0), proc_b=proc(1), satps=satps())
    hit = stop_at(UVA['a_sleep_ret'])
    step('A_woke', hit=hit, cpu=cpu(), gprs=gprs(), proc_a=proc(0), proc_b=proc(1),
         sched=sched(), satps=satps(), timebase=ev('boot_configuration.timebase_hz'))


def probe_t9_all():
    hit = stop_at(SYM['sched_wait'])
    step('wait', hit=hit, cpu=cpu(), wake=reg('a0'), procs=[proc(0), proc(1)], sched=sched())
    a1, b1 = user_words('user_a_data'), user_words('user_b_data')
    clear()
    watch = [bp(SYM['sched_idle']), bp(SYM['proc_fault']), bp(SYM['kernel_panic'])]
    hit = run_to(*watch, wall=2.0)
    step('after', watch_hit=None if hit is None else hit.location, procs=[proc(0), proc(1)],
         sched=sched(), words_a_first=a1, words_b_first=b1,
         words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))


PROBES.update({'t8': probe_t8, 'n6': probe_n6, 't9': probe_t9, 't9_all': probe_t9_all})


# ------------------------------------------------------------ v1.2 probes
A_IN = 0x100            # user.S D_IN: received bytes in A's data page


def read_trap():
    return trap_cond(FRAME_A, 8, a7=63)


def watch_dispatch_a():
    return (('dispatch_A', SYM['sched_dispatch'], f'$a0 == {FRAME_A}'),)


def probe_t10():
    uart = Uart()
    hit = stop_at(SYM['trap_from_user'], read_trap())
    s = satps()
    fr = frame(0)
    step('read_ecall', hit=hit, cpu=cpu(), frame=fr, satps=s, proc_a=proc(0),
         walk_first=walk(s['A'], fr['x11']), walk_last=walk(s['A'], fr['x11'] + fr['x12'] - 1),
         words_b=user_words('user_b_data'), input=inp())
    w = wait_ticks(30, watch_dispatch_a())
    step('blocked', wait=w, proc_a=proc(0), proc_b=proc(1), sched=sched(), input=inp(),
         words_b=user_words('user_b_data'))
    for k, data in ((1, b'abc'), (2, b'XY')):
        uart.send(data)
        hit = stop_at(UVA['a_read_ret'])
        step(f'read_ret{k}', hit=hit, cpu=cpu(), gprs=gprs(), satps=satps(), proc_a=proc(0),
             sched=sched(), input=inp(), data=phys(SYM['user_a_data'] + A_IN, 16).hex(),
             words_a=user_words('user_a_data'))


def probe_t11():
    hit = stop_at(UVA['a_uptime_ret1'])
    step('ret1', hit=hit, cpu=cpu(), gprs=gprs(), boot_time=ev('boot_time'),
         timebase=ev('boot_configuration.timebase_hz'))
    hit = stop_at(UVA['a_uptime_ret2'])
    step('ret2', hit=hit, cpu=cpu(), gprs=gprs(), proc_a=proc(0), sched=sched())
    w = wait_ticks(20)
    step('after', wait=w, procs=[proc(0), proc(1)], words_a=user_words('user_a_data'),
         results=[phys_u64(SYM['user_a_data'] + 0x40 + 8 * k) for k in range(2)])


SHELL_SCRIPT = ['help', 'ps', 'uptime', 'echo hello from the test', 'attack', 'ps', 'uptime']


def shell_settle(uart, prompts, wall=20.0):
    """Let the guest run until the shell has printed `prompts` prompts and is
    back in read(); every stop is at A's read ecall, so no output is cut."""
    end = time.time() + wall
    while time.time() < end:
        clear()
        b = bp(SYM['trap_from_user'], read_trap())
        run_to(b, wall=1.0)
        if uart.text().count('proto> ') >= prompts:
            return True
    return False


def probe_t12():
    uart = Uart()
    ok = shell_settle(uart, 1)
    step('first_prompt', ok=ok, cpu=cpu(), proc_a=proc(0), input=inp())
    w = wait_ticks(50, watch_dispatch_a())
    step('idle_1', wait=w, proc_a=proc(0), proc_b=proc(1), sched=sched())
    for n, cmd in enumerate(SHELL_SCRIPT, start=2):
        uart.send(cmd.encode() + b'\r')
        ok = shell_settle(uart, n)
        step(f'cmd_{n - 1}', cmd=cmd, ok=ok, proc_a=proc(0), proc_b=proc(1), sched=sched(),
             input=inp())
    w = wait_ticks(50, watch_dispatch_a())
    step('idle_2', wait=w, proc_a=proc(0), proc_b=proc(1), sched=sched(), input=inp(),
         socket_text=uart.text())


def probe_n7():
    uart = Uart()
    hit = stop_at(UVA['a_read_bad_block'])
    s = satps()
    step('rejected', hit=hit, cpu=cpu(), satps=s, proc_a=proc(0), input=inp(),
         results=[phys_u64(SYM['user_a_data'] + 0x40 + 8 * k) for k in range(9)],
         walk_secret=walk(s['A'], SYM['kernel_secret']), walk_b_data=walk(s['A'], 0x801000),
         walk_text=walk(s['A'], UVA['a_read_bad']), walk_guard=walk(s['A'], 0x402000))
    hit = stop_at(SYM['trap_from_user'], read_trap())
    w = wait_ticks(20, watch_dispatch_a())
    step('blocked', hit=hit, wait=w, proc_a=proc(0), proc_b=proc(1))
    uart.send(b'z')
    hit = stop_at(UVA['a_read_bad_done'])
    step('done', hit=hit, cpu=cpu(), gprs=gprs(), proc_a=proc(0), input=inp(),
         result9=phys_u64(SYM['user_a_data'] + 0x40 + 72),
         data=phys(SYM['user_a_data'] + A_IN, 4).hex())
    w = wait_ticks(20)
    a1, b1 = user_words('user_a_data'), user_words('user_b_data')
    w2 = wait_ticks(20)
    step('after', wait1=w, wait2=w2, procs=[proc(0), proc(1)], words_a_first=a1,
         words_b_first=b1, words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))


FLOOD = bytes(65 + i % 26 for i in range(300))


def probe_n8():
    uart = Uart()
    hit = stop_at(SYM['trap_from_user'], trap_cond(FRAME_A, 8, a7=1001))
    step('sleep_ecall', hit=hit, frame=frame(0), input=inp())
    uart.send(FLOOD)
    hit = stop_at(UVA['a_flood_read2_ecall'])
    step('drained', hit=hit, cpu=cpu(), proc_a=proc(0), input=inp(), sched=sched(),
         result0=phys_u64(SYM['user_a_data'] + 0x40),
         data=phys(SYM['user_a_data'] + A_IN, 256).hex())
    hit = stop_at(SYM['trap_from_user'], read_trap())
    w = wait_ticks(10, watch_dispatch_a())
    step('blocked', hit=hit, wait=w, proc_a=proc(0))
    uart.send(b'Z')
    hit = stop_at(UVA['a_flood_done'])
    step('done', hit=hit, cpu=cpu(), proc_a=proc(0), procs=[proc(0), proc(1)], input=inp(),
         result1=phys_u64(SYM['user_a_data'] + 0x48),
         data=phys(SYM['user_a_data'] + A_IN, 256).hex())
    w = wait_ticks(20)
    b1 = user_words('user_b_data')
    w2 = wait_ticks(20)
    step('after', wait1=w, wait2=w2, procs=[proc(0), proc(1)], words_b_first=b1,
         words_a=user_words('user_a_data'), words_b=user_words('user_b_data'))


PROBES.update({'t10': probe_t10, 't11': probe_t11, 't12': probe_t12, 'n7': probe_n7,
               'n8': probe_n8})


# ------------------------------------------------------------ v1.3 probes
PROG_VA = 0x4000000
SLOT2_BASE = 0xC00000        # USER_SLOT_BASE(2)


def results(n):
    """A's v1.3 test results: words at its data page + 0x40."""
    return [phys_u64(SYM['user_a_data'] + 0x40 + 8 * k) for k in range(n)]


def frame_of(i):
    return f'((unsigned long)&procs[{i}])'


def page_summary(pa):
    """Physical page content: zero words, 0xa5 words, and words 2/3 (memcheck result/done)."""
    raw = phys(pa, 4096)
    w = [int.from_bytes(raw[k:k + 8], 'little') for k in range(0, 4096, 8)]
    return {'zero': sum(x == 0 for x in w), 'a5': sum(x == 0xa5a5a5a5a5a5a5a5 for x in w),
            'w0': w[0], 'w2': w[2], 'w3': w[3]}


def instance(tag):
    p = proc(2)
    return dict(proc=p, mem=mem_state(), bits=page_bits(p['pages']),
                data=page_summary(p['pages'][3]), stack=page_summary(p['pages'][4]),
                results=results(6))


def probe_t13():
    step('after1', hit=stop_at(UVA['a_mem_after1']), **instance(1))
    step('after2', hit=stop_at(UVA['a_mem_after2']), **instance(2))
    hit = stop_at(UVA['a_mem_done'])
    p = proc(2)
    step('done', hit=hit, proc=p, mem=mem_state(), results=results(6), sched=sched())


def probe_t14():
    hit = stop_at(UVA['a_spawn_done'])
    s = satps()
    p = proc(2)
    root = p['satp']
    step('spawned', hit=hit, satps=s, proc=p, mem=mem_state(), results=results(3),
         walk_data=walk(root, SLOT2_BASE + 0x1000), walk_stack=walk(root, SLOT2_BASE + 0x3000),
         walk_text=walk(root, SLOT2_BASE), walk_prog=walk(root, PROG_VA),
         walk_a_data=walk(root, 0x401000), walk_b_data=walk(root, 0x801000),
         walk_secret=walk(root, SYM['kernel_secret']), walk_uart=walk(root, 0x10000000),
         walk_pool_alias=walk(root, p['pages'][3]),
         a_sees_c=walk(s['A'], SYM_C_DATA), b_sees_c=walk(s['B'], SYM_C_DATA))
    hit = stop_at(UVA['p_worker_loop'])
    step('C_running', hit=hit, cpu=cpu(), gprs=gprs(), proc=proc(2),
         counter=phys_u64(p['pages'][3]), words_b=user_words('user_b_data'))
    w = wait_ticks(30)
    step('after', wait=w, proc=proc(2), procs=[proc(0), proc(1)],
         counter=phys_u64(p['pages'][3]), words_b=user_words('user_b_data'))


SYM_C_DATA = SLOT2_BASE + 0x1000


def probe_t15():
    hit = stop_at(UVA['a_kill_killed'])
    p = proc(2)
    step('killed', hit=hit, proc=p, mem=mem_state(), bits=page_bits(p['pages']),
         results=results(4), sched=sched())
    w = wait_ticks(15, (('dispatch_C', SYM['sched_dispatch'], f'$a0 == {frame_of(2)}'),))
    step('quiet', wait=w, proc=proc(2))
    hit = stop_at(UVA['a_kill_respawned'])
    q = proc(2)
    step('respawned', hit=hit, proc=q, mem=mem_state(), bits=page_bits(q['pages']),
         results=results(5))
    hit = stop_at(UVA['a_kill_done'])
    r = proc(2)
    step('done', hit=hit, proc=r, mem=mem_state(), bits=page_bits(r['pages']),
         results=results(8), sched=sched())


def probe_t16():
    hit = stop_at(SYM['proc_fault'])
    slot = (reg('a0') - ev('(unsigned long)&procs[0]')) // ev('sizeof(struct process)')
    step('fault', hit=hit, slot=slot, frame=frame(slot), proc=proc(slot), mem=mem_state(),
         b_data=user_words('user_b_data'))
    hit = stop_at(SYM['sched_dispatch'])
    nxt = (reg('a0') - ev('(unsigned long)&procs[0]')) // ev('sizeof(struct process)')
    step('next_dispatch', hit=hit, slot=nxt, states=[ev(f'procs[{i}].state') for i in range(8)])
    hit = stop_at(UVA['a_multi_done'])
    snap = [proc(i) for i in range(6)]
    counters = [phys_u64(p['pages'][3]) if p['npages'] else None for p in snap]
    step('done', hit=hit, procs=snap, counters=counters, mem=mem_state(), results=results(6),
         words_b=user_words('user_b_data'))
    w = wait_ticks(30, (('fault', SYM['proc_fault'], None),))
    snap2 = [proc(i) for i in range(6)]
    step('after', wait=w, procs=snap2,
         counters=[phys_u64(p['pages'][3]) if p['npages'] else None for p in snap2],
         words_b=user_words('user_b_data'), mem=mem_state())


def probe_n9():
    hit = stop_at(UVA['a_spawn_bad_done'], wall=120.0)
    step('done', hit=hit, results=results(12), mem=mem_state(), sched=sched(),
         procs=[proc(i) for i in range(8)])


T17_SCRIPT = ['mem', 'run worker', 'run hello', 'ps', 'kill 3', 'run crash', 'ps', 'mem']


def probe_t17():
    uart = Uart()
    ok = shell_settle(uart, 1)
    step('first_prompt', ok=ok, mem=mem_state())
    for n, cmd in enumerate(T17_SCRIPT, start=2):
        uart.send(cmd.encode() + b'\r')
        ok = shell_settle(uart, n)
        w = wait_ticks(20)          # let run-time processes print and end
        step(f'cmd_{n - 1}', cmd=cmd, ok=ok, wait=w, mem=mem_state(),
             procs=[proc(i) for i in range(8)])
    step('end', procs=[proc(i) for i in range(8)], mem=mem_state(), sched=sched(),
         input=inp(), socket_text=uart.text())


PROBES.update({'t13': probe_t13, 't14': probe_t14, 't15': probe_t15, 't16': probe_t16,
               'n9': probe_n9, 't17': probe_t17})


# ------------------------------------------------------------ v1.4 probes
ELF_BASE = 0x40000000
USYM = CFG.get('user_symbols', {})      # program -> {symbol: user VA}
D_R14 = 0x400                           # user.S: v1.4 results in A's data page
D_NAME = 0x340
ANY_READ = '*(unsigned long *)($a0 + 272) == 8 && *(unsigned long *)($a0 + 136) == 63'
OTHER_READ = f'$a0 != {FRAME_A} && ' + ANY_READ


def results14(n):
    raw = phys(SYM['user_a_data'] + D_R14, 8 * n)
    return [int.from_bytes(raw[8 * k:8 * k + 8], 'little') for k in range(n)]


def proc14(i):
    """proc() plus every owned page and the v1.4 fields."""
    p = proc(i)
    p['pages'] = [ev(f'procs[{i}].pages[{k}]') for k in range(min(p['npages'], 64))]
    for k in ('heap_base', 'brk', 'wait_pid', 'waits_done'):
        p[k] = ev(f'procs[{i}].{k}')
    return p


def procs14():
    return [proc14(i) for i in range(8)]


def mem14():
    m = mem_state()
    m['reserved'] = ev('mem.reserved')
    return m


def sched14():
    s = sched()
    for k in ('loaded', 'load_errors', 'last_load_error', 'fg_pid', 'fg_waiter', 'interrupted'):
        s[k] = ev(f'sched.{k}')
    return s


def inp14():
    i = inp()
    for k in ('intr_armed', 'intr_pending', 'intrs'):
        i[k] = ev(f'input.{k}')
    return i


def archive_info():
    a = {k: ev(f'boot_configuration.initrd_{k}') for k in ('start', 'end', 'found')}
    a['pa'] = ev('prog_archive_pa')
    a['count'] = ev('prog_archive.count')
    return a


def archive_bits(a):
    """Bitmap bits of every page the boot loader's archive occupies."""
    if not a['found']:
        return []
    return page_bits(list(range(a['start'] & ~0xfff, (a['end'] + 0xfff) & ~0xfff, 4096)))


def page_digest(pa):
    import hashlib
    return hashlib.sha256(phys(pa & ~0xfff, 4096)).hexdigest()


def window(root, first, last):
    """Independent walks of every page in [first, last) of a user root."""
    return {hex(va): walk(root, va) for va in range(first, last, 4096)}


def root_pa(satp):
    return (satp & ((1 << 44) - 1)) << 12


def table(pa):
    raw = phys(pa, 4096)
    return [int.from_bytes(raw[8 * i:8 * i + 8], 'little') for i in range(512)]


def window_tables(satp):
    """Independent read of the WHOLE ELF window of a root: the root entry
    for the 1 GiB at 0x40000000 (VPN2 = 1), all 512 entries of its level-1
    table and, through entry 0, all 512 entries of the level-0 table of the
    2 MiB window (absent levels are omitted)."""
    out = {'root_entry': phys_u64(root_pa(satp) + 8)}
    e = out['root_entry']
    if e & 1 and not e & 0xe:
        out['l1'] = table((e >> 10) << 12)
        e0 = out['l1'][0]
        if e0 & 1 and not e0 & 0xe:
            out['l0'] = table((e0 >> 10) << 12)
    return out


def probe_t18():
    hit = stop_at(USYM['t_elf']['t_elf_checkpoint'])
    p = proc14(2)
    root, s, a = p['satp'], satps(), archive_info()
    tables = window_tables(root)
    contents = {hex(ELF_BASE + 4096 * i): page_digest((pte >> 10) << 12)
                for i, pte in enumerate(tables.get('l0', [])) if pte & 1}
    step('checkpoint', hit=hit, cpu=cpu(), gprs=gprs(), proc=p, mem=mem14(), sched=sched14(),
         bits=page_bits(p['pages']), satps=s, tables=tables, contents=contents, archive=a,
         archive_bits=archive_bits(a), walk_a_data=walk(root, 0x401000),
         walk_b_data=walk(root, 0x801000), walk_secret=walk(root, SYM['kernel_secret']),
         walk_uart=walk(root, 0x10000000), walk_prog=walk(root, PROG_VA),
         walk_archive=walk(root, a['start']), walk_pool_alias=walk(root, p['pages'][-1]),
         other_roots={k: phys_u64(root_pa(v) + 8) for k, v in s.items()})
    hit = stop_at(UVA['p_t_elf_done'])
    step('done', hit=hit, results=results14(11), mem=mem14(), sched=sched14(), proc=proc14(2),
         progs_name=phys(SYM['user_a_data'] + D_NAME, 16).hex())


def probe_t19():
    sy = USYM['t_sbrk']
    hit = stop_at(sy['t_sbrk_checkpoint1'])
    p = proc14(2)
    base = p['heap_base']
    step('cp1', hit=hit, cpu=cpu(), gprs=gprs(), proc=p, mem=mem14(),
         heap=window(p['satp'], base, base + 5 * 4096),
         heap_bytes=phys(walk(p['satp'], base)['pa'], 4096).hex()
         + phys(walk(p['satp'], base + 4096)['pa'], 4096).hex()
         + phys(walk(p['satp'], base + 8192)['pa'], 4096).hex())
    hit = stop_at(sy['t_sbrk_checkpoint2'])
    p = proc14(2)
    brk, steps_ = reg('a0'), reg('a1')
    top = (brk + 0xfff) & ~0xfff
    step('cp2', hit=hit, cpu=cpu(), brk=brk, steps=steps_, proc=p, mem=mem14(),
         bits=page_bits(p['pages']), heap=window(p['satp'], base, top + 4096),
         guard=walk(p['satp'], 0x401fb000), stack=window(p['satp'], 0x401fc000, 0x40200000))
    hit = stop_at(UVA['p_t_sbrk_done'])
    step('done', hit=hit, results=results14(4), mem=mem14(), proc=proc14(2), sched=sched14())


def probe_t20():
    hit = stop_at(UVA['p_t_wait_block'])
    hit2 = stop_at(SYM['trap_from_user'], trap_cond(FRAME_A, 8, a7=1009))
    step('wait_ecall', hit=hit and hit2, frame=frame(0), proc_a=proc14(0), sched=sched14(),
         results=results14(15), procs=procs14())
    w = wait_ticks(10, watch_dispatch_a())
    step('blocked', wait=w, proc_a=proc14(0), procs=procs14(), sched=sched14())
    hit = stop_at(UVA['p_t_wait_ret'])
    step('returned', hit=hit, cpu=cpu(), proc_a=proc14(0), results=results14(16),
         procs=procs14(), sched=sched14())
    hit = stop_at(UVA['p_t_wait_done'])
    step('done', hit=hit, results=results14(17), mem=mem14(), sched=sched14(), procs=procs14())


def probe_t21():
    uart = Uart()
    hit = stop_at(SYM['trap_from_user'], trap_cond(FRAME_A, 8, a7=1009))
    step('fg1_ecall', hit=hit, frame=frame(0), results=results14(4), sched=sched14(),
         input=inp14())
    # t_fgbusy sleeps 200 ms, is refused a second foreground wait and exits
    w = wait_ticks(60, watch_dispatch_a())
    step('fg1_armed', wait=w, procs=procs14(), sched=sched14(), input=inp14(), cpu=cpu())
    # Type Ctrl-C while the foreground t_spin itself is executing (audit): stop
    # in it under its own root, deliver the byte while the CPU is held, then
    # observe the kernel at console_intr().
    fg = ev('sched.fg_pid')
    slot = [i for i in range(8) if ev(f'procs[{i}].pid') == fg][0]
    fg_satp = ev(f'procs[{slot}].satp')
    hit = stop_at(USYM['t_spin']['t_spin_step'], f'$satp == {fg_satp}')
    step('fg_running', hit=hit, cpu=cpu(), slot=slot, fg_satp=fg_satp,
         state=ev(f'procs[{slot}].state'))
    uart.send(b'\x03')
    time.sleep(0.5)
    hit = stop_at(SYM['console_intr'])
    step('intr', hit=hit, cpu=cpu(), current=ev('(unsigned long)current'),
         fg_frame=ev(f'(unsigned long)&procs[{slot}]'), fg_state=ev(f'procs[{slot}].state'),
         fg_scause=ev(f'procs[{slot}].frame.scause'), fg_sepc=ev(f'procs[{slot}].frame.sepc'),
         input=inp14(), sched=sched14())
    hit = stop_at(UVA['p_t_ctrlc_after1'])
    step('after1', hit=hit, results=results14(5), procs=procs14(), sched=sched14(),
         input=inp14(), mem=mem14())
    hit = stop_at(UVA['p_t_ctrlc_fg2']) and stop_at(SYM['trap_from_user'],
                                                    trap_cond(FRAME_A, 8, a7=1009))
    step('fg2_ecall', hit=hit, frame=frame(0), results=results14(7), sched=sched14())
    hit = stop_at(SYM['trap_from_user'], OTHER_READ)
    step('reader_reads', hit=hit, text=uart.text(), sched=sched14())
    uart.send(b'abc\r')
    for _ in range(20):
        stop_at(SYM['trap_from_user'], OTHER_READ, wall=2.0)
        if 'got: abc' in uart.text():
            break
    # it answered and is back in read(): BLOCKED, nothing more to read
    w = wait_ticks(5, (('reader_read', SYM['trap_from_user'], OTHER_READ),))
    step('reader_answered', ok='got: abc' in uart.text(), wait=w, procs=procs14(),
         sched=sched14(), input=inp14())
    uart.send(b'\x03')
    hit = stop_at(UVA['p_t_ctrlc_plain'])
    step('after2', hit=hit, results=results14(8), procs=procs14(), sched=sched14(),
         input=inp14(), mem=mem14())
    hit = stop_at(SYM['trap_from_user'], read_trap())
    step('plain_read', hit=hit, frame=frame(0), sched=sched14(), input=inp14())
    uart.send(b'\x03')
    hit = stop_at(UVA['p_t_ctrlc_done'])
    step('done', hit=hit, results=results14(12), data=phys(SYM['user_a_data'] + A_IN, 4).hex(),
         procs=procs14(), sched=sched14(), input=inp14(), mem=mem14())
    w = wait_ticks(10)
    step('end', wait=w, text=uart.text(), procs=procs14(), words_b=user_words('user_b_data'))


def probe_t22():
    uart = Uart()
    prompts = [1]

    def run_until(pred, wall=30.0):
        """Run the guest in steps of 10 timer ticks (or up to the next read
        ecall) until pred(UART text) holds; breakpoints only, no SIGINT."""
        end = time.time() + wall
        while time.time() < end:
            if pred(uart.text()):
                return True
            target = ev('sched.timer_ticks') + 10
            clear()
            run_to(bp(SYM['trap_from_user'], f'({ANY_READ}) || sched.timer_ticks >= {target}'),
                   wall=10.0)
        return pred(uart.text())

    def command(text, settle=True, name=None):
        uart.send(text.encode() + b'\r')
        prompts[0] += 1
        ok = shell_settle(uart, prompts[0]) if settle else None
        step(name or f'cmd {text}', cmd=text, ok=ok, mem=mem14(), procs=procs14(),
             sched=sched14())
        return ok

    ok = shell_settle(uart, 1)
    step('first_prompt', ok=ok, mem=mem14(), archive=archive_info(),
         archive_bits=archive_bits(archive_info()))
    command('ls')
    command('run primes')
    # The first guess is typed ahead together with the command (audit): the
    # shell must leave "50\r" in the kernel buffer for the game.
    uart.send(b'run guess\r50\r')
    prompts[0] += 1
    lo, hi, guesses, answers = 1, 100, [], []
    for k in range(1, 9):
        if not run_until(lambda t, k=k: t.count('Your guess: ') >= k):
            break
        g = (lo + hi) // 2
        guesses.append(g)
        if k > 1:
            uart.send(f'{g}\r'.encode())
        got = []

        def answered(t, k=k):
            tail = t.split('Your guess: ')[k] if t.count('Your guess: ') >= k else ''
            got[:] = [w for w in ('Too small.', 'Too big.', 'Correct!') if w in tail]
            return bool(got)
        if not run_until(answered):
            break
        answers.append(got[0])
        if got[0] == 'Correct!':
            break
        lo, hi = (g + 1, hi) if got[0] == 'Too small.' else (lo, g - 1)
    ok = shell_settle(uart, prompts[0])
    step('guess', ok=ok, guesses=guesses, answers=answers, procs=procs14(), mem=mem14())
    uart.send(b'run clock\r')
    prompts[0] += 1
    ticks = run_until(lambda t: t.split('run clock')[-1].count('uptime ') >= 3, wall=30.0)
    step('clock_running', ok=ticks, procs=procs14(), sched=sched14(), input=inp14())
    uart.send(b'\x03')
    ok = shell_settle(uart, prompts[0])
    step('clock_stopped', ok=ok, procs=procs14(), sched=sched14(), input=inp14(), mem=mem14())
    command('run clock &')
    run_until(lambda t: t.split('run clock &')[-1].count('uptime ') >= 1)
    command('ps')
    started = [int(x) for x in __import__('re').findall(r'started pid (\d+)', uart.text())]
    command(f'kill {started[-1] if started else 0}', name='cmd kill')
    command('run nosuch')
    command('mem')
    w = wait_ticks(30, watch_dispatch_a())
    step('end', wait=w, procs=procs14(), mem=mem14(), sched=sched14(), input=inp14(),
         socket_text=uart.text())


def probe_n10():
    hit = stop_at(UVA['p_t_elfbad'])
    m0, s0 = mem14(), sched14()
    hit = stop_at(UVA['p_t_elfbad_refused'], wall=120.0) and hit
    step('refused', hit=hit, mem_before=m0, sched_before=s0, mem=mem14(), sched=sched14(),
         results=results14(32))
    hit = stop_at(UVA['p_t_elfbad_done'], wall=120.0)
    step('done', hit=hit, results=results14(72), mem=mem14(), sched=sched14(), procs=procs14())


def probe_n11():
    hit = stop_at(UVA['p_t_arcbad_done'])
    a = archive_info()
    s = satps()
    step('done', hit=hit, results=results14(4), mem=mem14(), sched=sched14(), archive=a,
         archive_bits=archive_bits(a), walk_a_archive=walk(s['A'], a['start']),
         words_b=user_words('user_b_data'))
    w = wait_ticks(10)
    step('after', wait=w, words_b=user_words('user_b_data'), procs=[proc(0), proc(1)])


def probe_n12():
    hit = stop_at(UVA['p_t_userbad_done'], wall=120.0)
    step('done', hit=hit, results=results14(41), mem=mem14(), sched=sched14(), procs=procs14(),
         words_b=user_words('user_b_data'))
    w = wait_ticks(10)
    step('after', wait=w, words_b=user_words('user_b_data'), procs=[proc(0), proc(1)])


PROBES.update({'t18': probe_t18, 't19': probe_t19, 't20': probe_t20, 't21': probe_t21,
               't22': probe_t22, 'n10': probe_n10, 'n11': probe_n11, 'n12': probe_n12})


def main():
    gdb.execute('set pagination off')
    gdb.execute('set confirm off')
    gdb.execute('set stack-cache off')
    gdb.execute('set code-cache off')
    gdb.execute('set architecture riscv:rv64')
    gdb.execute(f'file {CFG["elf"]}')
    gdb.execute('set tcp connect-timeout 15')   # retries while QEMU starts listening
    gdb.execute(f'target remote 127.0.0.1:{CFG["port"]}')
    try:
        PROBES[CFG['probe']]()
        obs['complete'] = True
    except Exception as e:   # recorded, never converted into a PASS
        obs['error'] = f'{type(e).__name__}: {e}'
    flush()
    try:
        gdb.execute('kill')
    except gdb.error:
        pass


main()
