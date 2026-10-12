#include "kernel.h"
#include "mem.h"
#include "proc.h"
#include "vm.h"

/* QEMU virt fallback for early failure only. Normal output uses DT UART. */
static uintptr_t uart_base=0x10000000UL;
volatile struct trap_frame last_boot_trap;
volatile uint64_t boot_hart, boot_dtb, boot_phase;
struct boot_config boot_configuration;
uint64_t boot_time;
/* Kernel-only data (U=0) that the N2 test tries to read from U-mode. */
__attribute__((used)) const volatile uint64_t kernel_secret=0x5ec12e7da7a00001ULL;

static void putc_uart(char c) {
    volatile unsigned char *u=(volatile unsigned char *)uart_base;
    while (!(u[5]&0x20)) __asm__ volatile("nop");
    u[0]=(unsigned char)c;
}
void uart_putc(char c) { putc_uart(c); }
void uart_puts(const char *s) {
    while(*s) { if(*s=='\n') putc_uart('\r'); putc_uart(*s++); }
}
void uart_hex(uint64_t v) {
    static const char digits[]="0123456789abcdef";
    uart_puts("0x");
    for(int shift=60;shift>=0;shift-=4) putc_uart(digits[(v>>shift)&15]);
}
void uart_dec(uint64_t v) {
    char buf[20]; unsigned n=0;
    do { buf[n++]=(char)('0'+v%10); v/=10; } while(v);
    while(n) putc_uart(buf[--n]);
}
/* Out of line on purpose: the single parking point observed by validation. */
__attribute__((noinline)) _Noreturn void halt(void) {
    __asm__ volatile("csrw sie, zero; csrci sstatus, 2" ::: "memory");
    for(;;) __asm__ volatile("wfi");
}
_Noreturn void kernel_panic(const char *why) {
    boot_phase=0xfe;
    uart_puts("KERNEL_PANIC "); uart_puts(why); uart_puts("\n");
    halt();
}
_Noreturn void boot_trap_fatal(const struct trap_frame *f) {
    for(unsigned i=0;i<32;++i) last_boot_trap.x[i]=f->x[i];
    last_boot_trap.sepc=f->sepc; last_boot_trap.sstatus=f->sstatus;
    last_boot_trap.scause=f->scause; last_boot_trap.stval=f->stval;
    boot_phase=0xff;
    uart_puts("KERNEL_PANIC cause="); uart_hex(f->scause);
    uart_puts(" sepc="); uart_hex(f->sepc);
    uart_puts(" stval="); uart_hex(f->stval); uart_puts("\n");
    halt();
}
static int str_equal(const char *a,const char *b) {
    while(*a && *a==*b) { ++a; ++b; } return *a==*b;
}
/* D1: inactive MMU, polling only; no interrupts or additional commands. */
#define CONSOLE_CAPACITY 64u
static void console_command(const char *line) {
    if(str_equal(line,"help")) {
        uart_puts("COMMANDS: help status\n");
    } else if(str_equal(line,"status")) {
        uint64_t satp;
        __asm__ volatile("csrr %0, satp":"=r"(satp));
        uart_puts("STATUS hart="); uart_hex(boot_hart);
        uart_puts(" mode=console privilege=S satp="); uart_hex(satp);
        uart_puts((satp>>60)==0 ? " MMU=off" : " MMU=active");
        uart_puts(" U-mode=not implemented scheduler=not implemented\n");
    } else {
        uart_puts("ERROR unknown command\n");
    }
}
static _Noreturn void console_run(void) {
    char line[CONSOLE_CAPACITY];
    size_t used=0;
    unsigned rejected=0, after_cr=0;
    volatile unsigned char *u=(volatile unsigned char *)uart_base;
    uart_puts("ProtoOS Technical Preview 0.1\nProtoOS console ready\nprotoos> ");
    for(;;) {
        unsigned lsr=u[5];
        /* Overrun/parity/framing/break invalidates the entire input line. */
        if(lsr&0x1e) rejected=2;
        if(!(lsr&1)) { __asm__ volatile("nop"); continue; }
        unsigned char c=u[0];
        if(c=='\n' && after_cr) { after_cr=0; continue; }
        after_cr=(c=='\r');
        if(c=='\r' || c=='\n') {
            uart_puts("\n");
            if(rejected) uart_puts(rejected==1 ? "ERROR line too long\n" : "ERROR invalid input\n");
            else if(used) { line[used]='\0'; console_command(line); }
            used=0; rejected=0;
            uart_puts("protoos> ");
        } else if(!rejected) {
            /* Reject control/non-ASCII bytes instead of silently joining commands. */
            if(c<0x20 || c>0x7e) rejected=2;
            else if(used==sizeof(line)-1) rejected=1;
            else { line[used++]=(char)c; putc_uart((char)c); }
        }
    }
}
/* Scenarios select user entry points and logging only; the kernel image and
 * its privilege/MMU policy are identical for Normal and every Test. */
extern char __user_a_text[], __user_b_text[], __user_prog_text[], __user_prog_text_end[];
extern char a_worker[], a_getpid[], a_yield[], a_exit[], a_cross_write[], a_fault[],
    a_kernel_read[], a_csr[], a_bad_syscall[], a_idle_exit[], a_idle_fault[];
extern char b_worker[], b_getpid[], b_yield[], b_idle_exit[];
extern char a_write[], a_write_bad[], a_sleep[], a_sleep_all[], a_demo[];
extern char b_sleep_all[], b_demo[];
extern char p_shell[], a_read[], a_uptime[], a_read_bad[], a_flood[];
extern char a_mem_test[], a_spawn_test[], a_kill_test[], a_multi_test[], a_spawn_bad[];
enum scenario_kind { SCENARIO_NORMAL, SCENARIO_TEST, SCENARIO_BOOT, SCENARIO_BOOT_TRAP,
    SCENARIO_DEMO, SCENARIO_SHELL };
struct scenario { const char *args; enum scenario_kind kind; char *a, *b; };
static const struct scenario scenarios[]={
    {"protoos.mode=normal",       SCENARIO_NORMAL,    a_worker, b_worker},
    {"protoos.test=boot",         SCENARIO_BOOT,      0, 0},
    {"protoos.test=boot-trap",    SCENARIO_BOOT_TRAP, 0, 0},
    {"protoos.test=umode",        SCENARIO_TEST, a_worker, b_worker},
    {"protoos.test=preempt",      SCENARIO_TEST, a_worker, b_worker},
    {"protoos.test=getpid",       SCENARIO_TEST, a_getpid, b_getpid},
    {"protoos.test=yield",        SCENARIO_TEST, a_yield, b_yield},
    {"protoos.test=exit",         SCENARIO_TEST, a_exit, b_worker},
    {"protoos.test=isolation",    SCENARIO_TEST, a_cross_write, b_worker},
    {"protoos.test=fault",        SCENARIO_TEST, a_fault, b_worker},
    {"protoos.test=kernel-read",  SCENARIO_TEST, a_kernel_read, b_worker},
    {"protoos.test=csr",          SCENARIO_TEST, a_csr, b_worker},
    {"protoos.test=bad-syscall",  SCENARIO_TEST, a_bad_syscall, b_worker},
    {"protoos.test=idle",         SCENARIO_TEST, a_idle_exit, b_idle_exit},
    {"protoos.test=idle-fault",   SCENARIO_TEST, a_idle_fault, b_idle_exit},
    /* v1.1 */
    {"protoos.mode=demo",         SCENARIO_DEMO, a_demo, b_demo},
    {"protoos.test=write",        SCENARIO_TEST, a_write, b_worker},
    {"protoos.test=write-bad",    SCENARIO_TEST, a_write_bad, b_worker},
    {"protoos.test=sleep",        SCENARIO_TEST, a_sleep, b_worker},
    {"protoos.test=sleep-all",    SCENARIO_TEST, a_sleep_all, b_sleep_all},
    /* v1.2 */
    {"protoos.mode=shell",        SCENARIO_SHELL, p_shell, b_worker},
    {"protoos.test=read",         SCENARIO_TEST, a_read, b_worker},
    {"protoos.test=uptime",       SCENARIO_TEST, a_uptime, b_worker},
    {"protoos.test=read-bad",     SCENARIO_TEST, a_read_bad, b_worker},
    {"protoos.test=read-flood",   SCENARIO_TEST, a_flood, b_worker},
    /* v1.3 */
    {"protoos.test=mem",          SCENARIO_TEST, a_mem_test, b_worker},
    {"protoos.test=spawn",        SCENARIO_TEST, a_spawn_test, b_worker},
    {"protoos.test=kill",         SCENARIO_TEST, a_kill_test, b_worker},
    {"protoos.test=multi",        SCENARIO_TEST, a_multi_test, b_worker},
    {"protoos.test=spawn-bad",    SCENARIO_TEST, a_spawn_bad, b_worker},
};
static const struct scenario *find_scenario(const char *args) {
    for(size_t i=0;i<sizeof(scenarios)/sizeof(scenarios[0]);++i)
        if(str_equal(args,scenarios[i].args)) return &scenarios[i];
    return 0;
}
/* User code runs at base + its offset inside the one-page user text image,
 * or (v1.3) in the shared program pages at PROG_VA. */
static uint64_t user_va(uint64_t base, const char *text, const char *entry) {
    if (entry >= __user_prog_text && entry < __user_prog_text_end)
        return PROG_VA + (uint64_t)(entry - __user_prog_text);
    return base + (uint64_t)(entry - text);
}

void kernel_main(uint64_t hart, uintptr_t dtb) {
    __asm__ volatile("csrr %0, time":"=r"(boot_time));
    boot_hart=hart; boot_dtb=dtb; boot_phase=1;
    /* No paging expected at the OpenSBI handoff; confirm instead of guessing. */
    uint64_t satp;
    __asm__ volatile("csrr %0, satp":"=r"(satp));
    if(satp) { uart_puts("KERNEL_PANIC unexpected initial satp\n"); halt(); }
    if(hart!=0 || dtb<0x80000000UL || dtb>0x88000000UL-40) {
        uart_puts("KERNEL_PANIC unsupported boot configuration\n"); halt();
    }
    int err=parse_dtb((const void *)dtb,0x88000000UL-dtb,&boot_configuration);
    if(err || boot_configuration.uart!=0x10000000UL) {
        uart_puts("KERNEL_PANIC DT configuration rejected\n"); halt();
    }
    uart_base=boot_configuration.uart;
    uart_puts("ProtoOS boot foundation: S-mode kernel, Sv39, 2 U-mode processes\n");
    uart_puts("hart="); uart_hex(hart); uart_puts(" dtb=");uart_hex(dtb);
    uart_puts(" timebase="); uart_hex(boot_configuration.timebase_hz);
    uart_puts(" bootargs=");uart_puts(boot_configuration.bootargs);uart_puts("\n");
    boot_phase=2;
    /* D1 demo console predates the MMU and stays a pure polling demo. */
    if(str_equal(boot_configuration.bootargs,"protoos.demo=console"))
        console_run();
    const struct scenario *s=find_scenario(boot_configuration.bootargs);
    if(!s) { uart_puts("KERNEL_PANIC unknown bootargs\n"); halt(); }
    if(boot_configuration.timebase_hz<100) kernel_panic("timebase too low");
    /* v1.3 page pool: up to the 2 MiB boundary below the device tree. */
    uint64_t pool_end=(uint64_t)dtb & ~(uint64_t)0x1fffff;
    if(pool_end>MEM_RAM_END) pool_end=MEM_RAM_END;
    if(pool_end<=MEM_POOL_START) kernel_panic("no memory for the page pool");
    mem_init(pool_end);
    int vm_err=vm_init(pool_end);
    if(vm_err) { uart_puts("KERNEL_PANIC vm_init failed code="); uart_dec((uint64_t)vm_err); uart_puts("\n"); halt(); }
    vm_switch(vm_kernel_satp);
    __asm__ volatile("csrr %0, satp":"=r"(satp));
    if(satp!=vm_kernel_satp) kernel_panic("Sv39 not accepted by satp");
    boot_phase=4;
    uart_puts("MMU Sv39 active satp="); uart_hex(satp); uart_puts("\n");
    if(s->kind==SCENARIO_BOOT_TRAP) __asm__ volatile(".word 0");
    if(s->kind==SCENARIO_BOOT) {
        uart_puts("BOOT_FOUNDATION_READY: S-mode with Sv39 active\n");
        boot_phase=3;
        halt();
    }
    boot_phase=5;
    uart_puts("SCENARIO "); uart_puts(s->args); uart_puts("\n");
    proc_start(user_va(USER_A_BASE,__user_a_text,s->a), user_va(USER_B_BASE,__user_b_text,s->b),
               s->kind==SCENARIO_TEST, s->kind==SCENARIO_NORMAL);
}
