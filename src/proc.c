#include "kernel.h"
#include "proc.h"
#include "sbi.h"
#include "sv39.h"

struct process procs[PROC_COUNT];
struct process *current;
struct sched_stats sched;

static const char proc_name[PROC_COUNT] = { 'A', 'B' };
#define HEARTBEAT_TICKS 100u

static const char *state_name(uint64_t s)
{
    switch (s) {
    case PROC_READY: return "READY";
    case PROC_RUNNING: return "RUNNING";
    case PROC_EXITED: return "EXITED";
    case PROC_FAULTED: return "FAULTED";
    case PROC_SLEEPING: return "SLEEPING";
    default: return "UNUSED";
    }
}

static void log_proc(const struct process *p)
{
    uart_putc(proc_name[p - procs]);
    uart_puts("(pid="); uart_dec(p->pid); uart_puts(")");
}

/* Return to U-mode with interrupts taken in U, no SUM/MXR, FP/V off so any
 * FP/vector instruction in user code traps as illegal (no FP state kept). */
static uint64_t user_sstatus(uint64_t s)
{
    return (s & ~(SSTATUS_SPP | SSTATUS_SIE | SSTATUS_SUM | SSTATUS_MXR |
                  SSTATUS_FS | SSTATUS_VS | SSTATUS_UBE)) | SSTATUS_SPIE;
}

static void timer_set(uint64_t deadline)
{
    struct sbiret r = sbi_call1(SBI_EXT_TIME, SBI_TIME_SET_TIMER, deadline);
    if (r.error) kernel_panic("SBI TIME set_timer failed");
}

static void timer_arm(void)
{
    timer_set(csr_read(time) + sched.quantum_ticks);
}

/* Terminal state: nothing runnable or sleeping. Never loads a process context again. */
__attribute__((noinline)) _Noreturn void sched_idle(void)
{
    csr_clear(sie, SIE_STIE);
    sbi_call1(SBI_EXT_TIME, SBI_TIME_SET_TIMER, UINT64_MAX);
    vm_switch(vm_kernel_satp);
    current = 0;
    sched.idle_entered = 1;
    uart_puts("IDLE no runnable processes:");
    for (unsigned i = 0; i < PROC_COUNT; ++i) {
        uart_puts(" "); log_proc(&procs[i]);
        uart_puts("="); uart_puts(state_name(procs[i].state));
    }
    uart_puts("\n");
    halt();
}

/* Every live process is SLEEPING: wait in S-mode until the earliest wake-up.
 * sstatus.SIE stays 0, so no trap is taken; wfi returns once the armed S
 * timer interrupt is pending (sie.STIE is set). */
__attribute__((noinline)) void sched_wait(uint64_t wake)
{
    sched.waits++;
    timer_set(wake);
    while (csr_read(time) < wake)
        __asm__ volatile("wfi");
}

__attribute__((noinline)) struct trap_frame *sched_dispatch(struct process *next,
    const char *reason)
{
    struct process *prev = current;
    if (next != prev) {
        sched.switches++;
        if (sched.verbose) {
            uart_puts("SCHED ");
            if (prev) log_proc(prev); else uart_puts("-");
            uart_puts(" -> "); log_proc(next);
            uart_puts(" reason="); uart_puts(reason);
            uart_puts(" switches="); uart_dec(sched.switches);
            uart_puts("\n");
        }
        vm_switch(next->satp);
    }
    current = next;
    next->state = PROC_RUNNING;
    next->dispatches++;
    next->last_dispatch_time = csr_read(time);
    next->frame.sstatus = user_sstatus(csr_read(sstatus));
    return &next->frame;
}

/* Round Robin: wake due sleepers, then run the first READY process after the
 * current one; if everyone alive sleeps, wait for the earliest wake-up. */
static struct trap_frame *schedule(const char *reason)
{
    for (;;) {
        uint64_t now = csr_read(time), next_wake = UINT64_MAX;
        for (unsigned i = 0; i < PROC_COUNT; ++i) {
            struct process *p = &procs[i];
            if (p->state != PROC_SLEEPING) continue;
            if (p->wake_time <= now) p->state = PROC_READY;
            else if (p->wake_time < next_wake) next_wake = p->wake_time;
        }
        size_t start = current ? (size_t)(current - procs) : PROC_COUNT - 1;
        for (size_t k = 1; k <= PROC_COUNT; ++k) {
            struct process *p = &procs[(start + k) % PROC_COUNT];
            if (p->state == PROC_READY) return sched_dispatch(p, reason);
        }
        if (next_wake == UINT64_MAX) sched_idle();
        sched_wait(next_wake);
        timer_arm();            /* fresh quantum for whoever wakes up */
        reason = "wake";
    }
}

/* Copy a validated user buffer to the console. The range check walks the
 * caller's own page table first, so the SUM window never faults. */
static uint64_t sys_write(struct process *p, uint64_t fd, uint64_t va, uint64_t len)
{
    if (fd != 1) return SYS_EBADF;
    if (len > WRITE_MAX) return SYS_EINVAL;
    if (!vm_user_range_ok((unsigned)(p - procs), va, len, SV39_PTE_R)) return SYS_EFAULT;
    csr_set(sstatus, SSTATUS_SUM);
    for (uint64_t i = 0; i < len; ++i) {
        char c = *(volatile const char *)(uintptr_t)(va + i);
        if (c == '\n') uart_putc('\r');
        uart_putc((c == '\n' || c == '\t' || (c >= 0x20 && c <= 0x7e)) ? c : '?');
    }
    csr_clear(sstatus, SSTATUS_SUM);
    p->writes++;
    p->bytes_written += len;
    return len;
}

__attribute__((noinline)) struct trap_frame *proc_syscall(struct process *p,
    struct trap_frame *f)
{
    uint64_t nr = f->x[17];
    f->sepc += 4;       /* resume after ecall (always a 32-bit encoding) */
    p->syscalls++;
    switch (nr) {
    case SYS_GETPID:
        f->x[10] = p->pid;
        return f;
    case SYS_WRITE:
        f->x[10] = sys_write(p, f->x[10], f->x[11], f->x[12]);
        return f;
    case SYS_YIELD:
        p->yields++;
        f->x[10] = 0;
        p->state = PROC_READY;
        return schedule("yield");
    case SYS_SLEEP: {
        uint64_t ms = f->x[10];
        if (ms > SLEEP_MAX_MS) { f->x[10] = SYS_EINVAL; return f; }
        f->x[10] = 0;
        p->sleeps++;
        p->sleep_start = csr_read(time);
        p->wake_time = p->sleep_start + ms * boot_configuration.timebase_hz / 1000u;
        p->state = ms ? PROC_SLEEPING : PROC_READY;
        return schedule("sleep");
    }
    case SYS_EXIT:
        p->exit_code = f->x[10];
        p->state = PROC_EXITED;
        uart_puts("EXIT "); log_proc(p);
        uart_puts(" code="); uart_dec(p->exit_code); uart_puts("\n");
        return schedule("exit");
    default:
        /* Documented policy: unknown numbers return -ENOSYS, process lives. */
        p->rejected_syscalls++;
        f->x[10] = SYS_ENOSYS;
        uart_puts("SYSCALL_REJECTED "); log_proc(p);
        uart_puts(" a7="); uart_dec(nr); uart_puts(" a0=-38\n");
        return f;
    }
}

/* Any synchronous exception from U-mode other than ecall (page/access
 * fault, illegal instruction, misalignment, breakpoint) kills only the
 * offending process. */
__attribute__((noinline)) struct trap_frame *proc_fault(struct process *p,
    struct trap_frame *f)
{
    p->state = PROC_FAULTED;
    p->fault_cause = f->scause;
    p->fault_epc = f->sepc;
    p->fault_tval = f->stval;
    uart_puts("FAULT "); log_proc(p);
    uart_puts(" scause="); uart_hex(f->scause);
    uart_puts(" sepc="); uart_hex(f->sepc);
    uart_puts(" stval="); uart_hex(f->stval);
    uart_puts(" -> FAULTED\n");
    return schedule("fault");
}

static void heartbeat(void)
{
    uart_puts("HEARTBEAT ticks="); uart_dec(sched.timer_ticks);
    uart_puts(" switches="); uart_dec(sched.switches);
    for (unsigned i = 0; i < PROC_COUNT; ++i) {
        uart_puts(" "); log_proc(&procs[i]);
        uart_puts(".preempt="); uart_dec(procs[i].preemptions);
    }
    uart_puts("\n");
}

struct trap_frame *trap_from_user(struct trap_frame *f)
{
    struct process *p = current;
    if (!p || f != &p->frame || p->state != PROC_RUNNING)
        kernel_panic("trap frame does not belong to the running process");
    uint64_t cause = f->scause;
    if (cause & SCAUSE_INTERRUPT) {
        if ((cause & ~SCAUSE_INTERRUPT) != IRQ_S_TIMER)
            kernel_panic("unexpected interrupt from U-mode");
        sched.timer_ticks++;
        p->preemptions++;
        timer_arm();
        p->state = PROC_READY;
        if (sched.heartbeat && sched.timer_ticks % HEARTBEAT_TICKS == 0) heartbeat();
        return schedule("timer");
    }
    if (cause == EXC_U_ECALL) return proc_syscall(p, f);
    return proc_fault(p, f);
}

_Noreturn void proc_start(uint64_t entry_a, uint64_t entry_b, int verbose, int heartbeat_on)
{
    static const uint64_t bases[PROC_COUNT] = { USER_A_BASE, USER_B_BASE };
    const uint64_t entries[PROC_COUNT] = { entry_a, entry_b };
    /* 10 ms quantum from the DT timebase (QEMU virt: 10 MHz). */
    sched.quantum_ticks = boot_configuration.timebase_hz / 100u;
    sched.verbose = (uint64_t)verbose;
    sched.heartbeat = (uint64_t)heartbeat_on;
    for (unsigned i = 0; i < PROC_COUNT; ++i) {
        struct process *p = &procs[i];
        p->pid = i + 1;
        p->satp = vm_proc_satp[i];
        p->frame.sepc = entries[i];
        p->frame.x[2] = bases[i] + USER_STACK_TOP_OFF;
        p->state = PROC_READY;
        if (verbose || heartbeat_on) {
            uart_puts("PROC "); log_proc(p);
            uart_puts(" entry="); uart_hex(p->frame.sepc);
            uart_puts(" sp="); uart_hex(p->frame.x[2]);
            uart_puts(" satp="); uart_hex(p->satp); uart_puts("\n");
        }
    }
    csr_write(scounteren, 0);
    timer_arm();
    csr_set(sie, SIE_STIE);
    user_return(schedule("start"));
}
