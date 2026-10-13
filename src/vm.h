#ifndef PROTOOS_VM_H
#define PROTOOS_VM_H
#include <stdint.h>

#define PROC_STATIC 2u      /* A and B: created at boot, static pages (v1.0) */
#define PROC_MAX 8u         /* v1.3: slots 2..7 are created at run time */

/* Per-process user virtual layout (identical shape, disjoint ranges, so a
 * cross-process access names a VA that is simply unmapped in the attacker):
 *   base+0x0000 text  R X U   (A and B: one page from the kernel image;
 *                              run-time processes: unmapped)
 *   base+0x1000 data  R W U
 *   base+0x2000 -     unmapped stack guard
 *   base+0x3000 stack R W U   (sp starts at base+0x4000)
 * Slot s has base 0x400000 * (s + 1): A 0x400000, B 0x800000, ...
 * Must match the .equ constants in src/user.S. */
#define USER_A_BASE UINT64_C(0x400000)
#define USER_B_BASE UINT64_C(0x800000)
#define USER_SLOT_BASE(s) (UINT64_C(0x400000) * ((uint64_t)(s) + 1u))
#define USER_TEXT_OFF  UINT64_C(0x0000)
#define USER_DATA_OFF  UINT64_C(0x1000)
#define USER_STACK_OFF UINT64_C(0x3000)
#define USER_STACK_TOP_OFF UINT64_C(0x4000)
/* v1.3: shared program pages (R X U) at the same VA in every process root;
 * every run-time process and the shell execute from here. */
#define PROG_VA UINT64_C(0x4000000)
#define PROG_PAGES 3u
/* Pages owned by a run-time process: root, low L1, user L0, data, stack. */
#define VM_PROC_PAGES 5u

/* v1.4 programs from the archive (src/elf.h): every one sees the same
 * 2 MiB window in its own root, behind a private level-1 table (the 1 GiB
 * at ELF_LOAD_BASE is unused by the kernel and the template) and a private
 * level-0 table:
 *   0x40000000 ... ELF segments (below ELF_LOAD_END), then the heap
 *   ELF_HEAP_END   guard page, never mapped
 *   0x401fc000 ... stack, ELF_STACK_PAGES R W U, sp = ELF_STACK_TOP */
#define ELF_TABLE_PAGES 3u          /* root, L1, L0 */
#define ELF_STACK_PAGES 4u
#define ELF_STACK_TOP  UINT64_C(0x40200000)
#define ELF_STACK_BASE (ELF_STACK_TOP - ELF_STACK_PAGES * UINT64_C(4096))
#define ELF_HEAP_END   (ELF_STACK_BASE - UINT64_C(4096))
/* Upper bound on the pages one run-time process may own (tables included). */
#define PROC_PAGE_LIMIT 64u
/* User leaf permissions (A and D preset). */
#define VM_PTE_UTEXT UINT64_C(0x5b)     /* V R X U A */
#define VM_PTE_URO   UINT64_C(0x53)     /* V R U A */
#define VM_PTE_URW   UINT64_C(0xd7)     /* V R W U A D */

extern uint64_t vm_kernel_satp;
extern uint64_t vm_proc_satp[PROC_STATIC];

/* Build the kernel, template and A/B address spaces (MMU still off); the
 * pool [MEM_POOL_START, pool_end) is mapped kernel-only in all of them.
 * Returns 0 or a nonzero diagnostic code. */
int vm_init(uint64_t pool_end);
/* 1 if every byte of [va, va+len) is in a valid user (U=1) 4 KiB page of the
 * root in satp whose leaf PTE also has all bits in `need` (e.g. SV39_PTE_R). */
int vm_user_range_ok(uint64_t satp, uint64_t va, uint64_t len, uint64_t need);
/* New address space for a run-time process at user VA base: kernel and
 * program pages shared from the template, fresh zeroed data and stack.
 * Fills pages[] and returns the satp value, or 0 if memory ran out. */
uint64_t vm_create(uint64_t base, uint64_t pages[VM_PROC_PAGES]);
/* Free pages[0..n) (from vm_create, vm_elf_create or later mappings). No
 * root among them may be in use. */
void vm_destroy(const uint64_t *pages, uint64_t n);
/* v1.4: new address space for an ELF program: root copied from the
 * template, private L1 and L0 for the window. pages[0..2] = root, L1, L0.
 * Returns the satp value, or 0 if memory ran out (nothing kept). */
uint64_t vm_elf_create(uint64_t pages[ELF_TABLE_PAGES]);
/* Map user page va (4 KiB aligned, inside the window) to pa in the window's
 * level-0 table with leaf flags pte (VM_PTE_*). */
void vm_elf_map(uint64_t l0_pa, uint64_t va, uint64_t pa, uint64_t pte);
/* Install satp and flush all translations. */
void vm_switch(uint64_t satp);
#endif
