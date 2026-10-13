/* N12: a store into its own code page (R X, not W) -> store page fault. */
#include "proto.h"

extern char _start[];

int main(void)
{
    print("b_text: writing into my own code...\n");
    *(volatile char *)_start = 0;
    return 0;
}
