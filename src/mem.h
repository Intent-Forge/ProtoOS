#ifndef PROTOOS_MEM_H
#define PROTOOS_MEM_H
#include <stdint.h>

/* v1.3 physical page allocator. The pool is the RAM above the 2 MiB that
 * hold the kernel image, up to the 2 MiB boundary below the device tree.
 * The kernel reaches every pool page through a 2 MiB identity map with
 * U=0 in every root, so user mode never sees the pool except through its
 * own 4 KiB user mappings. Pages are zeroed when handed out. */
#define MEM_POOL_START UINT64_C(0x80400000)
#define MEM_RAM_END    UINT64_C(0x88000000)
#define MEM_MAX_PAGES  ((MEM_RAM_END - MEM_POOL_START) / 4096u)

struct mem_state {
    uint64_t base;      /* physical address of page 0 of the pool */
    uint64_t pages;     /* pages in the pool */
    uint64_t free;      /* pages currently free */
    uint64_t allocs;    /* successful allocations so far */
    uint64_t frees;     /* frees so far */
    uint64_t reserved;  /* v1.4: pages taken for good at boot (program archive) */
    uint64_t bitmap[MEM_MAX_PAGES / 64u];   /* bit set = page in use */
};
extern struct mem_state mem;

/* Pool [MEM_POOL_START, end); end must be 2 MiB aligned and <= MEM_RAM_END. */
void mem_init(uint64_t end);
/* Lowest free page, zeroed; 0 if the pool is exhausted. */
uint64_t mem_alloc(void);
/* Return a page from mem_alloc; anything else is a kernel bug (panic). */
void mem_free(uint64_t pa);
/* v1.4: take the pages covering [start, end) out of the pool for good (the
 * program archive). The range must lie inside the pool and be free; it is
 * not counted as allocations. Returns the number of pages. */
uint64_t mem_reserve(uint64_t start, uint64_t end);
#endif
