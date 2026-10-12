#ifndef PROTOOS_SBI_H
#define PROTOOS_SBI_H
#include <stdint.h>

/* RISC-V SBI v2.0 calling convention: a7=EID, a6=FID, a0..a5 args,
 * returns error in a0 and value in a1. */
#define SBI_EXT_TIME UINT64_C(0x54494D45)
#define SBI_TIME_SET_TIMER UINT64_C(0)

struct sbiret { long error; long value; };

static inline struct sbiret sbi_call1(uint64_t eid, uint64_t fid, uint64_t arg0)
{
    register uint64_t a0 __asm__("a0") = arg0;
    register uint64_t a1 __asm__("a1") = 0;
    register uint64_t a6 __asm__("a6") = fid;
    register uint64_t a7 __asm__("a7") = eid;
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
    return (struct sbiret){ (long)a0, (long)a1 };
}
#endif
