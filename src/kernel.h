#ifndef PROTOOS_KERNEL_H
#define PROTOOS_KERNEL_H
#include "platform.h"

#define csr_read(csr) ({ uint64_t v_; \
    __asm__ volatile("csrr %0, " #csr : "=r"(v_)); v_; })
#define csr_write(csr, v) \
    __asm__ volatile("csrw " #csr ", %0" :: "r"((uint64_t)(v)) : "memory")
#define csr_set(csr, v) \
    __asm__ volatile("csrs " #csr ", %0" :: "r"((uint64_t)(v)) : "memory")
#define csr_clear(csr, v) \
    __asm__ volatile("csrc " #csr ", %0" :: "r"((uint64_t)(v)) : "memory")

/* RISC-V Privileged ISA 1.12, supervisor CSR fields used by ProtoOS. */
#define SSTATUS_SIE  (UINT64_C(1) << 1)
#define SSTATUS_SPIE (UINT64_C(1) << 5)
#define SSTATUS_UBE  (UINT64_C(1) << 6)
#define SSTATUS_SPP  (UINT64_C(1) << 8)
#define SSTATUS_VS   (UINT64_C(3) << 9)
#define SSTATUS_FS   (UINT64_C(3) << 13)
#define SSTATUS_SUM  (UINT64_C(1) << 18)
#define SSTATUS_MXR  (UINT64_C(1) << 19)
#define SIE_STIE     (UINT64_C(1) << 5)
#define SCAUSE_INTERRUPT (UINT64_C(1) << 63)
#define IRQ_S_TIMER  5u
#define EXC_U_ECALL  8u
#define SATP_MODE_SV39 (UINT64_C(8) << 60)

#define UART_PHYS UINT64_C(0x10000000)

extern struct boot_config boot_configuration;
void uart_putc(char c);
void uart_dec(uint64_t value);
_Noreturn void kernel_panic(const char *why);
#endif
