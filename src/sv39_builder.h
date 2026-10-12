#ifndef PROTOOS_SV39_BUILDER_H
#define PROTOOS_SV39_BUILDER_H
#include "sv39.h"
/* Offline, single-owner builder, for inactive tables only. No CSR or fences.
 * Caller supplies a static, contiguous, page-aligned pool and its actual PA.
 * Context and pool must be disjoint live objects, exclusively owned and not
 * externally modified. Host tests use a synthetic PA, never a host pointer.
 * Root is pool[0], at pool_pa. No heap, remap, superpages or batch API.
 * Each call either succeeds fully or leaves context/pool byte-identical.
 */
struct sv39_builder {
    struct sv39_page_table *pool;
    uint64_t pool_pa;
    size_t capacity;
    size_t used;
};
enum sv39_result { SV39_OK, SV39_BAD_ARGUMENT, SV39_BAD_VA,
    SV39_BAD_PA, SV39_BAD_FLAGS, SV39_CONFLICT, SV39_POOL_FULL,
    SV39_BAD_TABLE };
/* init/reinit clears the entire supplied pool, reserving one root page.
 * Caller must stop using any old root before reinitialization. */
enum sv39_result sv39_builder_init(struct sv39_builder *b,
    struct sv39_page_table *pool, size_t pages, uint64_t pool_pa);
/* flags are R/W/X/U/G/A/D (V is added); RSW/extensions rejected.
 * W requires R; at least R or X required. A/D are explicit caller policy.
 * Duplicate mappings, including identical ones, return CONFLICT. */
enum sv39_result sv39_map4k(struct sv39_builder *b,
    uint64_t va, uint64_t pa, uint64_t flags);
#endif
