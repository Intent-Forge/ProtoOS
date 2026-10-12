#include "kernel.h"
#include "input.h"
#include "sv39_builder.h"
#include "vm.h"

/* Static page-table pools: root, low L1, UART L0, two PLIC L0 (v1.2),
 * user L0, high L1, kernel L0 = 8 pages per address space; 10 leaves headroom. */
#define AS_POOL_PAGES 10u
static struct sv39_page_table kernel_pt_pool[AS_POOL_PAGES];
static struct sv39_page_table proc_pt_pool[PROC_COUNT][AS_POOL_PAGES];
static struct sv39_builder kernel_as, proc_as[PROC_COUNT];
uint64_t vm_kernel_satp;
uint64_t vm_proc_satp[PROC_COUNT];

extern char __kernel_start[], __text_end[], __rodata_end[], __kernel_end[];
extern char kernel_stack_guard[];
extern char __user_a_text[], __user_b_text[];
extern char user_a_data[], user_a_stack[], user_b_data[], user_b_stack[];

/* Kernel pages never carry U; user pages always do. A/D are preset so the
 * result does not depend on optional hardware A/D updating (Svadu). */
#define PTE_KTEXT (SV39_PTE_R | SV39_PTE_X | SV39_PTE_A)
#define PTE_KRO   (SV39_PTE_R | SV39_PTE_A)
#define PTE_KRW   (SV39_PTE_R | SV39_PTE_W | SV39_PTE_A | SV39_PTE_D)
#define PTE_UTEXT (SV39_PTE_R | SV39_PTE_X | SV39_PTE_U | SV39_PTE_A)
#define PTE_URW   (SV39_PTE_R | SV39_PTE_W | SV39_PTE_U | SV39_PTE_A | SV39_PTE_D)

static enum sv39_result map_identity(struct sv39_builder *b, uintptr_t start,
    uintptr_t end, uint64_t flags)
{
    for (uintptr_t pa = start; pa < end; pa += SV39_PAGE_SIZE) {
        if (pa == (uintptr_t)kernel_stack_guard) continue;
        enum sv39_result r = sv39_map4k(b, pa, pa, flags);
        if (r != SV39_OK) return r;
    }
    return SV39_OK;
}

static enum sv39_result map_kernel(struct sv39_builder *b)
{
    enum sv39_result r;
    if ((r = map_identity(b, (uintptr_t)__kernel_start, (uintptr_t)__text_end, PTE_KTEXT)) ||
        (r = map_identity(b, (uintptr_t)__text_end, (uintptr_t)__rodata_end, PTE_KRO)) ||
        (r = map_identity(b, (uintptr_t)__rodata_end, (uintptr_t)__kernel_end, PTE_KRW)) ||
        (r = sv39_map4k(b, UART_PHYS, UART_PHYS, PTE_KRW)))
        return r;
    /* v1.2: the three PLIC pages the kernel touches (priority, S enable,
     * S threshold/claim); kernel-only like the UART. */
    if ((r = sv39_map4k(b, PLIC_PHYS, PLIC_PHYS, PTE_KRW)) ||
        (r = sv39_map4k(b, PLIC_ENABLE_PAGE, PLIC_ENABLE_PAGE, PTE_KRW)))
        return r;
    return sv39_map4k(b, PLIC_CONTEXT_PAGE, PLIC_CONTEXT_PAGE, PTE_KRW);
}

static enum sv39_result map_user(struct sv39_builder *b, uint64_t base,
    const char *text, const char *data, const char *stack)
{
    enum sv39_result r;
    if ((r = sv39_map4k(b, base + USER_TEXT_OFF, (uintptr_t)text, PTE_UTEXT)) ||
        (r = sv39_map4k(b, base + USER_DATA_OFF, (uintptr_t)data, PTE_URW)))
        return r;
    return sv39_map4k(b, base + USER_STACK_OFF, (uintptr_t)stack, PTE_URW);
}

static uint64_t satp_for(const struct sv39_builder *b)
{
    /* ASID 0 everywhere: every switch flushes the whole TLB instead. */
    return SATP_MODE_SV39 | (b->pool_pa >> SV39_PAGE_SHIFT);
}

int vm_init(void)
{
    /* MMU is off here, so table virtual addresses are physical. */
    if (sv39_builder_init(&kernel_as, kernel_pt_pool, AS_POOL_PAGES,
            (uintptr_t)kernel_pt_pool) || map_kernel(&kernel_as))
        return 1;
    static const uint64_t bases[PROC_COUNT] = { USER_A_BASE, USER_B_BASE };
    const char *text[PROC_COUNT] = { __user_a_text, __user_b_text };
    const char *data[PROC_COUNT] = { user_a_data, user_b_data };
    const char *stack[PROC_COUNT] = { user_a_stack, user_b_stack };
    for (unsigned i = 0; i < PROC_COUNT; ++i) {
        if (sv39_builder_init(&proc_as[i], proc_pt_pool[i], AS_POOL_PAGES,
                (uintptr_t)proc_pt_pool[i]) || map_kernel(&proc_as[i]) ||
            map_user(&proc_as[i], bases[i], text[i], data[i], stack[i]))
            return 2 + (int)i;
        vm_proc_satp[i] = satp_for(&proc_as[i]);
    }
    vm_kernel_satp = satp_for(&kernel_as);
    return 0;
}

/* Leaf PTE for va in an address space built by this file, or 0. Tables are
 * in kernel memory and identity mapped, so their PAs are usable pointers. */
static sv39_pte_t leaf_pte(const struct sv39_builder *b, uint64_t va)
{
    const struct sv39_page_table *table = (const struct sv39_page_table *)(uintptr_t)b->pool_pa;
    for (unsigned level = 2;; --level) {
        sv39_pte_t pte = table->entries[(va >> (SV39_PAGE_SHIFT + level * SV39_VPN_BITS)) & SV39_VPN_MASK];
        if (!(pte & SV39_PTE_V)) return 0;
        if (pte & SV39_PTE_LEAF_MASK) return level == 0 ? pte : 0;  /* no superpages here */
        if (level == 0) return 0;
        table = (const struct sv39_page_table *)(uintptr_t)
            ((pte >> SV39_PTE_PPN_SHIFT) << SV39_PAGE_SHIFT);
    }
}

int vm_user_range_ok(unsigned proc, uint64_t va, uint64_t len, uint64_t need)
{
    if (proc >= PROC_COUNT) return 0;
    if (len == 0) return 1;
    /* User space is the lower canonical half; reject wrap-around. */
    if (va >= (UINT64_C(1) << 38) || len > (UINT64_C(1) << 38) - va) return 0;
    uint64_t want = SV39_PTE_V | SV39_PTE_U | need;
    for (uint64_t page = va & ~SV39_PAGE_OFFSET_MASK; page < va + len; page += SV39_PAGE_SIZE)
        if ((leaf_pte(&proc_as[proc], page) & want) != want) return 0;
    return 1;
}

void vm_switch(uint64_t satp)
{
    /* Order prior page-table stores before the walk, then drop stale TLB. */
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
    csr_write(satp, satp);
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
}
