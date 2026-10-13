#ifndef PROTOOS_PROC_H
#define PROTOOS_PROC_H
#include "platform.h"
#include "archive.h"
#include "vm.h"

/* User syscall ABI: number in a7, arguments in a0..a2, result in a0.
 * 63/64/93/124/172 follow the RISC-V Linux table; 1001+ are ProtoOS-specific. */
#define SYS_READ   UINT64_C(63)
#define SYS_WRITE  UINT64_C(64)
#define SYS_EXIT   UINT64_C(93)
#define SYS_YIELD  UINT64_C(124)
#define SYS_GETPID UINT64_C(172)
#define SYS_SLEEP  UINT64_C(1001)
#define SYS_UPTIME UINT64_C(1002)
#define SYS_PROCINFO UINT64_C(1003)
#define SYS_SPAWN  UINT64_C(1004)
#define SYS_KILL   UINT64_C(1005)
#define SYS_MEMINFO UINT64_C(1006)
#define SYS_SLOTINFO UINT64_C(1007)
/* v1.4 */
#define SYS_RUN    UINT64_C(1008)
#define SYS_WAIT   UINT64_C(1009)
#define SYS_SBRK   UINT64_C(1010)
#define SYS_PROGS  UINT64_C(1011)
#define SYS_EPERM  ((uint64_t)-1)
#define SYS_ENOENT ((uint64_t)-2)
#define SYS_ESRCH  ((uint64_t)-3)
#define SYS_ENOEXEC ((uint64_t)-8)
#define SYS_EAGAIN ((uint64_t)-11)
#define SYS_ENOMEM ((uint64_t)-12)
#define SYS_EBADF  ((uint64_t)-9)
#define SYS_EFAULT ((uint64_t)-14)
#define SYS_EBUSY  ((uint64_t)-16)
#define SYS_EINVAL ((uint64_t)-22)
#define SYS_ENOSYS ((uint64_t)-38)
#define WRITE_MAX  UINT64_C(256)
#define READ_MAX   UINT64_C(256)
#define SLEEP_MAX_MS UINT64_C(60000)
#define PROCINFO_WORDS 4u   /* state, preemptions, syscalls, dispatches */
#define SLOTINFO_WORDS 6u   /* pid, state, program, preemptions, syscalls, pages */
#define KILLED_EXIT_CODE UINT64_C(137)
/* v1.4 */
#define INTR_EXIT_CODE UINT64_C(130)     /* ended by Ctrl-C */
#define FAULT_STATUS   UINT64_C(139)     /* what wait() reports for a FAULTED process */
#define WAIT_FOREGROUND UINT64_C(1)      /* wait() flag: Ctrl-C ends the awaited process */
#define PROGS_NAME_BYTES 16u             /* progs(): name buffer, NUL padded */
/* v1.3 built-in programs (src/user.S, shared program pages) */
enum program_id { PROG_NONE, PROG_WORKER, PROG_HELLO, PROG_COUNTER, PROG_CRASH,
                  PROG_MEMCHECK, PROG_IDS };
/* v1.4: a process started from archive file n has program id PROG_FILE + n. */
#define PROG_FILE UINT64_C(16)

enum proc_state { PROC_UNUSED, PROC_READY, PROC_RUNNING, PROC_EXITED, PROC_FAULTED,
                  PROC_SLEEPING, PROC_BLOCKED };

struct process {
    struct trap_frame frame;   /* MUST stay first: trap.S uses sscratch=&frame */
    uint64_t pid;
    uint64_t state;
    uint64_t satp;
    uint64_t dispatches;
    uint64_t preemptions;
    uint64_t yields;
    uint64_t syscalls;
    uint64_t rejected_syscalls;
    uint64_t exit_code;
    uint64_t fault_cause, fault_epc, fault_tval;
    /* v1.1 */
    uint64_t writes, bytes_written, sleeps;
    uint64_t sleep_start, wake_time, last_dispatch_time;
    /* v1.2: one pending read per BLOCKED process, completed at dispatch */
    uint64_t reads, bytes_read, read_pending, read_va, read_len, block_start;
    /* v1.3: run-time processes own their pages until they end */
    uint64_t prog, killed, npages;
    uint64_t pages[PROC_PAGE_LIMIT];    /* v1.4: was VM_PROC_PAGES */
    /* v1.4: heap of an archive program; a BLOCKED waiter (wait_pid != 0) */
    uint64_t heap_base, brk;
    uint64_t wait_pid, waits_done;
};
_Static_assert(offsetof(struct process, frame) == 0, "trap.S frame protocol");

struct sched_stats {
    uint64_t quantum_ticks;
    uint64_t timer_ticks;
    uint64_t switches;
    uint64_t idle_entered;
    uint64_t verbose;
    uint64_t heartbeat;
    uint64_t waits;         /* times the kernel waited because everyone slept */
    uint64_t ext_irqs;      /* v1.2: external interrupts taken from U-mode */
    uint64_t next_pid;      /* v1.3: pids are never reused */
    uint64_t spawned, killed, reclaimed;
    uint64_t last_slot;     /* v1.3.1: Round Robin position, kept across reclaim */
    /* v1.4 */
    uint64_t loaded;        /* archive programs started */
    uint64_t load_errors;   /* run() calls refused */
    uint64_t last_load_error;   /* elf_check / archive reason of the last refusal */
    uint64_t fg_pid;        /* console foreground: Ctrl-C ends this process */
    uint64_t fg_waiter;     /* pid whose foreground wait() set fg_pid */
    uint64_t interrupted;   /* processes ended by Ctrl-C */
};

extern struct process procs[PROC_MAX];
extern struct process *current;
extern struct sched_stats sched;
/* v1.4: the validated program archive (count 0 if none or rejected) and
 * the address of its first byte (inside the pool, reserved, kernel-only). */
extern struct archive prog_archive;
extern uint64_t prog_archive_pa;

_Noreturn void proc_start(uint64_t entry_a, uint64_t entry_b, int verbose, int heartbeat);
struct trap_frame *trap_from_user(struct trap_frame *f);
_Noreturn void user_return(struct trap_frame *f);
#endif
