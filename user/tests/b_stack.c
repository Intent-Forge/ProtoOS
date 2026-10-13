/* N12: deep recursion runs off its 16 KiB stack into the guard page below
 * it (never mapped) -> store page fault long before the bound is reached. */
#include "proto.h"

__attribute__((noinline)) static long down(long n)
{
    volatile char frame[512];
    frame[0] = (char)n;
    if (n > 100000) return frame[0];
    return down(n + 1) + frame[0];
}

int main(void)
{
    print("b_stack: deep recursion...\n");
    return (int)down(0);
}
