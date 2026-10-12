"""ProtoOS acceptance probe, executed INSIDE gdb-multiarch (gdb Python API).

Observation only: the probe sets breakpoints and reads registers, CSRs,
virtual and physical memory. It never writes guest memory or registers and
never performs any kernel function (scheduling, ecall handling, MMU setup).

Configuration comes from the JSON file named by PROTOOS_PROBE_CFG:
  probe         probe name (see PROBES)
  symbols       name -> address (kernel symbols are physical = virtual)
  user_va       name -> user virtual address of user.S labels
  observations  output JSON path (rewritten after every step)
"""
import gdb
import json
import os
import signal
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
        'fault_epc', 'fault_tval')}
    out['addr'] = ev(f'(unsigned long)&procs[{i}]')
    return out


def sched():
    return {k: ev(f'sched.{k}') for k in (
        'quantum_ticks', 'timer_ticks', 'switches', 'idle_entered', 'verbose')}


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
