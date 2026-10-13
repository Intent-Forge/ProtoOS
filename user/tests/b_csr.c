/* N12: writing satp from U-mode -> illegal instruction. */
#include "proto.h"

int main(void)
{
    print("b_csr: writing satp...\n");
    __asm__ volatile("csrw satp, zero");
    return 0;
}
