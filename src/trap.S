/* Single S-mode trap vector.
 *
 * sscratch protocol: while a process runs in U-mode, sscratch holds the
 * address of its struct trap_frame (struct process starts with it); while
 * the kernel runs, sscratch is 0. The kernel never enables sstatus.SIE, so
 * a trap with sscratch==0 is a synchronous S-mode exception: fatal.
 * Frame layout must match struct trap_frame in platform.h.
 */
.option norvc
.section .text,"ax",@progbits
.balign 4
.globl trap_entry
trap_entry:
    csrrw t6, sscratch, t6
    beqz t6, kernel_trap
    /* From U-mode: t6 = frame, sscratch = user t6. */
    sd ra, 8(t6)
    sd sp, 16(t6)
    sd gp, 24(t6)
    sd tp, 32(t6)
    sd t0, 40(t6)
    sd t1, 48(t6)
    sd t2, 56(t6)
    sd s0, 64(t6)
    sd s1, 72(t6)
    sd a0, 80(t6)
    sd a1, 88(t6)
    sd a2, 96(t6)
    sd a3, 104(t6)
    sd a4, 112(t6)
    sd a5, 120(t6)
    sd a6, 128(t6)
    sd a7, 136(t6)
    sd s2, 144(t6)
    sd s3, 152(t6)
    sd s4, 160(t6)
    sd s5, 168(t6)
    sd s6, 176(t6)
    sd s7, 184(t6)
    sd s8, 192(t6)
    sd s9, 200(t6)
    sd s10, 208(t6)
    sd s11, 216(t6)
    sd t3, 224(t6)
    sd t4, 232(t6)
    sd t5, 240(t6)
    csrr t0, sscratch
    sd t0, 248(t6)
    csrw sscratch, zero
    csrr t0, sepc
    sd t0, 256(t6)
    csrr t0, sstatus
    sd t0, 264(t6)
    csrr t0, scause
    sd t0, 272(t6)
    csrr t0, stval
    sd t0, 280(t6)
    /* Every U-mode trap starts on an empty kernel stack: the kernel never
     * blocks inside a trap, so no kernel state lives across traps. */
    la sp, boot_stack_top
    mv a0, t6
    call trap_from_user
    /* fall through with a0 = frame of the process to run next */

/* _Noreturn void user_return(struct trap_frame *f) */
.globl user_return
user_return:
    ld t0, 256(a0)
    csrw sepc, t0
    ld t0, 264(a0)
    csrw sstatus, t0
    csrw sscratch, a0
    ld ra, 8(a0)
    ld sp, 16(a0)
    ld gp, 24(a0)
    ld tp, 32(a0)
    ld t0, 40(a0)
    ld t1, 48(a0)
    ld t2, 56(a0)
    ld s0, 64(a0)
    ld s1, 72(a0)
    ld a1, 88(a0)
    ld a2, 96(a0)
    ld a3, 104(a0)
    ld a4, 112(a0)
    ld a5, 120(a0)
    ld a6, 128(a0)
    ld a7, 136(a0)
    ld s2, 144(a0)
    ld s3, 152(a0)
    ld s4, 160(a0)
    ld s5, 168(a0)
    ld s6, 176(a0)
    ld s7, 184(a0)
    ld s8, 192(a0)
    ld s9, 200(a0)
    ld s10, 208(a0)
    ld s11, 216(a0)
    ld t3, 224(a0)
    ld t4, 232(a0)
    ld t5, 240(a0)
    ld t6, 248(a0)
    ld a0, 80(a0)
.globl user_return_sret
user_return_sret:
    sret

/* Fatal S-mode trap: switch to the emergency stack so that even a
 * kernel stack overflow (guard page fault) is reported. */
kernel_trap:
    csrrw t6, sscratch, t6      /* restore t6, sscratch = 0 */
    csrw sscratch, sp
    la sp, trap_emergency_stack_top
    addi sp, sp, -288
    sd zero, 0(sp)
    sd ra, 8(sp)
    sd gp, 24(sp)
    sd tp, 32(sp)
    sd t0, 40(sp)
    csrr t0, sscratch
    sd t0, 16(sp)
    csrw sscratch, zero
    sd t1, 48(sp)
    sd t2, 56(sp)
    sd s0, 64(sp)
    sd s1, 72(sp)
    sd a0, 80(sp)
    sd a1, 88(sp)
    sd a2, 96(sp)
    sd a3, 104(sp)
    sd a4, 112(sp)
    sd a5, 120(sp)
    sd a6, 128(sp)
    sd a7, 136(sp)
    sd s2, 144(sp)
    sd s3, 152(sp)
    sd s4, 160(sp)
    sd s5, 168(sp)
    sd s6, 176(sp)
    sd s7, 184(sp)
    sd s8, 192(sp)
    sd s9, 200(sp)
    sd s10, 208(sp)
    sd s11, 216(sp)
    sd t3, 224(sp)
    sd t4, 232(sp)
    sd t5, 240(sp)
    sd t6, 248(sp)
    csrr t0, sepc
    sd t0, 256(sp)
    csrr t0, sstatus
    sd t0, 264(sp)
    csrr t0, scause
    sd t0, 272(sp)
    csrr t0, stval
    sd t0, 280(sp)
    mv a0, sp
    call boot_trap_fatal
1:  wfi
    j 1b
