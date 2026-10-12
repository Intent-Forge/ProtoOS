.option norvc
.section .text.boot,"ax",@progbits
.globl _start
_start:
    csrw sie, zero
    csrci sstatus, 2
    csrw sscratch, zero
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, boot_stack_top
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 1b
    /* User data/stack pages are NOLOAD and outside .bss: clear them too. */
2:  la t0, __user_bss_start
    la t1, __user_bss_end
3:  bgeu t0, t1, 4f
    sd zero, 0(t0)
    addi t0, t0, 8
    j 3b
    /* sscratch==0 marks "trap taken in S-mode" for trap_entry. */
4:  la t0, trap_entry
    csrw stvec, t0
    /* a0=hart ID, a1=DTB physical address from OpenSBI. */
    call kernel_main
5:  wfi
    j 5b

.section .bss.kstack,"aw",@nobits
.balign 4096
/* Left unmapped once Sv39 is active: kernel stack overflow faults. */
.globl kernel_stack_guard
kernel_stack_guard:
    .skip 4096
.globl boot_stack_bottom
boot_stack_bottom:
    .skip 16384
.globl boot_stack_top
boot_stack_top:
/* Used only by the fatal S-mode trap path, so a corrupt or overflowed
 * kernel sp still produces a KERNEL_PANIC report. */
.globl trap_emergency_stack_bottom
trap_emergency_stack_bottom:
    .skip 4096
.globl trap_emergency_stack_top
trap_emergency_stack_top:
