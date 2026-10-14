#include "kernel.h"
#include "archive.h"
#include "disk.h"
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
extern char p_t_elf[], p_t_sbrk[], p_t_wait[], p_t_ctrlc[], p_t_elfbad[], p_t_arcbad[],
    p_t_userbad[];
enum scenario_kind { SCENARIO_NORMAL, SCENARIO_TEST, SCENARIO_BOOT, SCENARIO_BOOT_TRAP,
    SCENARIO_DEMO, SCENARIO_SHELL, SCENARIO_AUTORUN };
/* v1.6: protoos.run=<program> [argument] is the one AUTORUN scenario: it also
 * starts that program at boot through the path of run_arg (archive, then
 * disk); A is the shell, idle in read(). */
struct scenario { const char *args; enum scenario_kind kind; char *a, *b; const char *autorun; };
static const struct scenario scenarios[]={
    {"protoos.mode=normal",       SCENARIO_NORMAL,    a_worker, b_worker, 0},
    {"protoos.test=boot",         SCENARIO_BOOT,      0, 0, 0},
    {"protoos.test=boot-trap",    SCENARIO_BOOT_TRAP, 0, 0, 0},
    {"protoos.test=umode",        SCENARIO_TEST, a_worker, b_worker, 0},
    {"protoos.test=preempt",      SCENARIO_TEST, a_worker, b_worker, 0},
    {"protoos.test=getpid",       SCENARIO_TEST, a_getpid, b_getpid, 0},
    {"protoos.test=yield",        SCENARIO_TEST, a_yield, b_yield, 0},
    {"protoos.test=exit",         SCENARIO_TEST, a_exit, b_worker, 0},
    {"protoos.test=isolation",    SCENARIO_TEST, a_cross_write, b_worker, 0},
    {"protoos.test=fault",        SCENARIO_TEST, a_fault, b_worker, 0},
    {"protoos.test=kernel-read",  SCENARIO_TEST, a_kernel_read, b_worker, 0},
    {"protoos.test=csr",          SCENARIO_TEST, a_csr, b_worker, 0},
    {"protoos.test=bad-syscall",  SCENARIO_TEST, a_bad_syscall, b_worker, 0},
    {"protoos.test=idle",         SCENARIO_TEST, a_idle_exit, b_idle_exit, 0},
    {"protoos.test=idle-fault",   SCENARIO_TEST, a_idle_fault, b_idle_exit, 0},
    /* v1.1 */
    {"protoos.mode=demo",         SCENARIO_DEMO, a_demo, b_demo, 0},
    {"protoos.test=write",        SCENARIO_TEST, a_write, b_worker, 0},
    {"protoos.test=write-bad",    SCENARIO_TEST, a_write_bad, b_worker, 0},
    {"protoos.test=sleep",        SCENARIO_TEST, a_sleep, b_worker, 0},
    {"protoos.test=sleep-all",    SCENARIO_TEST, a_sleep_all, b_sleep_all, 0},
    /* v1.2 */
    {"protoos.mode=shell",        SCENARIO_SHELL, p_shell, b_worker, 0},
    {"protoos.test=read",         SCENARIO_TEST, a_read, b_worker, 0},
    {"protoos.test=uptime",       SCENARIO_TEST, a_uptime, b_worker, 0},
    {"protoos.test=read-bad",     SCENARIO_TEST, a_read_bad, b_worker, 0},
    {"protoos.test=read-flood",   SCENARIO_TEST, a_flood, b_worker, 0},
    /* v1.3 */
    {"protoos.test=mem",          SCENARIO_TEST, a_mem_test, b_worker, 0},
    {"protoos.test=spawn",        SCENARIO_TEST, a_spawn_test, b_worker, 0},
    {"protoos.test=kill",         SCENARIO_TEST, a_kill_test, b_worker, 0},
    {"protoos.test=multi",        SCENARIO_TEST, a_multi_test, b_worker, 0},
    {"protoos.test=spawn-bad",    SCENARIO_TEST, a_spawn_bad, b_worker, 0},
    /* v1.4: booted with a program archive (QEMU -initrd) */
    {"protoos.test=elf",          SCENARIO_TEST, p_t_elf, b_worker, 0},
    {"protoos.test=sbrk",         SCENARIO_TEST, p_t_sbrk, b_worker, 0},
    {"protoos.test=wait",         SCENARIO_TEST, p_t_wait, b_worker, 0},
    {"protoos.test=ctrlc",        SCENARIO_TEST, p_t_ctrlc, b_worker, 0},
    {"protoos.test=elf-bad",      SCENARIO_TEST, p_t_elfbad, b_worker, 0},
    {"protoos.test=archive-bad",  SCENARIO_TEST, p_t_arcbad, b_worker, 0},
    {"protoos.test=user-bad",     SCENARIO_TEST, p_t_userbad, b_worker, 0},
};
/* v1.6: "protoos.run=<name>" or "protoos.run=<name> <argument>": a name of
 * 1-15 characters without spaces, an argument of at most ARG_MAX bytes (the
 * rest of bootargs). Anything else is unknown bootargs. */
static char run_name[16];
static struct scenario run_scenario={"protoos.run", SCENARIO_AUTORUN, p_shell, b_worker, run_name};
static const char *run_arg_text; static uint64_t run_arg_len;
static const struct scenario *parse_run(const char *args) {
    static const char key[]="protoos.run=";
    for(size_t i=0;i<sizeof(key)-1;++i) if(args[i]!=key[i]) return 0;
    const char *p=args+sizeof(key)-1;
    uint64_t n=0;
    while(p[n] && p[n]!=' ') ++n;
    if(n==0 || n>15) return 0;
    for(uint64_t i=0;i<n;++i) run_name[i]=p[i];
    run_name[n]=0;
    run_arg_text=p[n] ? p+n+1 : p+n;
    run_arg_len=0;
    while(run_arg_text[run_arg_len]) ++run_arg_len;
    if(p[n] && run_arg_len==0) return 0;            /* "name " with nothing after it */
    if(run_arg_len>ARG_MAX) return 0;
    return &run_scenario;
}
static const struct scenario *find_scenario(const char *args) {
    for(size_t i=0;i<sizeof(scenarios)/sizeof(scenarios[0]);++i)
        if(str_equal(args,scenarios[i].args)) return &scenarios[i];
    return parse_run(args);
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
    /* v1.4: the program archive the boot loader placed in RAM (QEMU puts
     * -initrd inside the pool): reserve its pages for good, then validate
     * all of it once. Without one, nothing changes. */
    const struct boot_config *bc=&boot_configuration;
    if(bc->initrd_found) {
        if(bc->initrd_found!=3u ||
           !archive_range_ok(bc->initrd_start,bc->initrd_end,MEM_POOL_START,pool_end)) {
            uart_puts("ARCHIVE ignored: not inside the page pool\n");
        } else {
            uint64_t pages=mem_reserve(bc->initrd_start,bc->initrd_end);
            int aerr=archive_parse((const uint8_t *)(uintptr_t)bc->initrd_start,
                                   bc->initrd_end-bc->initrd_start,&prog_archive);
            uart_puts("ARCHIVE "); uart_dec(bc->initrd_end-bc->initrd_start);
            uart_puts(" bytes at "); uart_hex(bc->initrd_start);
            uart_puts(", "); uart_dec(pages); uart_puts(" pages reserved: ");
            if(aerr) { uart_puts("rejected code=-"); uart_dec((uint64_t)-(int64_t)aerr); }
            else { prog_archive_pa=bc->initrd_start; uart_dec(prog_archive.count); uart_puts(" programs"); }
            uart_puts("\n");
        }
    }
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
    /* v1.5: the disk, if there is one (nothing printed without one) */
    disk_init();
    boot_phase=5;
    uart_puts("SCENARIO "); uart_puts(s->kind==SCENARIO_AUTORUN ? bc->bootargs : s->args); uart_puts("\n");
    proc_start(user_va(USER_A_BASE,__user_a_text,s->a), user_va(USER_B_BASE,__user_b_text,s->b),
               s->kind==SCENARIO_TEST, s->kind==SCENARIO_NORMAL,
               s->kind==SCENARIO_AUTORUN ? s->autorun : 0, run_arg_text, run_arg_len);
}
