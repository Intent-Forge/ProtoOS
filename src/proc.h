#ifndef PROTOOS_PROC_H
#define PROTOOS_PROC_H
#include "platform.h"
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
#define SYS_EBADF  ((uint64_t)-9)
#define SYS_EFAULT ((uint64_t)-14)
#define SYS_EINVAL ((uint64_t)-22)
#define SYS_ENOSYS ((uint64_t)-38)
#define WRITE_MAX  UINT64_C(256)
#define READ_MAX   UINT64_C(256)
#define SLEEP_MAX_MS UINT64_C(60000)
#define PROCINFO_WORDS 4u   /* state, preemptions, syscalls, dispatches */

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
};

extern struct process procs[PROC_COUNT];
extern struct process *current;
extern struct sched_stats sched;

_Noreturn void proc_start(uint64_t entry_a, uint64_t entry_b, int verbose, int heartbeat);
struct trap_frame *trap_from_user(struct trap_frame *f);
_Noreturn void user_return(struct trap_frame *f);
#endif
