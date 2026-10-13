/* N12: a floating-point instruction with FP off in U-mode -> illegal
 * instruction (the program is built without FP; the encoding is raw). */
#include "proto.h"

int main(void)
{
    print("b_fpu: a floating-point add...\n");
    __asm__ volatile(".word 0x00000053");      /* fadd.s ft0, ft0, ft0 */
    return 0;
}
