/* N12: a store into its constant data (R only) -> store page fault. */
#include "proto.h"

const char b_rodata_constant[] = "never written";

int main(void)
{
    print("b_rodata: writing into my constants...\n");
    *(volatile char *)b_rodata_constant = 'X';
    return 0;
}
