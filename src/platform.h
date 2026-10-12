#ifndef PROTOOS_PLATFORM_H
#define PROTOOS_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
struct boot_config {
    uintptr_t uart;
    uint32_t timebase_hz;
    char bootargs[128];
};
/* Parse only the DT subset needed for QEMU virt. Reject malformed input. */
int parse_dtb(const void *blob, size_t available, struct boot_config *out);
struct trap_frame {
    uint64_t x[32];
    uint64_t sepc, sstatus, scause, stval;
};
_Static_assert(sizeof(struct trap_frame)==288, "trap.S frame size");
_Static_assert(offsetof(struct trap_frame,sepc)==256, "trap.S CSR offset");
void uart_puts(const char *s);
void uart_hex(uint64_t value);
_Noreturn void halt(void);
#endif
