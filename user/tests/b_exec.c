/* N12: a jump into its data page (R W, not X) -> instruction page fault. */
#include "proto.h"

unsigned int b_exec_code[4] = { 0x00000013, 0x00000013, 0x00008067, 0 };   /* nop; nop; ret */

int main(void)
{
    print("b_exec: jumping into my data...\n");
    ((void (*)(void))(uintptr_t)b_exec_code)();
    return 0;
}
