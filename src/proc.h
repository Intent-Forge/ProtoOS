#ifndef PROTOOS_PROC_H
#define PROTOOS_PROC_H
#include "platform.h"
#include "vm.h"

/* User syscall ABI: number in a7, argument in a0, result in a0.
 * Numbers follow the RISC-V Linux table for familiarity only. */
#define SYS_EXIT   UINT64_C(93)
#define SYS_YIELD  UINT64_C(124)
#define SYS_GETPID UINT64_C(172)
#define SYS_ENOSYS ((uint64_t)-38)

enum proc_state { PROC_UNUSED, PROC_READY, PROC_RUNNING, PROC_EXITED, PROC_FAULTED };

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
};
_Static_assert(offsetof(struct process, frame) == 0, "trap.S frame protocol");

struct sched_stats {
    uint64_t quantum_ticks;
    uint64_t timer_ticks;
    uint64_t switches;
    uint64_t idle_entered;
    uint64_t verbose;
};

extern struct process procs[PROC_COUNT];
extern struct process *current;
extern struct sched_stats sched;

_Noreturn void proc_start(uint64_t entry_a, uint64_t entry_b, int verbose);
struct trap_frame *trap_from_user(struct trap_frame *f);
_Noreturn void user_return(struct trap_frame *f);
#endif
