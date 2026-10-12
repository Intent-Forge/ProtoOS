#ifndef PROTOOS_VM_H
#define PROTOOS_VM_H
#include <stdint.h>

#define PROC_COUNT 2u

/* Per-process user virtual layout (identical shape, disjoint ranges, so a
 * cross-process access names a VA that is simply unmapped in the attacker):
 *   base+0x0000 text  R X U   (one page from the kernel image)
 *   base+0x1000 data  R W U
 *   base+0x2000 -     unmapped stack guard
 *   base+0x3000 stack R W U   (sp starts at base+0x4000)
 * Must match the .equ constants in src/user.S. */
#define USER_A_BASE UINT64_C(0x400000)
#define USER_B_BASE UINT64_C(0x800000)
#define USER_TEXT_OFF  UINT64_C(0x0000)
#define USER_DATA_OFF  UINT64_C(0x1000)
#define USER_STACK_OFF UINT64_C(0x3000)
#define USER_STACK_TOP_OFF UINT64_C(0x4000)

extern uint64_t vm_kernel_satp;
extern uint64_t vm_proc_satp[PROC_COUNT];

/* Build the kernel and both process address spaces (MMU still off).
 * Returns 0 or a nonzero diagnostic code. */
int vm_init(void);
/* Install satp and flush all translations. */
void vm_switch(uint64_t satp);
#endif
