#include "sv39_builder.h"
#define PA_LIMIT (UINT64_C(1) << 56)
static int pool_valid(const struct sv39_page_table *pool, size_t n, uint64_t pa)
{
    return pool && n && !((uintptr_t)pool & SV39_PAGE_OFFSET_MASK) &&
        !(pa & SV39_PAGE_OFFSET_MASK) && pa < PA_LIMIT &&
        n <= (PA_LIMIT - pa) / SV39_PAGE_SIZE &&
        n <= SIZE_MAX / sizeof(*pool);
}
static void clear_page(struct sv39_page_table *p)
{
    for (size_t i = 0; i < SV39_PT_ENTRIES; ++i) p->entries[i] = 0;
}
enum sv39_result sv39_builder_init(struct sv39_builder *b,
    struct sv39_page_table *pool, size_t pages, uint64_t pool_pa)
{
    if (!b || !pool_valid(pool, pages, pool_pa)) return SV39_BAD_ARGUMENT;
    for (size_t i = 0; i < pages; ++i) clear_page(&pool[i]);
    b->pool = pool; b->pool_pa = pool_pa; b->capacity = pages; b->used = 1;
    return SV39_OK;
}
static sv39_pte_t table_pte(const struct sv39_builder *b, size_t index)
{
    return ((b->pool_pa / SV39_PAGE_SIZE + index) << SV39_PTE_PPN_SHIFT) | SV39_PTE_V;
}
enum sv39_result sv39_map4k(struct sv39_builder *b,
    uint64_t va, uint64_t pa, uint64_t flags)
{
    if (!b || !pool_valid(b->pool, b->capacity, b->pool_pa) ||
        !b->used || b->used > b->capacity) return SV39_BAD_ARGUMENT;
    /* Bits 63:39 must replicate bit 38; offsets must be zero. */
    uint64_t high = va >> 38;
    if ((high != 0 && high != (UINT64_MAX >> 38)) ||
        (va & SV39_PAGE_OFFSET_MASK)) return SV39_BAD_VA;
    if (pa >= PA_LIMIT || (pa & SV39_PAGE_OFFSET_MASK)) return SV39_BAD_PA;
    if ((flags & ~(SV39_PTE_FLAGS_MASK & ~SV39_PTE_V)) ||
        !(flags & SV39_PTE_LEAF_MASK) ||
        ((flags & SV39_PTE_W) && !(flags & SV39_PTE_R))) return SV39_BAD_FLAGS;
    size_t index = 0, parent_index = 0;
    unsigned level;
    /* Read-only preflight: stop at first missing edge. */
    for (level = 2; level > 0; --level) {
        size_t vpn = (va >> (SV39_PAGE_SHIFT + level * SV39_VPN_BITS)) & SV39_VPN_MASK;
        sv39_pte_t pte = b->pool[index].entries[vpn];
        if (!pte) break;
        if (pte & SV39_PTE_LEAF_MASK) return SV39_CONFLICT;
        /* Accept only nonleaves emitted by this builder; fail closed on corruption. */
        if ((pte & ~SV39_PTE_PPN_MASK) != SV39_PTE_V) return SV39_BAD_TABLE;
        uint64_t next_pa = (pte >> SV39_PTE_PPN_SHIFT) << SV39_PAGE_SHIFT;
        if (next_pa < b->pool_pa) return SV39_BAD_TABLE;
        uint64_t next = (next_pa - b->pool_pa) >> SV39_PAGE_SHIFT;
        if (!next || next >= b->used || next == index ||
            (level == 1 && next == parent_index)) return SV39_BAD_TABLE;
        parent_index = index; index = (size_t)next;
    }
    size_t leaf_vpn = (va >> SV39_PAGE_SHIFT) & SV39_VPN_MASK;
    if (level == 0 && b->pool[index].entries[leaf_vpn]) return SV39_CONFLICT;
    if (b->capacity - b->used < level) return SV39_POOL_FULL;
    /* No failure paths below: at most two new tables. No hardware reader. */
    for (; level > 0; --level) {
        size_t vpn = (va >> (SV39_PAGE_SHIFT + level * SV39_VPN_BITS)) & SV39_VPN_MASK;
        size_t next = b->used++;
        clear_page(&b->pool[next]);
        b->pool[index].entries[vpn] = table_pte(b, next);
        index = next;
    }
    b->pool[index].entries[leaf_vpn] =
        ((pa >> SV39_PAGE_SHIFT) << SV39_PTE_PPN_SHIFT) | flags | SV39_PTE_V;
    return SV39_OK;
}
