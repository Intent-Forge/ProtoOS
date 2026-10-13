#include "kernel.h"
#include "elf.h"
#include "input.h"
#include "mem.h"
#include "proc.h"
#include "sbi.h"
#include "sv39.h"

struct process procs[PROC_MAX];
struct process *current;
struct sched_stats sched;
struct archive prog_archive;
uint64_t prog_archive_pa;

#define HEARTBEAT_TICKS 100u

/* v1.3 built-in programs in the shared program pages (src/user.S). */
extern char __user_prog_text[];
extern char p_worker[], p_hello[], p_counter[], p_crash[], p_memcheck[];
static char *const programs[PROG_IDS] = {
    [PROG_WORKER] = p_worker, [PROG_HELLO] = p_hello, [PROG_COUNTER] = p_counter,
    [PROG_CRASH] = p_crash, [PROG_MEMCHECK] = p_memcheck,
};

static const char *state_name(uint64_t s)
{
    switch (s) {
    case PROC_READY: return "READY";
    case PROC_RUNNING: return "RUNNING";
    case PROC_EXITED: return "EXITED";
    case PROC_FAULTED: return "FAULTED";
    case PROC_SLEEPING: return "SLEEPING";
    case PROC_BLOCKED: return "BLOCKED";
    default: return "UNUSED";
    }
}

static void log_proc(const struct process *p)
{
    uart_putc((char)('A' + (p - procs)));      /* slot letter: A, B, C, ... */
    uart_puts("(pid="); uart_dec(p->pid); uart_puts(")");
}

static int alive(const struct process *p)
{
    return p->state == PROC_READY || p->state == PROC_RUNNING ||
           p->state == PROC_SLEEPING || p->state == PROC_BLOCKED;
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

/* Terminal state: nothing runnable, sleeping or waiting for input. Never
 * loads a process context again. */
__attribute__((noinline)) _Noreturn void sched_idle(void)
{
    csr_clear(sie, SIE_STIE | SIE_SEIE);
    sbi_call1(SBI_EXT_TIME, SBI_TIME_SET_TIMER, UINT64_MAX);
    vm_switch(vm_kernel_satp);
    current = 0;
    sched.idle_entered = 1;
    uart_puts("IDLE no runnable processes:");
    for (unsigned i = 0; i < PROC_MAX; ++i) {
        if (procs[i].state == PROC_UNUSED) continue;
        uart_puts(" "); log_proc(&procs[i]);
        uart_puts("="); uart_puts(state_name(procs[i].state));
    }
    uart_puts("\n");
    halt();
}

static int reader_blocked(void)
{
    for (unsigned i = 0; i < PROC_MAX; ++i)
        if (procs[i].state == PROC_BLOCKED && procs[i].read_pending) return 1;
    return 0;
}

static struct process *find_pid(uint64_t pid)
{
    for (unsigned i = 0; i < PROC_MAX; ++i)
        if (procs[i].state != PROC_UNUSED && procs[i].pid == pid) return &procs[i];
    return 0;
}

/* v1.4: what wait() reports for an ended process. */
static uint64_t wait_status(const struct process *q)
{
    return q->state == PROC_FAULTED ? FAULT_STATUS : (q->exit_code & 0xffu);
}

/* v1.4: q has just ended (EXITED or FAULTED). Processes waiting for it
 * become READY with its status; if it was the console foreground process
 * or the one waiting for it, Ctrl-C is disarmed. */
static void ended(const struct process *q)
{
    for (unsigned i = 0; i < PROC_MAX; ++i) {
        struct process *w = &procs[i];
        if (w->state == PROC_BLOCKED && !w->read_pending && w->wait_pid == q->pid) {
            w->frame.x[10] = wait_status(q);
            w->wait_pid = 0;
            w->waits_done++;
            w->state = PROC_READY;
        }
    }
    if (sched.fg_pid && (sched.fg_pid == q->pid || sched.fg_waiter == q->pid)) {
        sched.fg_pid = 0;
        sched.fg_waiter = 0;
        input.intr_armed = 0;
    }
}

static void reclaim(struct process *p);

/* v1.4: Ctrl-C arrived while a foreground wait() was armed. End the
 * foreground process like kill() (exit code 130). 1 if a process ended. */
static int console_intr(void)
{
    input.intr_pending = 0;
    struct process *q = find_pid(sched.fg_pid);
    if (!sched.fg_pid || !q || !alive(q) || q - procs < (ptrdiff_t)PROC_STATIC) return 0;
    q->state = PROC_EXITED;
    q->exit_code = INTR_EXIT_CODE;
    q->killed = 1;
    sched.interrupted++;
    if (sched.verbose) {        /* like KILL; the shell prints ^C itself */
        uart_puts("INTR "); log_proc(q); uart_puts(" ended by Ctrl-C\n");
    }
    reclaim(q);
    ended(q);
    return 1;
}

/* Every live process is SLEEPING or BLOCKED on input: wait in S-mode until
 * the earliest wake-up (UINT64_MAX: none, the timer stays disarmed) or
 * until input arrives for a blocked reader. sstatus.SIE stays 0, so no trap
 * is taken; wfi returns once an enabled interrupt (sie.STIE/SEIE) is
 * pending, and a pending external interrupt is serviced here by polling. */
__attribute__((noinline)) void sched_wait(uint64_t wake)
{
    sched.waits++;
    timer_set(wake);
    while (csr_read(time) < wake) {
        if (csr_read(sip) & SIP_SEIP) {
            input_irq();
            if (input.intr_pending && console_intr()) return;  /* v1.4: a waiter woke */
            if (input.count && reader_blocked()) return;
        }
        __asm__ volatile("wfi");
    }
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
    /* A reader is dispatched only with input waiting; its buffer lives in
     * its own address space, which is installed now. */
    if (next->read_pending) {
        uint64_t n = input_take_user(next->read_va, next->read_len);
        next->frame.x[10] = n;
        next->read_pending = 0;
        next->reads++;
        next->bytes_read += n;
    }
    current = next;
    sched.last_slot = (uint64_t)(next - procs);
    next->state = PROC_RUNNING;
    next->dispatches++;
    next->last_dispatch_time = csr_read(time);
    next->frame.sstatus = user_sstatus(csr_read(sstatus));
    return &next->frame;
}

/* Round Robin: wake due sleepers and readers with input waiting, then run
 * the first READY process after the current one; if everyone alive sleeps
 * or waits for input, wait in the kernel. */
static struct trap_frame *schedule(const char *reason)
{
    for (;;) {
        uint64_t now = csr_read(time), next_wake = UINT64_MAX;
        int blocked = 0;
        for (unsigned i = 0; i < PROC_MAX; ++i) {
            struct process *p = &procs[i];
            if (p->state == PROC_BLOCKED) {
                /* v1.4: only readers wake on input; waiters wake in ended() */
                if (p->read_pending && input.count) p->state = PROC_READY;
                else blocked = 1;
            }
            if (p->state != PROC_SLEEPING) continue;
            if (p->wake_time <= now) p->state = PROC_READY;
            else if (p->wake_time < next_wake) next_wake = p->wake_time;
        }
        /* v1.3.1: continue after the last dispatched slot even if that
         * process has just ended and `current` was cleared by reclaim(). */
        size_t start = (size_t)sched.last_slot;
        for (size_t k = 1; k <= PROC_MAX; ++k) {
            struct process *p = &procs[(start + k) % PROC_MAX];
            if (p->state != PROC_READY) continue;
            /* Another reader took the input first: wait again. */
            if (p->read_pending && !input.count) { p->state = PROC_BLOCKED; blocked = 1; continue; }
            return sched_dispatch(p, reason);
        }
        if (next_wake == UINT64_MAX && !blocked) sched_idle();
        sched_wait(next_wake);
        timer_arm();            /* fresh quantum for whoever wakes up */
        reason = "wake";
    }
}

/* v1.3: give back every page of a run-time process that has ended. Its
 * root may be the one installed (exit, fault): switch to the kernel root
 * first, so no freed table is ever in use. */
static void reclaim(struct process *p)
{
    if (!p->npages) return;
    if (p == current || csr_read(satp) == p->satp) {
        vm_switch(vm_kernel_satp);
        current = 0;
    }
    vm_destroy(p->pages, p->npages);
    p->read_pending = 0;
    if (sched.verbose) {
        uart_puts("RECLAIM "); log_proc(p);
        uart_puts(" pages="); uart_dec(p->npages);
        uart_puts(" free="); uart_dec(mem.free); uart_puts("\n");
    }
    p->npages = 0;
    sched.reclaimed++;
}

/* Copy a validated user buffer to the console. The range check walks the
 * caller's own page table first, so the SUM window never faults. */
static uint64_t sys_write(struct process *p, uint64_t fd, uint64_t va, uint64_t len)
{
    if (fd != 1) return SYS_EBADF;
    if (len > WRITE_MAX) return SYS_EINVAL;
    if (!vm_user_range_ok(p->satp, va, len, SV39_PTE_R)) return SYS_EFAULT;
    csr_set(sstatus, SSTATUS_SUM);
    for (uint64_t i = 0; i < len; ++i) {
        char c = *(volatile const char *)(uintptr_t)(va + i);
        if (c == '\n') uart_putc('\r');
        /* v1.2: backspace passes too, for line editing in the shell */
        uart_putc((c == '\n' || c == '\t' || c == '\b' || (c >= 0x20 && c <= 0x7e)) ? c : '?');
    }
    csr_clear(sstatus, SSTATUS_SUM);
    p->writes++;
    p->bytes_written += len;
    return len;
}

/* v1.2 read(0, buf, len): validate everything before blocking; with input
 * waiting copy at once, otherwise BLOCKED until input arrives. The result
 * of a blocked read is stored by sched_dispatch. */
static struct trap_frame *sys_read(struct process *p, struct trap_frame *f)
{
    uint64_t fd = f->x[10], va = f->x[11], len = f->x[12];
    if (fd != 0) { f->x[10] = SYS_EBADF; return f; }
    if (len > READ_MAX) { f->x[10] = SYS_EINVAL; return f; }
    if (!vm_user_range_ok(p->satp, va, len, SV39_PTE_R | SV39_PTE_W)) {
        f->x[10] = SYS_EFAULT;
        return f;
    }
    if (len == 0) { f->x[10] = 0; return f; }
    if (input.count) {
        uint64_t n = input_take_user(va, len);
        p->reads++;
        p->bytes_read += n;
        f->x[10] = n;
        return f;
    }
    p->read_pending = 1;
    p->read_va = va;
    p->read_len = len;
    p->block_start = csr_read(time);
    p->state = PROC_BLOCKED;
    return schedule("read");
}

/* Microseconds of `time` since kernel entry (no overflow for ~58 000 years). */
static uint64_t uptime_us(void)
{
    uint64_t d = csr_read(time) - boot_time, tb = boot_configuration.timebase_hz;
    return d / tb * 1000000u + d % tb * 1000000u / tb;
}

/* Copy words to a user buffer the caller validated as U+R+W (bytewise:
 * the buffer may be unaligned). */
static void copy_words_to_user(uint64_t va, const uint64_t *w, unsigned words)
{
    csr_set(sstatus, SSTATUS_SUM);
    for (unsigned i = 0; i < words * 8u; ++i)
        *(volatile uint8_t *)(uintptr_t)(va + i) = (uint8_t)(w[i / 8] >> (8 * (i % 8)));
    csr_clear(sstatus, SSTATUS_SUM);
}

/* procinfo(pid, buf): state, preemptions, syscalls, dispatches of any pid,
 * written as 4 little-endian words into the caller's writable buffer. */
static uint64_t sys_procinfo(struct process *p, uint64_t pid, uint64_t va)
{
    const struct process *q = find_pid(pid);
    if (!q) return SYS_EINVAL;
    if (!vm_user_range_ok(p->satp, va, PROCINFO_WORDS * 8u, SV39_PTE_R | SV39_PTE_W))
        return SYS_EFAULT;
    const uint64_t w[PROCINFO_WORDS] = { q->state, q->preemptions, q->syscalls, q->dispatches };
    copy_words_to_user(va, w, PROCINFO_WORDS);
    return 0;
}

/* v1.3 slotinfo(slot, buf): pid, state, program, preemptions, syscalls and
 * pages owned of process-table slot 0..7 (state 0 = unused slot). */
static uint64_t sys_slotinfo(struct process *p, uint64_t slot, uint64_t va)
{
    if (slot >= PROC_MAX) return SYS_EINVAL;
    if (!vm_user_range_ok(p->satp, va, SLOTINFO_WORDS * 8u, SV39_PTE_R | SV39_PTE_W))
        return SYS_EFAULT;
    const struct process *q = &procs[slot];
    const uint64_t w[SLOTINFO_WORDS] = { q->pid, q->state, q->prog, q->preemptions,
                                         q->syscalls, q->npages };
    copy_words_to_user(va, w, SLOTINFO_WORDS);
    return 0;
}

/* v1.3 spawn(program): a new process in a free slot (unused, or ended and
 * reclaimed) with a fresh address space. Returns its pid. */
/* A run-time slot that is unused, or whose process ended and was reclaimed. */
static struct process *free_slot(void)
{
    for (unsigned i = PROC_STATIC; i < PROC_MAX; ++i)
        if (procs[i].state == PROC_UNUSED ||
            (!alive(&procs[i]) && procs[i].npages == 0)) return &procs[i];
    return 0;
}

/* Forget everything about the previous owner of slot q. */
static void clear_slot(struct process *q)
{
    volatile uint64_t *w = (volatile uint64_t *)q;
    for (size_t i = 0; i < sizeof(*q) / 8u; ++i) w[i] = 0;
}

static uint64_t sys_spawn(uint64_t prog)
{
    if (prog == PROG_NONE || prog >= PROG_IDS) return SYS_EINVAL;
    if (sched.next_pid == UINT64_MAX) return SYS_EAGAIN;   /* v1.3.1: pids never wrap */
    struct process *q = free_slot();
    if (!q) return SYS_EAGAIN;
    unsigned slot = (unsigned)(q - procs);
    uint64_t base = USER_SLOT_BASE(slot), pages[VM_PROC_PAGES];
    uint64_t satp = vm_create(base, pages);
    if (!satp) return SYS_ENOMEM;
    clear_slot(q);
    q->pid = sched.next_pid++;
    q->prog = prog;
    q->satp = satp;
    q->npages = VM_PROC_PAGES;
    for (unsigned i = 0; i < VM_PROC_PAGES; ++i) q->pages[i] = pages[i];
    q->frame.sepc = PROG_VA + (uint64_t)(programs[prog] - __user_prog_text);
    q->frame.x[2] = base + USER_STACK_TOP_OFF;
    q->frame.x[10] = base + USER_DATA_OFF;      /* a0: own data page */
    q->frame.x[11] = q->pid;                    /* a1: own pid */
    q->state = PROC_READY;
    sched.spawned++;
    if (sched.verbose) {
        uart_puts("SPAWN "); log_proc(q);
        uart_puts(" prog="); uart_dec(prog);
        uart_puts(" satp="); uart_hex(satp);
        uart_puts(" free="); uart_dec(mem.free); uart_puts("\n");
    }
    return q->pid;
}

/* v1.3 kill(pid): end a live run-time process and reclaim its pages.
 * The boot processes and the caller itself cannot be killed (use exit). */
static uint64_t sys_kill(struct process *p, uint64_t pid)
{
    struct process *q = find_pid(pid);
    if (!q || !alive(q)) return SYS_ESRCH;
    if (q == p || q - procs < (ptrdiff_t)PROC_STATIC) return SYS_EPERM;
    q->state = PROC_EXITED;
    q->exit_code = KILLED_EXIT_CODE;
    q->killed = 1;
    sched.killed++;
    if (sched.verbose) {
        uart_puts("KILL "); log_proc(q);
        uart_puts(" by "); log_proc(p); uart_puts("\n");
    }
    reclaim(q);
    ended(q);
    return 0;
}

/* ------------------------------------------------------------------ v1.4 */
static uint64_t page_up(uint64_t v)
{
    return (v + SV39_PAGE_OFFSET_MASK) & ~SV39_PAGE_OFFSET_MASK;
}

static uint64_t refused(int reason, const char *name)
{
    sched.load_errors++;
    sched.last_load_error = (uint64_t)(int64_t)reason;
    if (sched.verbose) {
        uart_puts("RUN refused name="); uart_puts(name);
        uart_puts(" reason=-"); uart_dec((uint64_t)-(int64_t)reason); uart_puts("\n");
    }
    return SYS_ENOEXEC;
}

/* run(name, len): start program `name` from the archive in a free slot.
 * Everything about the file is checked before anything is allocated; then
 * its own root, tables, segment pages (file bytes copied, rest zero) and
 * stack are built, or all of it is given back. Returns the pid. */
static uint64_t sys_run(struct process *p, uint64_t va, uint64_t len)
{
    char name[ARCHIVE_NAME_MAX + 1];
    if (len == 0 || len > ARCHIVE_NAME_MAX) return SYS_EINVAL;
    if (!vm_user_range_ok(p->satp, va, len, SV39_PTE_R)) return SYS_EFAULT;
    csr_set(sstatus, SSTATUS_SUM);
    for (uint64_t i = 0; i < len; ++i) name[i] = *(volatile const char *)(uintptr_t)(va + i);
    csr_clear(sstatus, SSTATUS_SUM);
    name[len] = 0;
    int idx = archive_find(&prog_archive, name, len);
    if (idx < 0) return SYS_ENOENT;
    const struct archive_file *file = &prog_archive.files[idx];
    const uint8_t *img = (const uint8_t *)(uintptr_t)(prog_archive_pa + file->offset);
    struct elf_plan plan;
    int err = elf_check(img, file->size, PROC_PAGE_LIMIT - ELF_TABLE_PAGES - ELF_STACK_PAGES, &plan);
    if (err) return refused(err, name);
    if (sched.next_pid == UINT64_MAX) return SYS_EAGAIN;
    struct process *q = free_slot();
    if (!q) return SYS_EAGAIN;
    uint64_t pages[PROC_PAGE_LIMIT], n = ELF_TABLE_PAGES;
    uint64_t satp = vm_elf_create(pages);
    if (!satp) return SYS_ENOMEM;
    for (uint64_t k = 0; k < plan.nsegs; ++k) {
        const struct elf_segment *s = &plan.seg[k];
        uint64_t pte = (s->flags & ELF_PF_X) ? VM_PTE_UTEXT :
                       (s->flags & ELF_PF_W) ? VM_PTE_URW : VM_PTE_URO;
        for (uint64_t off = 0; off < s->memsz; off += SV39_PAGE_SIZE) {
            uint64_t pa = mem_alloc();          /* zeroed: .bss and the tail stay 0 */
            if (!pa) goto out_of_memory;
            pages[n++] = pa;
            volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)pa;
            for (uint64_t i = 0; i < SV39_PAGE_SIZE && off + i < s->filesz; ++i)
                dst[i] = img[s->offset + off + i];
            vm_elf_map(pages[2], s->vaddr + off, pa, pte);
        }
    }
    for (uint64_t sva = ELF_STACK_BASE; sva < ELF_STACK_TOP; sva += SV39_PAGE_SIZE) {
        uint64_t pa = mem_alloc();
        if (!pa) goto out_of_memory;
        pages[n++] = pa;
        vm_elf_map(pages[2], sva, pa, VM_PTE_URW);
    }
    /* The code arrived through ordinary stores: order them before any
     * instruction fetch from these pages (single hart). */
    __asm__ volatile("fence.i" ::: "memory");
    clear_slot(q);
    q->pid = sched.next_pid++;
    q->prog = PROG_FILE + (uint64_t)idx;
    q->satp = satp;
    q->npages = n;
    for (uint64_t i = 0; i < n; ++i) q->pages[i] = pages[i];
    q->heap_base = q->brk = plan.image_end;
    q->frame.sepc = plan.entry;
    q->frame.x[2] = ELF_STACK_TOP;
    q->state = PROC_READY;
    sched.loaded++;
    if (sched.verbose) {
        uart_puts("RUN "); log_proc(q);
        uart_puts(" name="); uart_puts(name);
        uart_puts(" pages="); uart_dec(n);
        uart_puts(" entry="); uart_hex(plan.entry);
        uart_puts(" free="); uart_dec(mem.free); uart_puts("\n");
    }
    return q->pid;
out_of_memory:
    vm_destroy(pages, n);
    return SYS_ENOMEM;
}

/* wait(pid, flags): the status of process pid once it has ended: its exit
 * code (low 8 bits; 137 killed, 130 Ctrl-C) or 139 if it faulted. Blocks
 * while it is alive. WAIT_FOREGROUND makes it the console foreground
 * process for the duration: Ctrl-C then ends it. */
static struct trap_frame *sys_wait(struct process *p, struct trap_frame *f)
{
    uint64_t pid = f->x[10], flags = f->x[11];
    if (flags & ~WAIT_FOREGROUND) { f->x[10] = SYS_EINVAL; return f; }
    struct process *q = find_pid(pid);
    if (!q) { f->x[10] = SYS_ESRCH; return f; }
    if (q == p) { f->x[10] = SYS_EPERM; return f; }
    if (!alive(q)) {
        f->x[10] = wait_status(q);
        p->waits_done++;
        return f;
    }
    if (flags & WAIT_FOREGROUND) {
        if (q - procs < (ptrdiff_t)PROC_STATIC) { f->x[10] = SYS_EPERM; return f; }
        if (sched.fg_pid) { f->x[10] = SYS_EBUSY; return f; }
        sched.fg_pid = q->pid;
        sched.fg_waiter = p->pid;
        input.intr_armed = 1;
    }
    p->wait_pid = q->pid;
    p->block_start = csr_read(time);
    p->state = PROC_BLOCKED;
    return schedule("wait");
}

/* sbrk(bytes): grow the heap of an archive program by `bytes` (>= 0) with
 * fresh zeroed R W U pages; returns the previous break. All or nothing. */
static uint64_t sys_sbrk(struct process *p, uint64_t incr)
{
    if (p->prog < PROG_FILE || (incr >> 63)) return SYS_EINVAL;
    uint64_t old = p->brk;
    if (incr > ELF_HEAP_END - old) return SYS_ENOMEM;
    uint64_t from = page_up(old), to = page_up(old + incr);
    uint64_t need = (to - from) / SV39_PAGE_SIZE;
    if (need > PROC_PAGE_LIMIT - p->npages) return SYS_ENOMEM;
    for (uint64_t k = 0; k < need; ++k) {
        uint64_t pa = mem_alloc();
        if (!pa) {
            while (k--) mem_free(p->pages[p->npages + k]);
            return SYS_ENOMEM;
        }
        p->pages[p->npages + k] = pa;
    }
    for (uint64_t k = 0; k < need; ++k)
        vm_elf_map(p->pages[2], from + k * SV39_PAGE_SIZE, p->pages[p->npages + k], VM_PTE_URW);
    p->npages += need;
    p->brk = old + incr;
    /* New valid leaves in the installed root. */
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
    return old;
}

/* progs(index, buf): name of archive file `index` (16 bytes, NUL padded)
 * into the caller's writable buffer; returns the file size. */
static uint64_t sys_progs(struct process *p, uint64_t index, uint64_t va)
{
    if (index >= prog_archive.count) return SYS_ENOENT;
    if (!vm_user_range_ok(p->satp, va, PROGS_NAME_BYTES, SV39_PTE_R | SV39_PTE_W))
        return SYS_EFAULT;
    const char *name = prog_archive.files[index].name;
    int end = 0;
    csr_set(sstatus, SSTATUS_SUM);
    for (unsigned i = 0; i < PROGS_NAME_BYTES; ++i) {
        if (!name[i]) end = 1;
        *(volatile char *)(uintptr_t)(va + i) = end ? 0 : name[i];
    }
    csr_clear(sstatus, SSTATUS_SUM);
    return prog_archive.files[index].size;
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
    case SYS_READ:
        return sys_read(p, f);
    case SYS_UPTIME:
        f->x[10] = uptime_us();
        return f;
    case SYS_PROCINFO:
        f->x[10] = sys_procinfo(p, f->x[10], f->x[11]);
        return f;
    case SYS_SPAWN:
        f->x[10] = sys_spawn(f->x[10]);
        return f;
    case SYS_KILL:
        f->x[10] = sys_kill(p, f->x[10]);
        return f;
    case SYS_MEMINFO:
        f->x[10] = mem.free;
        f->x[11] = mem.pages - mem.reserved;    /* v1.4: archive pages excluded */
        return f;
    case SYS_SLOTINFO:
        f->x[10] = sys_slotinfo(p, f->x[10], f->x[11]);
        return f;
    case SYS_RUN:
        f->x[10] = sys_run(p, f->x[10], f->x[11]);
        return f;
    case SYS_WAIT:
        return sys_wait(p, f);
    case SYS_SBRK:
        f->x[10] = sys_sbrk(p, f->x[10]);
        return f;
    case SYS_PROGS:
        f->x[10] = sys_progs(p, f->x[10], f->x[11]);
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
        reclaim(p);
        ended(p);
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
    reclaim(p);
    ended(p);
    return schedule("fault");
}

static void heartbeat(void)
{
    uart_puts("HEARTBEAT ticks="); uart_dec(sched.timer_ticks);
    uart_puts(" switches="); uart_dec(sched.switches);
    for (unsigned i = 0; i < PROC_MAX; ++i) {
        if (procs[i].state == PROC_UNUSED) continue;
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
        if ((cause & ~SCAUSE_INTERRUPT) == IRQ_S_EXTERNAL) {
            /* Input only fills the buffer; readers wake at the next
             * scheduling point (at most one quantum later). */
            sched.ext_irqs++;
            input_irq();
            /* v1.4 Ctrl-C: if it ended the interrupted process, run another */
            if (input.intr_pending && console_intr() && p->state != PROC_RUNNING)
                return schedule("intr");
            return f;
        }
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
    static const uint64_t bases[PROC_STATIC] = { USER_A_BASE, USER_B_BASE };
    const uint64_t entries[PROC_STATIC] = { entry_a, entry_b };
    /* 10 ms quantum from the DT timebase (QEMU virt: 10 MHz). */
    sched.quantum_ticks = boot_configuration.timebase_hz / 100u;
    sched.verbose = (uint64_t)verbose;
    sched.heartbeat = (uint64_t)heartbeat_on;
    sched.next_pid = PROC_STATIC + 1u;
    sched.last_slot = PROC_MAX - 1u;            /* the first pick is slot 0 */
    for (unsigned i = 0; i < PROC_STATIC; ++i) {
        struct process *p = &procs[i];
        p->pid = i + 1;
        p->satp = vm_proc_satp[i];
        p->frame.sepc = entries[i];
        p->frame.x[2] = bases[i] + USER_STACK_TOP_OFF;
        p->frame.x[10] = bases[i] + USER_DATA_OFF;  /* a0: own data page (v1.3) */
        p->frame.x[11] = p->pid;                     /* a1: own pid */
        p->state = PROC_READY;
        if (verbose || heartbeat_on) {
            uart_puts("PROC "); log_proc(p);
            uart_puts(" entry="); uart_hex(p->frame.sepc);
            uart_puts(" sp="); uart_hex(p->frame.x[2]);
            uart_puts(" satp="); uart_hex(p->satp); uart_puts("\n");
        }
    }
    csr_write(scounteren, 0);
    input_init();
    timer_arm();
    csr_set(sie, SIE_STIE | SIE_SEIE);
    user_return(schedule("start"));
}
