/* ProtoOS user programs A and B.
 *
 * Executed ONLY in U-mode at the virtual addresses of src/vm.h; the kernel
 * identity map never covers these pages. Each process image is one text
 * page with several entry points; the bootargs scenario chooses the entry.
 * Code is position independent (PC-relative or absolute user VAs only).
 * Syscall ABI (src/proc.h): a7=number, a0=argument/result.
 *
 * Data page layout (offsets from base+0x1000):
 *   0x00 counter   0x08 getpid #1   0x10 error flag   0x18 yields
 *   0x20 last syscall result        0x28 stage marker 0x30 getpid #2
 *   0x800..0xfff canary: word i = pattern ^ i, written once, never changed
 */
.option norvc
.equ SYS_EXIT, 93
.equ SYS_YIELD, 124
.equ SYS_GETPID, 172
.equ SYS_UNDEFINED, 999
.equ SYS_WRITE, 64
.equ SYS_SLEEP, 1001

.equ A_BASE, 0x400000
.equ B_BASE, 0x800000
.equ A_DATA, A_BASE + 0x1000
.equ B_DATA, B_BASE + 0x1000
.equ D_COUNTER, 0x00
.equ D_PID, 0x08
.equ D_ERROR, 0x10
.equ D_YIELDS, 0x18
.equ D_RET, 0x20
.equ D_STAGE, 0x28
.equ D_PID2, 0x30
.equ D_RESULTS, 0x40
.equ D_CANARY, 0x800
.equ CANARY_WORDS, 256
.equ A_PATTERN, 0xa1a1a1a100000000
.equ B_PATTERN, 0xb2b2b2b200000000

/* Fill the canary, then loop forever without any syscall. The loop keeps
 * s0+s1==0 and s2..s10 == pattern+2..10 and checks them every iteration, so
 * a lost or foreign register context is detected by the process itself. */
.macro WORKER data, pattern, loop, error
    li s11, \data
    li t0, 0
    li t1, \data + D_CANARY
    li t2, CANARY_WORDS
    li t3, \pattern
1:  xor t4, t3, t0
    sd t4, 0(t1)
    addi t1, t1, 8
    addi t0, t0, 1
    bltu t0, t2, 1b
    ld s0, D_COUNTER(s11)
    neg s1, s0
    li s2, \pattern + 2
    li s3, \pattern + 3
    li s4, \pattern + 4
    li s5, \pattern + 5
    li s6, \pattern + 6
    li s7, \pattern + 7
    li s8, \pattern + 8
    li s9, \pattern + 9
    li s10, \pattern + 10
.globl \loop
\loop:
    addi s0, s0, 1
    addi s1, s1, -1
    add t0, s0, s1
    bnez t0, \error
    li t0, \pattern + 2
    bne s2, t0, \error
    li t0, \pattern + 3
    bne s3, t0, \error
    li t0, \pattern + 4
    bne s4, t0, \error
    li t0, \pattern + 5
    bne s5, t0, \error
    li t0, \pattern + 6
    bne s6, t0, \error
    li t0, \pattern + 7
    bne s7, t0, \error
    li t0, \pattern + 8
    bne s8, t0, \error
    li t0, \pattern + 9
    bne s9, t0, \error
    li t0, \pattern + 10
    bne s10, t0, \error
    sd s0, D_COUNTER(s11)
    j \loop
.endm

/* Record a self-detected inconsistency and exit with code 0xbad. */
.macro ERROR_EXIT data
    li s11, \data
    li t0, 0xbad
    sd t0, D_ERROR(s11)
    li a0, 0xbad
    li a7, SYS_EXIT
    ecall
1:  j 1b
.endm

.macro YIELD
    li a7, SYS_YIELD
    ecall
.endm


/* String building for one-call (atomic) console lines.
 * bufstr: a0 = dst, a1 = NUL-terminated src -> a0 = dst end.
 * bufdec: a0 = dst, a1 = unsigned value     -> a0 = dst end. */
.macro BUF_LIB bufstr, bufdec
\bufstr:
1:  lbu t0, 0(a1)
    beqz t0, 2f
    sb t0, 0(a0)
    addi a0, a0, 1
    addi a1, a1, 1
    j 1b
2:  ret
\bufdec:
    li t2, 10
    mv t0, a1
    li t4, 1
1:  divu t0, t0, t2
    beqz t0, 2f
    addi t4, t4, 1
    j 1b
2:  add t5, a0, t4
    mv t1, t5
    mv t0, a1
3:  remu t3, t0, t2
    addi t3, t3, 48
    addi t1, t1, -1
    sb t3, 0(t1)
    divu t0, t0, t2
    bnez t0, 3b
    mv a0, t5
    ret
.endm

/* a0 = 1, a1 = start label, a2 = end - start, ecall write */
.macro WRITE_RANGE start, end
    li a0, 1
    la a1, \start
    la t0, \end
    sub a2, t0, a1
    li a7, SYS_WRITE
    ecall
.endm

/* Build "<prefix><pid><middle><n>\n" at s2 and write it with ONE call. */
.macro DEMO_LINE prefix, middle, nl, bufstr, bufdec
    mv a0, s2
    la a1, \prefix
    call \bufstr
    mv a1, s1
    call \bufdec
    la a1, \middle
    call \bufstr
    mv a1, s0
    call \bufdec
    la a1, \nl
    call \bufstr
    sub a2, a0, s2
    mv a1, s2
    li a0, 1
    li a7, SYS_WRITE
    ecall
.endm

/* ===================================================================== */
.section .user.a.text,"ax",@progbits
.balign 4

.globl a_worker
a_worker:
    WORKER A_DATA, A_PATTERN, a_worker_loop, a_error

.globl a_error
a_error:
    ERROR_EXIT A_DATA

/* T4 getpid: two calls must both return pid 1. */
.globl a_getpid
a_getpid:
    li s11, A_DATA
    li a0, -1
    li a7, SYS_GETPID
.globl a_getpid_ecall
a_getpid_ecall:
    ecall
.globl a_getpid_ret
a_getpid_ret:
    sd a0, D_PID(s11)
    li a0, -1
    li a7, SYS_GETPID
    ecall
    sd a0, D_PID2(s11)
    ld t0, D_PID(s11)
    bne a0, t0, a_error
    j a_worker

/* T4 yield: yield on every iteration; the result must be 0. */
.globl a_yield
a_yield:
    li s11, A_DATA
    li s0, 0
1:  addi s0, s0, 1
    sd s0, D_YIELDS(s11)
    li a0, 0x1234
    li a7, SYS_YIELD
.globl a_yield_ecall
a_yield_ecall:
    ecall
.globl a_yield_ret
a_yield_ret:
    sd a0, D_RET(s11)
    bnez a0, a_error
    j 1b

/* T4 exit: 1000 iterations, then exit(42). Code after it must never run. */
.globl a_exit
a_exit:
    li s11, A_DATA
    li s0, 0
    li t1, 1000
1:  addi s0, s0, 1
    sd s0, D_COUNTER(s11)
    bltu s0, t1, 1b
    li a0, 42
    li a7, SYS_EXIT
.globl a_exit_ecall
a_exit_ecall:
    ecall
.globl a_exit_after
a_exit_after:
    li t0, 0xdead
    sd t0, D_ERROR(s11)
    j a_exit_after

/* T5/N1: write into B's data page, which is unmapped in A's table.
 * Yield first so that B has written its canary. */
.globl a_cross_write
a_cross_write:
    li s11, A_DATA
    YIELD
    li t0, 1
    sd t0, D_STAGE(s11)
    li t0, B_DATA + D_CANARY
    li t1, 0x4141414141414141
.globl a_cross_write_store
a_cross_write_store:
    sd t1, 0(t0)
    li t0, 2
    sd t0, D_STAGE(s11)
1:  j 1b

/* T6: write into A's own text page (mapped R+X+U, no W). */
.globl a_fault
a_fault:
    li s11, A_DATA
    YIELD
    li t0, 1
    sd t0, D_STAGE(s11)
    la t0, a_fault_store
    li t1, 0x13
.globl a_fault_store
a_fault_store:
    sd t1, 0(t0)
    li t0, 2
    sd t0, D_STAGE(s11)
1:  j 1b

/* N2: read kernel-only data (identity mapped with U=0). a1 must keep the
 * sentinel because the load never completes. */
.globl a_kernel_read
a_kernel_read:
    li s11, A_DATA
    YIELD
    li t0, 1
    sd t0, D_STAGE(s11)
    ld t0, a_kernel_secret_ptr
    li a1, 0x5a5a5a5a5a5a5a5a
.globl a_kernel_read_load
a_kernel_read_load:
    ld a1, 0(t0)
    sd a1, D_RET(s11)
    li t0, 2
    sd t0, D_STAGE(s11)
1:  j 1b

/* N3: S-mode-only CSR write from U-mode must be an illegal instruction. */
.globl a_csr
a_csr:
    li s11, A_DATA
    YIELD
    li t0, 1
    sd t0, D_STAGE(s11)
    li t0, 0
.globl a_csr_write
a_csr_write:
    csrw satp, t0
    li t0, 2
    sd t0, D_STAGE(s11)
1:  j 1b

/* N4: unknown syscall number returns -38 (ENOSYS); A then keeps working. */
.globl a_bad_syscall
a_bad_syscall:
    li s11, A_DATA
    YIELD
    li a0, 7
    li a7, SYS_UNDEFINED
.globl a_bad_syscall_ecall
a_bad_syscall_ecall:
    ecall
.globl a_bad_syscall_ret
a_bad_syscall_ret:
    sd a0, D_RET(s11)
    li t0, -38
    bne a0, t0, a_error
    li t0, 1
    sd t0, D_STAGE(s11)
    j a_worker

/* N5: A exits immediately, B exits later; nothing stays runnable. */
.globl a_idle_exit
a_idle_exit:
    li s11, A_DATA
    li t0, 1
    sd t0, D_COUNTER(s11)
    li a0, 0
    li a7, SYS_EXIT
    ecall
1:  j 1b

/* N5 variant: A faults on an unmapped address, B exits later. */
.globl a_idle_fault
a_idle_fault:
    li t0, 0
.globl a_idle_fault_load
a_idle_fault_load:
    ld t1, 0(t0)
1:  j 1b


/* ---------------------------------------------------------------- v1.1 */
/* T8: two valid writes of a fixed message, each must return its length. */
.globl a_write
a_write:
    li s11, A_DATA
    li s3, 0
1:  WRITE_RANGE a_write_msg, a_write_msg_end
.globl a_write_ret
a_write_ret:
    sd a0, D_RET(s11)
    la t0, a_write_msg
    la t1, a_write_msg_end
    sub t0, t1, t0
    bne a0, t0, a_error
    addi s3, s3, 1
    li t0, 2
    bltu s3, t0, 1b
    li t0, 1
    sd t0, D_STAGE(s11)
    j a_worker

/* N6: rejected writes; results stored at D_RESULTS + 8*k, then a valid one. */
.globl a_write_bad
a_write_bad:
    li s11, A_DATA
    WRITE_RANGE a_n6_begin, a_n6_begin_end
    li a0, 2                        /* 0: wrong fd -> -9 */
    la a1, a_n6_ok
    li a2, 4
    li a7, SYS_WRITE
    ecall
    sd a0, D_RESULTS+0(s11)
    li a0, 1                        /* 1: kernel page (U=0) -> -14 */
    ld a1, a_kernel_secret_ptr
    li a2, 8
    li a7, SYS_WRITE
    ecall
    sd a0, D_RESULTS+8(s11)
    li a0, 1                        /* 2: B's data page, unmapped in A -> -14 */
    li a1, B_DATA
    li a2, 8
    li a7, SYS_WRITE
    ecall
    sd a0, D_RESULTS+16(s11)
    li a0, 1                        /* 3: crosses into A's unmapped guard page -> -14 */
    li a1, A_DATA + 0x1000 - 4
    li a2, 8
    li a7, SYS_WRITE
    ecall
    sd a0, D_RESULTS+24(s11)
    li a0, 1                        /* 4: len > 256 -> -22 */
    la a1, a_n6_ok
    li a2, 257
    li a7, SYS_WRITE
    ecall
    sd a0, D_RESULTS+32(s11)
    li a0, 1                        /* 5: NULL -> -14 */
    li a1, 0
    li a2, 1
    li a7, SYS_WRITE
    ecall
    sd a0, D_RESULTS+40(s11)
    li a0, 1                        /* 6: wraps past the top of the address space -> -14 */
    li a1, -16
    li a2, 32
    li a7, SYS_WRITE
    ecall
    sd a0, D_RESULTS+48(s11)
    WRITE_RANGE a_n6_ok, a_n6_ok_end   /* 7: valid -> length */
    sd a0, D_RESULTS+56(s11)
.globl a_write_bad_done
a_write_bad_done:
    li t0, 1
    sd t0, D_STAGE(s11)
    j a_worker

/* T9: sleep 200 ms in a loop; every sleep must return 0. */
.globl a_sleep
a_sleep:
    li s11, A_DATA
    li s0, 0
1:  addi s0, s0, 1
    sd s0, D_COUNTER(s11)
    li a0, 200
    li a7, SYS_SLEEP
.globl a_sleep_ecall
a_sleep_ecall:
    ecall
.globl a_sleep_ret
a_sleep_ret:
    sd a0, D_RET(s11)
    bnez a0, a_error
    j 1b

/* T9 (both asleep): 100 ms sleeps; B uses 150 ms. */
.globl a_sleep_all
a_sleep_all:
    li s11, A_DATA
    li s0, 0
1:  addi s0, s0, 1
    sd s0, D_COUNTER(s11)
    li a0, 100
    li a7, SYS_SLEEP
    ecall
.globl a_sleep_all_ret
a_sleep_all_ret:
    bnez a0, a_error
    j 1b

/* Demo: a numbered line every 400 ms, forever. */
.globl a_demo
a_demo:
    li a7, SYS_GETPID
    ecall
    mv s1, a0
    addi sp, sp, -128
    mv s2, sp
    li s0, 0
1:  addi s0, s0, 1
    DEMO_LINE a_demo_prefix, a_demo_middle, a_demo_nl, a_bufstr, a_bufdec
    li a0, 400
    li a7, SYS_SLEEP
    ecall
    j 1b

    BUF_LIB a_bufstr, a_bufdec

a_write_msg:    .ascii "ProtoOS T8: hello from user mode A\n"
a_write_msg_end:
a_n6_begin:     .ascii "N6-begin\n"
a_n6_begin_end:
a_n6_ok:        .ascii "N6 ok\n"
a_n6_ok_end:
a_demo_prefix:  .asciz "[A] pid "
a_demo_middle:  .asciz ": hello #"
a_demo_nl:      .asciz "\n"

.balign 8
a_kernel_secret_ptr:
    .dword kernel_secret

/* ===================================================================== */
.section .user.b.text,"ax",@progbits
.balign 4

.globl b_worker
b_worker:
    WORKER B_DATA, B_PATTERN, b_worker_loop, b_error

.globl b_error
b_error:
    ERROR_EXIT B_DATA

.globl b_getpid
b_getpid:
    li s11, B_DATA
    li a0, -1
    li a7, SYS_GETPID
.globl b_getpid_ecall
b_getpid_ecall:
    ecall
.globl b_getpid_ret
b_getpid_ret:
    sd a0, D_PID(s11)
    li a0, -1
    li a7, SYS_GETPID
    ecall
    sd a0, D_PID2(s11)
    ld t0, D_PID(s11)
    bne a0, t0, b_error
    j b_worker

.globl b_yield
b_yield:
    li s11, B_DATA
    li s0, 0
1:  addi s0, s0, 1
    sd s0, D_YIELDS(s11)
    li a0, 0x1234
    li a7, SYS_YIELD
.globl b_yield_ecall
b_yield_ecall:
    ecall
.globl b_yield_ret
b_yield_ret:
    sd a0, D_RET(s11)
    bnez a0, b_error
    j 1b

/* Long enough to span several 10 ms quanta, then exit(0). */
.globl b_idle_exit
b_idle_exit:
    li s11, B_DATA
    li s0, 0
    li t1, 3000000
1:  addi s0, s0, 1
    sd s0, D_COUNTER(s11)
    bltu s0, t1, 1b
    li a0, 0
    li a7, SYS_EXIT
.globl b_idle_exit_ecall
b_idle_exit_ecall:
    ecall
1:  j 1b


/* ---------------------------------------------------------------- v1.1 */
.globl b_sleep_all
b_sleep_all:
    li s11, B_DATA
    li s0, 0
1:  addi s0, s0, 1
    sd s0, D_COUNTER(s11)
    li a0, 150
    li a7, SYS_SLEEP
    ecall
.globl b_sleep_all_ret
b_sleep_all_ret:
    bnez a0, b_error
    j 1b

/* Demo: four lines every 700 ms, then announce and attempt a write into
 * A's memory (unmapped in B's root) -> store page fault, B is FAULTED. */
.globl b_demo
b_demo:
    li a7, SYS_GETPID
    ecall
    mv s1, a0
    addi sp, sp, -128
    mv s2, sp
    li s0, 0
1:  addi s0, s0, 1
    DEMO_LINE b_demo_prefix, b_demo_middle, b_demo_nl, b_bufstr, b_bufdec
    li a0, 700
    li a7, SYS_SLEEP
    ecall
    li t0, 4
    bltu s0, t0, 1b
    WRITE_RANGE b_demo_attack, b_demo_attack_end
    li a0, 300
    li a7, SYS_SLEEP
    ecall
    li t0, A_DATA
    li t1, 0x4242424242424242
.globl b_demo_attack_store
b_demo_attack_store:
    sd t1, 0(t0)
2:  j 2b

    BUF_LIB b_bufstr, b_bufdec

b_demo_prefix:  .asciz "[B] pid "
b_demo_middle:  .asciz ": hello #"
b_demo_nl:      .asciz "\n"
b_demo_attack:  .ascii "[B] pid 2: now I will try to overwrite A's memory...\n"
b_demo_attack_end:
.balign 4

/* ===================================================================== */
.section .user.bss,"aw",@nobits
.balign 4096
.globl user_a_data
user_a_data:
    .skip 4096
.globl user_a_stack
user_a_stack:
    .skip 4096
.globl user_b_data
user_b_data:
    .skip 4096
.globl user_b_stack
user_b_stack:
    .skip 4096
