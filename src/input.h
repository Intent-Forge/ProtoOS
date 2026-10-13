#ifndef PROTOOS_INPUT_H
#define PROTOOS_INPUT_H
#include <stdint.h>

/* v1.2 console input: UART receive interrupt through the PLIC into a fixed
 * kernel ring buffer. QEMU virt only: the PLIC is at PLIC_PHYS and the UART
 * is PLIC source UART_IRQ; hart 0 S-mode is PLIC context 1. */
#define PLIC_PHYS          UINT64_C(0x0c000000)
#define PLIC_ENABLE_PAGE   (PLIC_PHYS + 0x2000)
#define PLIC_CONTEXT_PAGE  (PLIC_PHYS + 0x201000)
#define UART_IRQ           10u
#define INPUT_CAPACITY     128u
#define INPUT_INTR         0x03u    /* v1.4: Ctrl-C */

struct input_state {
    uint64_t count;     /* bytes waiting in buf */
    uint64_t head;      /* index of the oldest byte */
    uint64_t irqs;      /* external interrupts serviced (trap or wait loop) */
    uint64_t received;  /* bytes read from the UART */
    uint64_t dropped;   /* bytes discarded because buf was full */
    uint64_t spurious;  /* claims that were not the UART */
    /* v1.4: while a foreground wait() is armed, Ctrl-C is not input: it
     * sets intr_pending and the scheduler ends the foreground process. */
    uint64_t intr_armed, intr_pending, intrs;
    uint8_t buf[INPUT_CAPACITY];
};
extern struct input_state input;

/* Enable the UART receive interrupt in the UART and the PLIC (not sie). */
void input_init(void);
/* Claim, drain the UART into the buffer, complete. Never blocks. */
void input_irq(void);
/* Move up to max buffered bytes to user VA dst; caller has validated the
 * range in the current address space. Returns the number of bytes. */
uint64_t input_take_user(uint64_t dst, uint64_t max);
#endif
