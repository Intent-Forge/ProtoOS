#include "kernel.h"
#include "mem.h"

struct mem_state mem;

void mem_init(uint64_t end)
{
    mem.base = MEM_POOL_START;
    mem.pages = (end - MEM_POOL_START) / 4096u;
    mem.free = mem.pages;
}

/* Lowest-first keeps the reuse of freed pages deterministic (and testable). */
uint64_t mem_alloc(void)
{
    for (uint64_t w = 0; w * 64u < mem.pages; ++w) {
        if (mem.bitmap[w] == UINT64_MAX) continue;
        for (unsigned bit = 0; bit < 64u; ++bit) {
            uint64_t page = w * 64u + bit;
            if (page >= mem.pages) return 0;
            if (mem.bitmap[w] & (UINT64_C(1) << bit)) continue;
            mem.bitmap[w] |= UINT64_C(1) << bit;
            mem.free--;
            mem.allocs++;
            uint64_t pa = mem.base + page * 4096u;
            /* No data from a previous owner may survive into the next one. */
            volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)pa;
            for (unsigned i = 0; i < 512u; ++i) p[i] = 0;
            return pa;
        }
    }
    return 0;
}

void mem_free(uint64_t pa)
{
    uint64_t page = (pa - mem.base) / 4096u;
    if (pa < mem.base || (pa & 4095u) || page >= mem.pages ||
        !(mem.bitmap[page / 64u] & (UINT64_C(1) << (page % 64u))))
        kernel_panic("mem_free of a page that is not allocated");
    mem.bitmap[page / 64u] &= ~(UINT64_C(1) << (page % 64u));
    mem.free++;
    mem.frees++;
}
