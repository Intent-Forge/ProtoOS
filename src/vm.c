#include "kernel.h"
#include "input.h"
#include "mem.h"
#include "sv39_builder.h"
#include "vm.h"

/* Static page-table pools: root, low L1, UART L0, two PLIC L0 (v1.2),
 * program L0 (v1.3), user L0, high L1, kernel L0 = 9 pages per address
 * space; 10 leaves headroom. The pool direct map uses 2 MiB leaves in the
 * high L1 and needs no extra tables. */
#define AS_POOL_PAGES 10u
static struct sv39_page_table kernel_pt_pool[AS_POOL_PAGES];
static struct sv39_page_table template_pt_pool[AS_POOL_PAGES];
static struct sv39_page_table proc_pt_pool[PROC_STATIC][AS_POOL_PAGES];
static struct sv39_builder kernel_as, template_as, proc_as[PROC_STATIC];
uint64_t vm_kernel_satp;
uint64_t vm_proc_satp[PROC_STATIC];

extern char __kernel_start[], __text_end[], __rodata_end[], __kernel_end[];
extern char kernel_stack_guard[];
extern char __user_a_text[], __user_b_text[], __user_prog_text[];
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

static enum sv39_result map_kernel(struct sv39_builder *b, uint64_t pool_end)
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
        (r = sv39_map4k(b, PLIC_ENABLE_PAGE, PLIC_ENABLE_PAGE, PTE_KRW)) ||
        (r = sv39_map4k(b, PLIC_CONTEXT_PAGE, PLIC_CONTEXT_PAGE, PTE_KRW)))
        return r;
    /* v1.3: the page pool, kernel-only, so the kernel can build and clear
     * pages of run-time processes in any address space. */
    for (uint64_t pa = MEM_POOL_START; pa < pool_end; pa += SV39_MEGA_SIZE)
        if ((r = sv39_map2m(b, pa, pa, PTE_KRW))) return r;
    return SV39_OK;
}

static enum sv39_result map_prog(struct sv39_builder *b)
{
    for (unsigned i = 0; i < PROG_PAGES; ++i) {
        enum sv39_result r = sv39_map4k(b, PROG_VA + i * SV39_PAGE_SIZE,
            (uintptr_t)__user_prog_text + i * SV39_PAGE_SIZE, PTE_UTEXT);
        if (r != SV39_OK) return r;
    }
    return SV39_OK;
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

static uint64_t satp_for_root(uint64_t root_pa)
{
    /* ASID 0 everywhere: every switch flushes the whole TLB instead. */
    return SATP_MODE_SV39 | (root_pa >> SV39_PAGE_SHIFT);
}

int vm_init(uint64_t pool_end)
{
    /* MMU is off here, so table virtual addresses are physical. */
    if (sv39_builder_init(&kernel_as, kernel_pt_pool, AS_POOL_PAGES,
            (uintptr_t)kernel_pt_pool) || map_kernel(&kernel_as, pool_end))
        return 1;
    /* The template is never installed: run-time roots copy its entries. */
    if (sv39_builder_init(&template_as, template_pt_pool, AS_POOL_PAGES,
            (uintptr_t)template_pt_pool) || map_kernel(&template_as, pool_end) ||
        map_prog(&template_as))
        return 4;
    static const uint64_t bases[PROC_STATIC] = { USER_A_BASE, USER_B_BASE };
    const char *text[PROC_STATIC] = { __user_a_text, __user_b_text };
    const char *data[PROC_STATIC] = { user_a_data, user_b_data };
    const char *stack[PROC_STATIC] = { user_a_stack, user_b_stack };
    for (unsigned i = 0; i < PROC_STATIC; ++i) {
        if (sv39_builder_init(&proc_as[i], proc_pt_pool[i], AS_POOL_PAGES,
                (uintptr_t)proc_pt_pool[i]) || map_kernel(&proc_as[i], pool_end) ||
            map_prog(&proc_as[i]) ||
            map_user(&proc_as[i], bases[i], text[i], data[i], stack[i]))
            return 2 + (int)i;
        vm_proc_satp[i] = satp_for_root(proc_as[i].pool_pa);
    }
    vm_kernel_satp = satp_for_root(kernel_as.pool_pa);
    return 0;
}

static const sv39_pte_t *table_at(uint64_t pa)
{
    return (const sv39_pte_t *)(uintptr_t)pa;
}

static uint64_t pte_pa(sv39_pte_t pte)
{
    return (pte >> SV39_PTE_PPN_SHIFT) << SV39_PAGE_SHIFT;
}

/* Leaf PTE of a 4 KiB page for va under root_pa, or 0. Every table the
 * kernel built is reachable at its PA (static tables in the identity-mapped
 * kernel image, run-time tables in the pool direct map). */
static sv39_pte_t leaf_pte(uint64_t root_pa, uint64_t va)
{
    const sv39_pte_t *table = table_at(root_pa);
    for (unsigned level = 2;; --level) {
        sv39_pte_t pte = table[(va >> (SV39_PAGE_SHIFT + level * SV39_VPN_BITS)) & SV39_VPN_MASK];
        if (!(pte & SV39_PTE_V)) return 0;
        if (pte & SV39_PTE_LEAF_MASK) return level == 0 ? pte : 0;  /* 2 MiB: kernel only */
        if (level == 0) return 0;
        table = table_at(pte_pa(pte));
    }
}

int vm_user_range_ok(uint64_t satp, uint64_t va, uint64_t len, uint64_t need)
{
    if (len == 0) return 1;
    /* User space is the lower canonical half; reject wrap-around. */
    if (va >= (UINT64_C(1) << 38) || len > (UINT64_C(1) << 38) - va) return 0;
    uint64_t root_pa = (satp & SV39_PPN_MASK) << SV39_PAGE_SHIFT;
    uint64_t want = SV39_PTE_V | SV39_PTE_U | need;
    for (uint64_t page = va & ~SV39_PAGE_OFFSET_MASK; page < va + len; page += SV39_PAGE_SIZE)
        if ((leaf_pte(root_pa, page) & want) != want) return 0;
    return 1;
}

static sv39_pte_t table_entry(uint64_t pa)
{
    return ((pa >> SV39_PAGE_SHIFT) << SV39_PTE_PPN_SHIFT) | SV39_PTE_V;
}

static sv39_pte_t leaf_entry(uint64_t pa, uint64_t flags)
{
    return ((pa >> SV39_PAGE_SHIFT) << SV39_PTE_PPN_SHIFT) | flags | SV39_PTE_V;
}

static size_t vpn(uint64_t va, unsigned level)
{
    return (size_t)((va >> (SV39_PAGE_SHIFT + level * SV39_VPN_BITS)) & SV39_VPN_MASK);
}

uint64_t vm_create(uint64_t base, uint64_t pages[VM_PROC_PAGES])
{
    for (unsigned i = 0; i < VM_PROC_PAGES; ++i) {
        if (!(pages[i] = mem_alloc())) {
            while (i--) mem_free(pages[i]);
            return 0;
        }
    }
    /* Pages arrive zeroed. Root and low L1 start as copies of the template
     * (shared kernel and program tables, all with U=0 except the program
     * pages); only the low L1 slot of this process's user L0 is private. */
    volatile sv39_pte_t *root = (volatile sv39_pte_t *)(uintptr_t)pages[0];
    volatile sv39_pte_t *l1 = (volatile sv39_pte_t *)(uintptr_t)pages[1];
    volatile sv39_pte_t *l0 = (volatile sv39_pte_t *)(uintptr_t)pages[2];
    const sv39_pte_t *troot = table_at(template_as.pool_pa);
    const sv39_pte_t *tl1 = table_at(pte_pa(troot[vpn(base, 2)]));
    for (unsigned e = 0; e < SV39_PT_ENTRIES; ++e) {
        root[e] = troot[e];
        l1[e] = tl1[e];
    }
    if (tl1[vpn(base, 1)]) kernel_panic("user slot collides with a template mapping");
    root[vpn(base, 2)] = table_entry(pages[1]);
    l1[vpn(base, 1)] = table_entry(pages[2]);
    l0[vpn(base + USER_DATA_OFF, 0)] = leaf_entry(pages[3], PTE_URW);
    l0[vpn(base + USER_STACK_OFF, 0)] = leaf_entry(pages[4], PTE_URW);
    return satp_for_root(pages[0]);
}

void vm_destroy(const uint64_t pages[VM_PROC_PAGES])
{
    for (unsigned i = 0; i < VM_PROC_PAGES; ++i)
        mem_free(pages[i]);
}

void vm_switch(uint64_t satp)
{
    /* Order prior page-table stores before the walk, then drop stale TLB. */
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
    csr_write(satp, satp);
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
}
