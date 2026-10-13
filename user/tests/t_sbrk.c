/* T19 test program: the heap. Exit code 0 if every check passes, else the
 * number of the first failed check. The two checkpoints let the harness
 * look at the page tables and the allocator. */
#include "proto.h"

extern char _end[];

__attribute__((noinline)) void t_sbrk_checkpoint1(long brk)
{
    __asm__ volatile("" :: "r"(brk) : "memory");
}

__attribute__((noinline)) void t_sbrk_checkpoint2(long brk, long steps)
{
    __asm__ volatile("" :: "r"(brk), "r"(steps) : "memory");
}

int main(void)
{
    long base = (long)_end;
    if (sys_sbrk(0) != base) return 1;
    if (sys_sbrk(10000) != base) return 2;
    volatile unsigned char *h = (volatile unsigned char *)base;
    for (long i = 0; i < 10000; ++i) {
        if (h[i]) return 3;
        h[i] = (unsigned char)(i * 7 + 1);
    }
    if (sys_sbrk(0) != base + 10000) return 4;
    t_sbrk_checkpoint1(base + 10000);
    if (sys_sbrk(-4096) != E_INVAL) return 5;
    if (sys_sbrk(1L << 40) != E_NOMEM) return 6;
    if (sys_sbrk(0) != base + 10000) return 7;
    long steps = 0;
    for (;;) {
        long old = sys_sbrk(4096);
        if (old == E_NOMEM) break;
        if (old < 0 || steps > 1000) return 8;
        volatile long *w = (volatile long *)old;
        for (long k = 0; k < 4096 / 8; ++k) {
            if (w[k]) return 9;
            w[k] = old + k;
        }
        ++steps;
    }
    for (long i = 0; i < 10000; ++i)
        if (h[i] != (unsigned char)(i * 7 + 1)) return 10;
    long brk = sys_sbrk(0);
    if (brk != base + 10000 + steps * 4096) return 11;
    t_sbrk_checkpoint2(brk, steps);
    if (sys_sbrk(4096) != E_NOMEM || sys_sbrk(0) != brk) return 12;
    print("t_sbrk: all checks passed\n");
    return 0;
}
