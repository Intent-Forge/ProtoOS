#ifndef PROTOOS_SV39_H
#define PROTOOS_SV39_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>

/* RISC-V Supervisor ISA 1.13, sections 9.3/9.4, Figure 29:
 * https://docs.riscv.org/reference/isa/v20240411/priv/supervisor.html
 * Base Sv39 only: Svnapot and Svpbmt encodings are NOT enabled.
 * Integer representation, no implementation-defined C bitfields.
 * This header neither allocates tables nor writes any CSR.
 */
typedef uint64_t sv39_pte_t;
#define SV39_PAGE_SHIFT 12u
#define SV39_PAGE_SIZE (UINT64_C(1) << SV39_PAGE_SHIFT)
#define SV39_PAGE_OFFSET_MASK (SV39_PAGE_SIZE - UINT64_C(1))
#define SV39_MEGA_SIZE (UINT64_C(1) << 21)          /* level-1 leaf, 2 MiB */
#define SV39_MEGA_OFFSET_MASK (SV39_MEGA_SIZE - UINT64_C(1))
#define SV39_LEVELS 3u
#define SV39_PT_ENTRIES 512u
#define SV39_VPN_BITS 9u
#define SV39_VPN_MASK UINT64_C(0x1ff)

#define SV39_PTE_V (UINT64_C(1) << 0)
#define SV39_PTE_R (UINT64_C(1) << 1)
#define SV39_PTE_W (UINT64_C(1) << 2)
#define SV39_PTE_X (UINT64_C(1) << 3)
#define SV39_PTE_U (UINT64_C(1) << 4)
#define SV39_PTE_G (UINT64_C(1) << 5)
#define SV39_PTE_A (UINT64_C(1) << 6)
#define SV39_PTE_D (UINT64_C(1) << 7)
#define SV39_PTE_FLAGS_MASK UINT64_C(0xff)
#define SV39_PTE_RSW_SHIFT 8u
#define SV39_PTE_RSW_MASK (UINT64_C(3) << SV39_PTE_RSW_SHIFT)
#define SV39_PTE_PPN_SHIFT 10u
#define SV39_PTE_PPN_BITS 44u
#define SV39_PPN_MASK ((UINT64_C(1) << SV39_PTE_PPN_BITS) - UINT64_C(1))
#define SV39_PTE_PPN_MASK (SV39_PPN_MASK << SV39_PTE_PPN_SHIFT)
#define SV39_PTE_PPN0_SHIFT 10u
#define SV39_PTE_PPN1_SHIFT 19u
#define SV39_PTE_PPN2_SHIFT 28u
#define SV39_PTE_PPN0_MASK (UINT64_C(0x1ff) << SV39_PTE_PPN0_SHIFT)
#define SV39_PTE_PPN1_MASK (UINT64_C(0x1ff) << SV39_PTE_PPN1_SHIFT)
#define SV39_PTE_PPN2_MASK (UINT64_C(0x3ffffff) << SV39_PTE_PPN2_SHIFT)
/* Bits 63:54 must be zero in the selected base format. */
#define SV39_PTE_RESERVED_MASK UINT64_C(0xffc0000000000000)
/* R or X identifies a leaf; W=1,R=0 is invalid, not a leaf kind.
 * Non-leaf U/A/D must be zero; G and software RSW may be present. */
#define SV39_PTE_LEAF_MASK (SV39_PTE_R | SV39_PTE_X)
#define SV39_PTE_NONLEAF_ZERO_MASK (SV39_PTE_R | SV39_PTE_W | SV39_PTE_X | SV39_PTE_U | SV39_PTE_A | SV39_PTE_D | SV39_PTE_RESERVED_MASK)

struct sv39_page_table {
    _Alignas(4096) sv39_pte_t entries[SV39_PT_ENTRIES];
};

_Static_assert(CHAR_BIT == 8 && sizeof(sv39_pte_t) == 8, "64-bit PTE");
_Static_assert((sv39_pte_t)-1 == UINT64_MAX, "unsigned PTE");
_Static_assert(sizeof(struct sv39_page_table) == 4096, "one page per table");
_Static_assert(_Alignof(struct sv39_page_table) == 4096, "page-aligned table");
_Static_assert(offsetof(struct sv39_page_table, entries) == 0, "no leading padding");
_Static_assert(SV39_PT_ENTRIES * sizeof(sv39_pte_t) == SV39_PAGE_SIZE, "table geometry");
_Static_assert((SV39_PTE_PPN0_MASK | SV39_PTE_PPN1_MASK | SV39_PTE_PPN2_MASK) == SV39_PTE_PPN_MASK, "PPN fields");
_Static_assert((SV39_PTE_FLAGS_MASK | SV39_PTE_RSW_MASK | SV39_PTE_PPN_MASK | SV39_PTE_RESERVED_MASK) == UINT64_MAX, "all 64 bits covered");
#endif
