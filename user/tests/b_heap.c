/* N12: a store just past the heap's last page -> store page fault. */
#include "proto.h"

int main(void)
{
    print("b_heap: writing past the end of my heap...\n");
    long base = sys_sbrk(100);
    if (base < 0) return 1;
    *(volatile char *)(base + 4096) = 1;
    return 0;
}
