/* N12: a load from the kernel image (mapped U=0) -> load page fault. */
#include "proto.h"

int main(void)
{
    print("b_kernel: reading kernel memory...\n");
    return (int)*(volatile long *)0x80200000ul;
}
