#include "kernel.h"
#include "input.h"

struct input_state input;

#define PLIC_PRIORITY(irq) (PLIC_PHYS + 4u * (irq))
#define PLIC_S_ENABLE      (PLIC_ENABLE_PAGE + 0x80)   /* context 1, sources 0..31 */
#define PLIC_S_THRESHOLD   (PLIC_CONTEXT_PAGE + 0x0)
#define PLIC_S_CLAIM       (PLIC_CONTEXT_PAGE + 0x4)
#define UART_RBR 0u
#define UART_IER 1u
#define UART_LSR 5u
#define UART_LSR_DR 0x01u
#define UART_IER_RDA 0x01u

static volatile uint32_t *reg32(uint64_t pa)
{
    return (volatile uint32_t *)(uintptr_t)pa;
}

static volatile uint8_t *uart(void)
{
    return (volatile uint8_t *)(uintptr_t)UART_PHYS;
}

void input_init(void)
{
    /* Bytes that arrived before the shell existed are not input to it. */
    while (uart()[UART_LSR] & UART_LSR_DR)
        (void)uart()[UART_RBR];
    *reg32(PLIC_PRIORITY(UART_IRQ)) = 1;
    *reg32(PLIC_S_ENABLE) = UINT32_C(1) << UART_IRQ;
    *reg32(PLIC_S_THRESHOLD) = 0;
    uart()[UART_IER] = UART_IER_RDA;
}

void input_irq(void)
{
    uint32_t id = *reg32(PLIC_S_CLAIM);
    if (id == 0) return;                 /* already claimed / nothing pending */
    input.irqs++;
    if (id != UART_IRQ) {
        input.spurious++;
    } else {
        while (uart()[UART_LSR] & UART_LSR_DR) {
            uint8_t c = uart()[UART_RBR];
            input.received++;
            if (c == INPUT_INTR && input.intr_armed) {
                input.intr_pending = 1;
                input.intrs++;
                continue;
            }
            /* Full buffer: keep the oldest bytes, drop the new one. */
            if (input.count == INPUT_CAPACITY) { input.dropped++; continue; }
            input.buf[(input.head + input.count) % INPUT_CAPACITY] = c;
            input.count++;
        }
    }
    *reg32(PLIC_S_CLAIM) = id;           /* complete */
}

uint64_t input_take_user(uint64_t dst, uint64_t max)
{
    uint64_t n = input.count < max ? input.count : max;
    csr_set(sstatus, SSTATUS_SUM);
    for (uint64_t i = 0; i < n; ++i)
        *(volatile uint8_t *)(uintptr_t)(dst + i) =
            input.buf[(input.head + i) % INPUT_CAPACITY];
    csr_clear(sstatus, SSTATUS_SUM);
    input.head = (input.head + n) % INPUT_CAPACITY;
    input.count -= n;
    return n;
}
