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
.equ SYS_READ, 63
.equ SYS_UPTIME, 1002
.equ SYS_PROCINFO, 1003
.equ SYS_SPAWN, 1004
.equ SYS_KILL, 1005
.equ SYS_MEMINFO, 1006
.equ SYS_SLOTINFO, 1007
.equ PROG_WORKER, 1     /* src/proc.h enum program_id */
.equ PROG_HELLO, 2
.equ PROG_COUNTER, 3
.equ PROG_CRASH, 4
.equ PROG_MEMCHECK, 5
.equ SYS_RUN, 1008      /* v1.4 */
.equ SYS_WAIT, 1009
.equ SYS_SBRK, 1010
.equ SYS_PROGS, 1011
.equ PROG_FILE, 16      /* program id of archive file n: PROG_FILE + n */

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
.equ D_IN, 0x100        /* v1.2 tests: received bytes */
.equ D_LINE, 0x100      /* v1.2 shell: current line (LINE_MAX + NUL) */
.equ D_CHUNK, 0x180     /* v1.2 shell: one read() chunk */
.equ D_OUT, 0x200       /* v1.2 shell: output line being built */
.equ D_INFO, 0x300      /* v1.2 shell: procinfo() result */
.equ D_NAME, 0x340      /* v1.4 shell: progs() name (16 bytes) */
.equ D_R14, 0x400       /* v1.4 tests: results, one word each */
.equ LINE_MAX, 63
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

/* ---------------------------------------------------------------- v1.2 */
/* T10: read forever in 16-byte chunks; bytes accumulate at D_IN. */
.globl a_read
a_read:
    li s11, A_DATA
    li s0, 0
1:  li a0, 0
    li a1, A_DATA + D_IN
    add a1, a1, s0
    li a2, 16
    li a7, SYS_READ
.globl a_read_ecall
a_read_ecall:
    ecall
.globl a_read_ret
a_read_ret:
    sd a0, D_RET(s11)
    blez a0, a_error
    add s0, s0, a0
    sd s0, D_COUNTER(s11)
    li t0, 0x600
    bltu s0, t0, 1b
2:  j 2b

/* T11: uptime, sleep 100 ms, uptime again; the value must grow. */
.globl a_uptime
a_uptime:
    li s11, A_DATA
    li a7, SYS_UPTIME
    ecall
.globl a_uptime_ret1
a_uptime_ret1:
    sd a0, D_RESULTS+0(s11)
    li a0, 100
    li a7, SYS_SLEEP
    ecall
    li a7, SYS_UPTIME
    ecall
.globl a_uptime_ret2
a_uptime_ret2:
    sd a0, D_RESULTS+8(s11)
    ld t0, D_RESULTS+0(s11)
    bgeu t0, a0, a_error
    li t0, 1
    sd t0, D_STAGE(s11)
    j a_worker

/* N7: rejected reads; results stored at D_RESULTS + 8*k. */
.globl a_read_bad
a_read_bad:
    li s11, A_DATA
    li a0, 1                        /* 0: wrong fd -> -9 */
    li a1, A_DATA + D_IN
    li a2, 4
    li a7, SYS_READ
    ecall
    sd a0, D_RESULTS+0(s11)
    li a0, 0                        /* 1: kernel page (U=0) -> -14 */
    ld a1, a_kernel_secret_ptr
    li a2, 8
    li a7, SYS_READ
    ecall
    sd a0, D_RESULTS+8(s11)
    li a0, 0                        /* 2: B's data page, unmapped in A -> -14 */
    li a1, B_DATA
    li a2, 8
    li a7, SYS_READ
    ecall
    sd a0, D_RESULTS+16(s11)
    li a0, 0                        /* 3: A's own text page (R+X, no W) -> -14 */
    la a1, a_read_bad
    li a2, 4
    li a7, SYS_READ
    ecall
    sd a0, D_RESULTS+24(s11)
    li a0, 0                        /* 4: crosses into A's guard page -> -14 */
    li a1, A_DATA + 0x1000 - 4
    li a2, 8
    li a7, SYS_READ
    ecall
    sd a0, D_RESULTS+32(s11)
    li a0, 0                        /* 5: len > 256 -> -22 */
    li a1, A_DATA + D_IN
    li a2, 257
    li a7, SYS_READ
    ecall
    sd a0, D_RESULTS+40(s11)
    li a0, 0                        /* 6: NULL -> -14 */
    li a1, 0
    li a2, 1
    li a7, SYS_READ
    ecall
    sd a0, D_RESULTS+48(s11)
    li a0, 0                        /* 7: wraps past the top of the address space -> -14 */
    li a1, -16
    li a2, 32
    li a7, SYS_READ
    ecall
    sd a0, D_RESULTS+56(s11)
    li a0, 0                        /* 8: len 0 -> 0, does not block */
    li a1, A_DATA + D_IN
    li a2, 0
    li a7, SYS_READ
    ecall
    sd a0, D_RESULTS+64(s11)
    li t0, 1
    sd t0, D_STAGE(s11)
    li a0, 0                        /* 9: valid read, blocks until input */
    li a1, A_DATA + D_IN
    li a2, 4
    li a7, SYS_READ
.globl a_read_bad_block
a_read_bad_block:
    ecall
    sd a0, D_RESULTS+72(s11)
.globl a_read_bad_done
a_read_bad_done:
    li t0, 2
    sd t0, D_STAGE(s11)
    j a_worker

/* N8: sleep while the harness floods the UART, then drain with one read,
 * then one more (blocking) read for a byte sent after the flood. */
.globl a_flood
a_flood:
    li s11, A_DATA
    li a0, 500
    li a7, SYS_SLEEP
.globl a_flood_sleep_ecall
a_flood_sleep_ecall:
    ecall
    li a0, 0
    li a1, A_DATA + D_IN
    li a2, 256
    li a7, SYS_READ
    ecall
    sd a0, D_RESULTS+0(s11)
    blez a0, a_error
    li a1, A_DATA + D_IN
    add a1, a1, a0
    li a0, 0
    li a2, 16
    li a7, SYS_READ
.globl a_flood_read2_ecall
a_flood_read2_ecall:
    ecall
    sd a0, D_RESULTS+8(s11)
.globl a_flood_done
a_flood_done:
    li t0, 1
    sd t0, D_STAGE(s11)
    j a_worker

/* ---------------------------------------------------------------- v1.3 */
.macro SYSCALL1 nr, arg
    li a0, \arg
    li a7, \nr
    ecall
.endm
.macro MEMINFO_TO off
    li a7, SYS_MEMINFO
    ecall
    sd a0, D_RESULTS+\off(s11)
.endm
.macro SPAWN_TO prog, off
    SYSCALL1 SYS_SPAWN, \prog
    sd a0, D_RESULTS+\off(s11)
    blez a0, a_error
.endm
.macro KILL_TO reg, off
    mv a0, \reg
    li a7, SYS_KILL
    ecall
    sd a0, D_RESULTS+\off(s11)
.endm

/* T13: memcheck twice in the same slot; the second instance gets the pages
 * the first one dirtied and must still find them zeroed. */
.globl a_mem_test
a_mem_test:
    li s11, A_DATA
    MEMINFO_TO 0
    SPAWN_TO PROG_MEMCHECK, 8
    mv s0, a0
    SYSCALL1 SYS_SLEEP, 100
.globl a_mem_after1
a_mem_after1:
    KILL_TO s0, 16
    bnez a0, a_error
    SPAWN_TO PROG_MEMCHECK, 24
    mv s0, a0
    SYSCALL1 SYS_SLEEP, 100
.globl a_mem_after2
a_mem_after2:
    KILL_TO s0, 32
    bnez a0, a_error
    MEMINFO_TO 40
.globl a_mem_done
a_mem_done:
    li t0, 1
    sd t0, D_STAGE(s11)
    j a_worker

/* T14: one run-time worker; A then only sleeps. */
.globl a_spawn_test
a_spawn_test:
    li s11, A_DATA
    MEMINFO_TO 0
    SPAWN_TO PROG_WORKER, 8
    MEMINFO_TO 16
.globl a_spawn_done
a_spawn_done:
    li t0, 1
    sd t0, D_STAGE(s11)
1:  SYSCALL1 SYS_SLEEP, 1000
    j 1b

/* T15: kill a worker, wait, respawn into the same slot, kill again, then a
 * hello that exits by itself; free pages must return to the baseline. */
.globl a_kill_test
a_kill_test:
    li s11, A_DATA
    MEMINFO_TO 0
    SPAWN_TO PROG_WORKER, 8
    mv s0, a0
    SYSCALL1 SYS_SLEEP, 100
    KILL_TO s0, 16
    MEMINFO_TO 24
.globl a_kill_killed
a_kill_killed:
    SYSCALL1 SYS_SLEEP, 200
    SPAWN_TO PROG_WORKER, 32
    mv s1, a0
.globl a_kill_respawned
a_kill_respawned:
    SYSCALL1 SYS_SLEEP, 100
    KILL_TO s1, 40
    SPAWN_TO PROG_HELLO, 48
    SYSCALL1 SYS_SLEEP, 300
    MEMINFO_TO 56
.globl a_kill_done
a_kill_done:
    li t0, 1
    sd t0, D_STAGE(s11)
    j a_worker

/* T16: three workers and a crash; only the crash may fault. The crash sits
 * between workers (slot D), so the Round Robin order after its fault is
 * observable (v1.3.1). */
.globl a_multi_test
a_multi_test:
    li s11, A_DATA
    MEMINFO_TO 0
    SPAWN_TO PROG_WORKER, 8
    SPAWN_TO PROG_CRASH, 16
    SPAWN_TO PROG_WORKER, 24
    SPAWN_TO PROG_WORKER, 32
    SYSCALL1 SYS_SLEEP, 500
    MEMINFO_TO 40
.globl a_multi_done
a_multi_done:
    li t0, 1
    sd t0, D_STAGE(s11)
1:  SYSCALL1 SYS_SLEEP, 1000
    j 1b

/* N9: rejected spawn/kill, the slot limit, and 200 spawn/kill cycles. */
.globl a_spawn_bad
a_spawn_bad:
    li s11, A_DATA
    MEMINFO_TO 0
    SYSCALL1 SYS_SPAWN, 0           /* program 0 -> -22 */
    sd a0, D_RESULTS+8(s11)
    SYSCALL1 SYS_SPAWN, 99          /* unknown program -> -22 */
    sd a0, D_RESULTS+16(s11)
    SYSCALL1 SYS_KILL, 999          /* no such pid -> -3 */
    sd a0, D_RESULTS+24(s11)
    SYSCALL1 SYS_KILL, 1            /* itself -> -1 */
    sd a0, D_RESULTS+32(s11)
    SYSCALL1 SYS_KILL, 2            /* boot process B -> -1 */
    sd a0, D_RESULTS+40(s11)
    li s0, 0                        /* fill every free slot */
1:  SYSCALL1 SYS_SPAWN, PROG_WORKER
    bltz a0, 2f
    slli t0, s0, 3
    add t0, t0, s11
    sd a0, D_IN(t0)
    addi s0, s0, 1
    li t0, 16
    bltu s0, t0, 1b
2:  sd a0, D_RESULTS+48(s11)        /* first refusal -> -11 */
    sd s0, D_RESULTS+56(s11)        /* successes -> 6 */
    li s1, 0
3:  bgeu s1, s0, 4f
    slli t0, s1, 3
    add t0, t0, s11
    ld a0, D_IN(t0)
    li a7, SYS_KILL
    ecall
    bnez a0, a_error
    addi s1, s1, 1
    j 3b
4:  ld a0, D_IN(s11)                /* already killed -> -3 */
    li a7, SYS_KILL
    ecall
    sd a0, D_RESULTS+64(s11)
    li s1, 0                        /* 200 spawn/kill cycles */
    li s2, 0
5:  SYSCALL1 SYS_SPAWN, PROG_WORKER
    blez a0, 6f
    li a7, SYS_KILL
    ecall
    beqz a0, 7f
6:  addi s2, s2, 1
7:  addi s1, s1, 1
    li t0, 200
    bltu s1, t0, 5b
    sd s1, D_RESULTS+72(s11)
    sd s2, D_RESULTS+80(s11)        /* failures -> 0 */
    MEMINFO_TO 88
.globl a_spawn_bad_done
a_spawn_bad_done:
    li t0, 1
    sd t0, D_STAGE(s11)
    j a_worker

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
/* v1.3 shared program pages: mapped R X U at PROG_VA in every process.
 * Position independent; a program finds its own data page in a0 and its
 * pid in a1 at entry (set by the kernel), never at a fixed address. */
.section .user.prog.text,"ax",@progbits
.balign 4

/* Build "<s1><dec a1><s2>" helpers on top of BUF_LIB. */
    BUF_LIB p_bufstr, p_bufdec

/* write(1, s11 + D_OUT, a0 - (s11 + D_OUT)) */
.macro OUT_FLUSH
    addi t0, s11, D_OUT
    sub a2, a0, t0
    mv a1, t0
    li a0, 1
    li a7, SYS_WRITE
    ecall
.endm

/* a0 = dst, a1 = signed value -> a0 = dst end ("-" for negatives) */
p_bufsigned:
    bgez a1, 1f
    li t0, 45
    sb t0, 0(a0)
    addi a0, a0, 1
    neg a1, a1
1:  j p_bufdec

/* worker: count forever in its data page, no system calls */
.globl p_worker
p_worker:
    mv s11, a0
    ld s0, 0(s11)
.globl p_worker_loop
p_worker_loop:
    addi s0, s0, 1
    sd s0, 0(s11)
    j p_worker_loop

/* hello: one line, then exit(0) */
.globl p_hello
p_hello:
    mv s11, a0
    mv s10, a1
    addi a0, s11, D_OUT
    la a1, p_hello_msg
    call p_bufstr
    mv a1, s10
    call p_bufdec
    la a1, p_nl
    call p_bufstr
    OUT_FLUSH
    li a0, 0
    li a7, SYS_EXIT
    ecall
1:  j 1b

/* counter: three lines one second apart, then exit(0) */
.globl p_counter
p_counter:
    mv s11, a0
    mv s10, a1
    li s9, 1
1:  addi a0, s11, D_OUT
    la a1, p_pid_prefix
    call p_bufstr
    mv a1, s10
    call p_bufdec
    la a1, p_count_mid
    call p_bufstr
    mv a1, s9
    call p_bufdec
    la a1, p_nl
    call p_bufstr
    OUT_FLUSH
    li a0, 1000
    li a7, SYS_SLEEP
    ecall
    addi s9, s9, 1
    li t0, 4
    bltu s9, t0, 1b
    li a0, 0
    li a7, SYS_EXIT
    ecall
2:  j 2b

/* crash: announce, then store into B's data page (unmapped here) */
.globl p_crash
p_crash:
    mv s11, a0
    mv s10, a1
    addi a0, s11, D_OUT
    la a1, p_crash_msg1
    call p_bufstr
    mv a1, s10
    call p_bufdec
    la a1, p_crash_msg2
    call p_bufstr
    OUT_FLUSH
    li t0, B_DATA
    li t1, 0x4343434343434343
.globl p_crash_store
p_crash_store:
    sd t1, 0(t0)
1:  j 1b

/* memcheck (test program): data and stack pages must arrive all zero;
 * result at data+0x10 (1 clean, 0xbad dirty), then dirty both pages with
 * 0xa5 so a later owner would see them if the kernel did not clear them. */
.globl p_memcheck
p_memcheck:
    mv s11, a0
    li s0, 1
    mv t0, s11
    li t2, 512
1:  ld t1, 0(t0)
    bnez t1, 2f
    addi t0, t0, 8
    addi t2, t2, -1
    bnez t2, 1b
    j 3f
2:  li s0, 0xbad
3:  li t0, 0x2000                    /* stack page = data page + 0x2000 */
    add t0, t0, s11
    li t2, 512
4:  ld t1, 0(t0)
    bnez t1, 5f
    addi t0, t0, 8
    addi t2, t2, -1
    bnez t2, 4b
    j 6f
5:  li s0, 0xbad
6:  li t1, 0xa5a5a5a5a5a5a5a5
    addi t0, s11, 0x40               /* keep 0x00..0x3f for results */
    li t2, 0x1000
    add t2, t2, s11
7:  sd t1, 0(t0)
    addi t0, t0, 8
    bltu t0, t2, 7b
    li t0, 0x2000
    add t0, t0, s11
    li t2, 0x3000
    add t2, t2, s11
8:  sd t1, 0(t0)
    addi t0, t0, 8
    bltu t0, t2, 8b
    sd s0, 0x10(s11)
    li t0, 1
    sd t0, 0x18(s11)
.globl p_memcheck_done
p_memcheck_done:
9:  li a0, 1000
    li a7, SYS_SLEEP
    ecall
    j 9b

/* ---------------------------------------------------------------- shell */
/* proto> (protoos.mode=shell, process A). Registers kept across commands:
 *   s11 data page, s0 line length, s1 bytes in the current read chunk,
 *   s2 next index in it, s4 = 1 if the previous byte was CR. */
.macro SH_CMD name, handler
    addi a0, s11, D_LINE
    la a1, \name
    call p_streq
    bnez a0, \handler
.endm

.globl p_shell
p_shell:
    mv s11, a0
    li s1, 0
    li s2, 0
    li s4, 0
    WRITE_RANGE p_sh_banner, p_sh_banner_end
.globl p_sh_prompt
p_sh_prompt:
    WRITE_RANGE p_sh_prompt_str, p_sh_prompt_end
    li s0, 0
p_sh_char:
    bltu s2, s1, 1f
    li a0, 0                        /* chunk used up: wait for input */
    addi a1, s11, D_CHUNK
    li a2, 16
    li a7, SYS_READ
.globl p_shell_read_ecall
p_shell_read_ecall:
    ecall
    blez a0, p_sh_fatal
    mv s1, a0
    li s2, 0
1:  addi t0, s11, D_CHUNK
    add t0, t0, s2
    lbu s3, 0(t0)
    addi s2, s2, 1
    mv t1, s4
    li s4, 0
    li t0, 13
    beq s3, t0, p_sh_cr
    li t0, 10
    bne s3, t0, 2f
    bnez t1, p_sh_char              /* LF right after CR */
    j p_sh_enter
p_sh_cr:
    li s4, 1
    j p_sh_enter
2:  li t0, 127
    beq s3, t0, p_sh_bs
    li t0, 8
    beq s3, t0, p_sh_bs
    li t0, 32
    bltu s3, t0, p_sh_char          /* other control bytes: ignored */
    li t0, 126
    bgtu s3, t0, p_sh_char          /* non-ASCII: ignored */
    li t0, LINE_MAX
    bgeu s0, t0, p_sh_char          /* line full: ignored */
    addi a1, s11, D_LINE
    add a1, a1, s0
    sb s3, 0(a1)
    addi s0, s0, 1
    li a0, 1                        /* echo it */
    li a2, 1
    li a7, SYS_WRITE
    ecall
    j p_sh_char
p_sh_bs:
    beqz s0, p_sh_char
    addi s0, s0, -1
    WRITE_RANGE p_sh_bs_seq, p_sh_bs_seq_end
    j p_sh_char
p_sh_fatal:
    li a0, 0xbad
    li a7, SYS_EXIT
    ecall
3:  j 3b

p_sh_enter:
    WRITE_RANGE p_sh_nl, p_sh_nl_end
    addi t0, s11, D_LINE
    add t0, t0, s0
    sb zero, 0(t0)
    beqz s0, p_sh_prompt
    SH_CMD p_sh_c_help, p_sh_help
    SH_CMD p_sh_c_ps, p_sh_ps
    SH_CMD p_sh_c_uptime, p_sh_uptime
    SH_CMD p_sh_c_attack, p_sh_attack
    SH_CMD p_sh_c_mem, p_sh_mem
    SH_CMD p_sh_c_ls, p_sh_ls
    addi a0, s11, D_LINE
    la a1, p_sh_c_run
    call p_strprefix
    bnez a0, p_sh_run
    addi a0, s11, D_LINE
    la a1, p_sh_c_kill
    call p_strprefix
    bnez a0, p_sh_kill
    addi a0, s11, D_LINE
    la a1, p_sh_c_echo
    call p_strprefix
    beqz a0, p_sh_unknown
    lbu t0, 0(a0)
    beqz t0, p_sh_echo              /* "echo" alone */
    li t1, 32
    bne t0, t1, p_sh_unknown        /* "echoX..." */
    addi a0, a0, 1
p_sh_echo:
    mv a1, a0
    addi a0, s11, D_OUT
    call p_bufstr
    la a1, p_nl
    call p_bufstr
    j p_sh_out
p_sh_unknown:
    WRITE_RANGE p_sh_unknown_msg, p_sh_unknown_msg_end
    j p_sh_prompt
p_sh_help:
    WRITE_RANGE p_sh_help_msg, p_sh_help_msg_end
    j p_sh_prompt

/* "uptime: <s>.<cc> s" from the microsecond counter */
p_sh_uptime:
    li a7, SYS_UPTIME
    ecall
    mv s5, a0
    addi a0, s11, D_OUT
    la a1, p_sh_up_prefix
    call p_bufstr
    li t0, 1000000
    divu a1, s5, t0
    call p_bufdec
    la a1, p_sh_dot
    call p_bufstr
    li t0, 10000
    divu t1, s5, t0
    li t0, 100
    remu s6, t1, t0
    li t0, 10
    bgeu s6, t0, 1f
    la a1, p_sh_zero
    call p_bufstr
1:  mv a1, s6
    call p_bufdec
    la a1, p_sh_secs
    call p_bufstr
    j p_sh_out

/* "free pages: <free> of <total>" */
p_sh_mem:
    li a7, SYS_MEMINFO
    ecall
    mv s5, a0
    mv s6, a1
    addi a0, s11, D_OUT
    la a1, p_sh_mem_prefix
    call p_bufstr
    mv a1, s5
    call p_bufdec
    la a1, p_sh_of
    call p_bufstr
    mv a1, s6
    call p_bufdec
    la a1, p_nl
    call p_bufstr
    j p_sh_out

/* ps: one line per used slot, "pid <n> <slot letter> <STATE> preempted
 * <p> syscalls <s>", plus the program name for run-time processes */
p_sh_ps:
    li s5, 0
1:  mv a0, s5
    addi a1, s11, D_INFO
    li a7, SYS_SLOTINFO
    ecall
    bnez a0, p_sh_fatal
    ld s6, D_INFO+8(s11)            /* state */
    beqz s6, 9f                     /* unused slot */
    addi a0, s11, D_OUT
    la a1, p_sh_pid
    call p_bufstr
    ld a1, D_INFO+0(s11)
    call p_bufdec
    li t0, 32
    sb t0, 0(a0)
    addi t1, s5, 65                 /* slot letter 'A' + slot */
    sb t1, 1(a0)
    sb t0, 2(a0)
    addi a0, a0, 3
    li t0, 6
    bleu s6, t0, 2f
    li s6, 0
2:  la a1, p_sh_states
    mv a2, s6
    call p_nth
    call p_bufstr
    la a1, p_sh_preempted
    call p_bufstr
    ld a1, D_INFO+24(s11)
    call p_bufdec
    la a1, p_sh_syscalls
    call p_bufstr
    ld a1, D_INFO+32(s11)
    call p_bufdec
    li t0, 2
    bltu s5, t0, 3f                 /* boot processes: v1.2 format */
    ld a2, D_INFO+16(s11)           /* program id */
    li t0, 32
    sb t0, 0(a0)
    addi a0, a0, 1
    li t0, PROG_FILE
    bgeu a2, t0, 5f                 /* v1.4: a program from the archive */
    li t0, 5
    bleu a2, t0, 4f
    li a2, 0
4:  la a1, p_sh_progs
    call p_nth
    call p_bufstr
    j 3f
5:  mv s9, a0
    addi a0, a2, -PROG_FILE
    addi a1, s11, D_NAME
    li a7, SYS_PROGS
    ecall
    mv a0, s9
    addi a1, s11, D_NAME
    call p_bufstr
3:  la a1, p_nl
    call p_bufstr
    OUT_FLUSH
9:  addi s5, s5, 1
    li t0, 8
    bltu s5, t0, 1b
    j p_sh_prompt

/* run <program>: a built-in program by name (started in the background,
 * as in v1.3), else (v1.4) a program from the archive */
p_sh_run:
    mv s7, a0                       /* program name */
    la s8, p_sh_progs
    li s6, 0
1:  addi s6, s6, 1
    li t0, 5
    bgtu s6, t0, p_sh_runfile
2:  lbu t0, 0(s8)                   /* next name in the list */
    addi s8, s8, 1
    bnez t0, 2b
    mv a0, s7
    mv a1, s8
    call p_streq
    beqz a0, 1b
    mv a0, s6
    li a7, SYS_SPAWN
    ecall
    mv s5, a0
    blez s5, p_sh_run_failed_out
p_sh_run_started:                   /* s5 = pid */
    addi a0, s11, D_OUT
    la a1, p_sh_started
    call p_bufstr
    mv a1, s5
    call p_bufdec
    la a1, p_nl
    call p_bufstr
    j p_sh_out
p_sh_run_failed_out:                /* s5 = error */
    addi a0, s11, D_OUT
    la a1, p_sh_run_failed
    call p_bufstr
    mv a1, s5
    call p_bufsigned
    la a1, p_nl
    call p_bufstr
    j p_sh_out

/* v1.4: run <program> [&] from the archive. Without "&" the shell waits
 * with the program in the console foreground (Ctrl-C ends it) and then
 * reports a non-zero status; with "&" it only reports the pid. */
p_sh_runfile:
    mv a0, s7
    call p_strlen
    mv s6, a0                       /* name length */
    li s8, 0                        /* 1 = background */
    li t0, 2
    bltu s6, t0, 1f
    add t1, s7, s6
    lbu t2, -1(t1)
    li t0, 38                       /* '&' */
    bne t2, t0, 1f
    lbu t2, -2(t1)
    li t0, 32
    bne t2, t0, 1f
    li s8, 1
    addi s6, s6, -2
    add t1, s7, s6
    sb zero, 0(t1)                  /* the name ends before " &" */
1:  beqz s6, 5f
    mv a0, s7
    mv a1, s6
    li a7, SYS_RUN
    ecall
    mv s5, a0
    blez s5, 4f
    bnez s8, p_sh_run_started
    mv a0, s5
    li a1, 1                        /* WAIT_FOREGROUND */
    li a7, SYS_WAIT
.globl p_sh_wait_ecall
p_sh_wait_ecall:
    ecall
    mv s6, a0
    beqz s6, p_sh_prompt
    li t0, 130
    bne s6, t0, 2f
    WRITE_RANGE p_sh_intr, p_sh_intr_end
    j p_sh_prompt
2:  addi a0, s11, D_OUT
    la a1, p_sh_pid
    call p_bufstr
    mv a1, s5
    call p_bufdec
    la a1, p_sh_ended
    call p_bufstr
    mv a1, s6
    call p_bufsigned
    la a1, p_nl
    call p_bufstr
    j p_sh_out
4:  li t0, -2
    bne s5, t0, p_sh_run_failed_out
    addi a0, s11, D_OUT
    la a1, p_sh_noprog
    call p_bufstr
    mv a1, s7
    call p_bufstr
    la a1, p_sh_tryls
    call p_bufstr
    j p_sh_out
5:  WRITE_RANGE p_sh_run_usage, p_sh_run_usage_end
    j p_sh_prompt

/* v1.4 ls: "<name>  <size> bytes" per archive program */
p_sh_ls:
    li s5, 0
1:  mv a0, s5
    addi a1, s11, D_NAME
    li a7, SYS_PROGS
    ecall
    bltz a0, 9f
    mv s6, a0
    addi a0, s11, D_OUT
    addi a1, s11, D_NAME
    call p_bufstr
    la a1, p_sh_ls_gap
    call p_bufstr
    mv a1, s6
    call p_bufdec
    la a1, p_sh_bytes
    call p_bufstr
    OUT_FLUSH
    addi s5, s5, 1
    j 1b
9:  bnez s5, p_sh_prompt
    WRITE_RANGE p_sh_ls_none, p_sh_ls_none_end
    j p_sh_prompt

/* kill <pid>: end it and report how many pages came back */
p_sh_kill:
    call p_atoi
    bltz a0, 3f
    mv s7, a0
    li a7, SYS_MEMINFO
    ecall
    mv s6, a0
    mv a0, s7
    li a7, SYS_KILL
    ecall
    mv s5, a0
    bnez s5, 4f
    li a7, SYS_MEMINFO
    ecall
    sub s6, a0, s6
    addi a0, s11, D_OUT
    la a1, p_sh_killed
    call p_bufstr
    mv a1, s7
    call p_bufdec
    la a1, p_sh_killed2
    call p_bufstr
    mv a1, s6
    call p_bufdec
    la a1, p_sh_returned
    call p_bufstr
    j p_sh_out
4:  addi a0, s11, D_OUT
    la a1, p_sh_kill_failed
    call p_bufstr
    mv a1, s5
    call p_bufsigned
    la a1, p_nl
    call p_bufstr
    j p_sh_out
3:  WRITE_RANGE p_sh_kill_usage, p_sh_kill_usage_end
    j p_sh_prompt

/* Ask the kernel to print B's memory: the PT walk of A's root refuses. */
p_sh_attack:
    WRITE_RANGE p_sh_att_msg, p_sh_att_msg_end
    li a0, 1
    li a1, B_DATA
    li a2, 8
    li a7, SYS_WRITE
    ecall
    mv a1, a0
    addi a0, s11, D_OUT
    mv s5, a1
    la a1, p_sh_att_ans
    call p_bufstr
    mv a1, s5
    call p_bufsigned
    la a1, p_sh_att_tail
    call p_bufstr
    j p_sh_out

p_sh_out:
    OUT_FLUSH
    j p_sh_prompt

/* a0, a1: NUL-terminated strings -> a0 = 1 if equal, else 0 */
p_streq:
1:  lbu t0, 0(a0)
    lbu t1, 0(a1)
    bne t0, t1, 2f
    beqz t0, 3f
    addi a0, a0, 1
    addi a1, a1, 1
    j 1b
2:  li a0, 0
    ret
3:  li a0, 1
    ret

/* a0 = string, a1 = prefix -> a0 = first byte after the prefix, or 0 */
p_strprefix:
1:  lbu t1, 0(a1)
    beqz t1, 2f
    lbu t0, 0(a0)
    bne t0, t1, 3f
    addi a0, a0, 1
    addi a1, a1, 1
    j 1b
2:  ret
3:  li a0, 0
    ret

/* a1 = list of NUL-terminated strings, a2 = index -> a1 = that string */
p_nth:
1:  beqz a2, 3f
2:  lbu t0, 0(a1)
    addi a1, a1, 1
    bnez t0, 2b
    addi a2, a2, -1
    j 1b
3:  ret

/* a0 = decimal string (1..9 digits, nothing else) -> a0 = value, or -1 */
p_atoi:
    li t2, 0
    li t3, 0
1:  lbu t0, 0(a0)
    beqz t0, 2f
    addi t0, t0, -48
    li t1, 10
    bgeu t0, t1, 3f
    mul t2, t2, t1
    add t2, t2, t0
    addi a0, a0, 1
    addi t3, t3, 1
    li t1, 9
    bgtu t3, t1, 3f
    j 1b
2:  beqz t3, 3f
    mv a0, t2
    ret
3:  li a0, -1
    ret

/* a0 = NUL-terminated string -> a0 = its length */
p_strlen:
    mv t0, a0
1:  lbu t1, 0(t0)
    beqz t1, 2f
    addi t0, t0, 1
    j 1b
2:  sub a0, t0, a0
    ret

/* a0 = NUL-terminated name -> a0 = run(name, its length) */
p_run:
    mv t0, a0
1:  lbu t1, 0(t0)
    beqz t1, 2f
    addi t0, t0, 1
    j 1b
2:  sub a1, t0, a0
    li a7, SYS_RUN
    ecall
    ret

/* ------------------------------------------------- v1.4 test drivers (A)
 * Run as process A (entry in these shared pages, a0 = A's data page).
 * Results: word k at data + D_R14 + 8k; D_STAGE = 1 when done. */
.macro R14 k
    sd a0, D_R14+8*\k(s11)
.endm
.macro RUN_TO name, k
    la a0, \name
    call p_run
    R14 \k
.endm
.macro WAIT_TO reg, flags, k
    mv a0, \reg
    li a1, \flags
    li a7, SYS_WAIT
    ecall
    R14 \k
.endm
.macro MEMINFO14 k
    li a7, SYS_MEMINFO
    ecall
    R14 \k
.endm
.macro SLEEP14 ms
    li a0, \ms
    li a7, SYS_SLEEP
    ecall
.endm
.macro SYSCALL2_TO nr, x, y, k
    li a0, \x
    li a1, \y
    li a7, \nr
    ecall
    R14 \k
.endm
.macro DONE14 label
.globl \label
\label:
    li t0, 1
    sd t0, D_STAGE(s11)
1:  SLEEP14 1000
    j 1b
.endm

/* T18: load t_elf (self-checks, exit 0) and t_exit42 (exit 42) */
.globl p_t_elf
p_t_elf:
    mv s11, a0
    MEMINFO14 0
    RUN_TO n_t_elf, 1
    mv s0, a0
    WAIT_TO s0, 0, 2
    MEMINFO14 3
    RUN_TO n_t_exit42, 4
    mv s0, a0
    WAIT_TO s0, 0, 5
    MEMINFO14 6
    SYSCALL2_TO SYS_SBRK, 4096, 0, 7        /* A has no heap -> -22 */
    li a0, 0                                /* progs(0): size, name -> D_NAME */
    addi a1, s11, D_NAME
    li a7, SYS_PROGS
    ecall
    R14 8
    li a0, 99                               /* no such entry -> -2 */
    addi a1, s11, D_NAME
    li a7, SYS_PROGS
    ecall
    R14 9
    SYSCALL2_TO SYS_PROGS, 0, B_DATA, 10    /* buffer not A's -> -14 */
    DONE14 p_t_elf_done

/* T19: t_sbrk grows its heap to the page limit */
.globl p_t_sbrk
p_t_sbrk:
    mv s11, a0
    MEMINFO14 0
    RUN_TO n_t_sbrk, 1
    mv s0, a0
    WAIT_TO s0, 0, 2
    MEMINFO14 3
    DONE14 p_t_sbrk_done

/* T20: wait() for exit, fault, kill, already ended, errors, and blocking */
.globl p_t_wait
p_t_wait:
    mv s11, a0
    MEMINFO14 0
    RUN_TO n_t_exit42, 1
    mv s0, a0
    WAIT_TO s0, 0, 2                        /* 42 */
    RUN_TO n_b_text, 3
    mv s0, a0
    WAIT_TO s0, 0, 4                        /* faulted -> 139 */
    RUN_TO n_t_spin, 5
    mv s0, a0
    SLEEP14 100
    mv a0, s0
    li a7, SYS_KILL
    ecall
    R14 6                                   /* 0 */
    WAIT_TO s0, 0, 7                        /* killed -> 137 */
    RUN_TO n_t_exit42, 8
    mv s0, a0
    SLEEP14 200
    WAIT_TO s0, 0, 9                        /* already ended -> 42 */
    SYSCALL2_TO SYS_WAIT, 999, 0, 10        /* no such pid -> -3 */
    SYSCALL2_TO SYS_WAIT, 1, 0, 11          /* itself -> -1 */
    WAIT_TO s0, 2, 12                       /* unknown flag -> -22 */
    SYSCALL2_TO SYS_WAIT, 2, 1, 13          /* foreground on boot process B -> -1 */
    RUN_TO n_t_sleepy, 14
    mv s0, a0
.globl p_t_wait_block
p_t_wait_block:
    WAIT_TO s0, 0, 15                       /* blocks about 300 ms -> 7 */
.globl p_t_wait_ret
p_t_wait_ret:
    MEMINFO14 16
    DONE14 p_t_wait_done

/* T21: Ctrl-C ends the foreground process (running, then blocked in read);
 * a second foreground wait is refused; without one Ctrl-C is input. */
.globl p_t_ctrlc
p_t_ctrlc:
    mv s11, a0
    MEMINFO14 0
    RUN_TO n_t_spin, 1                      /* background: must survive */
    mv s1, a0
    RUN_TO n_t_spin, 2
    mv s0, a0
    RUN_TO n_t_fgbusy, 3
    mv s2, a0
.globl p_t_ctrlc_fg1
p_t_ctrlc_fg1:
    WAIT_TO s0, 1, 4                        /* Ctrl-C -> 130 */
.globl p_t_ctrlc_after1
p_t_ctrlc_after1:
    WAIT_TO s2, 0, 5                        /* 16: its foreground wait got -16 */
    RUN_TO n_t_reader, 6
    mv s0, a0
.globl p_t_ctrlc_fg2
p_t_ctrlc_fg2:
    WAIT_TO s0, 1, 7                        /* Ctrl-C while it reads -> 130 */
.globl p_t_ctrlc_plain
p_t_ctrlc_plain:
    li a0, 0                                /* no foreground: 0x03 is input */
    addi a1, s11, D_IN
    li a2, 4
    li a7, SYS_READ
    ecall
    R14 8
    mv a0, s1
    li a7, SYS_KILL
    ecall
    R14 9                                   /* 0 */
    WAIT_TO s1, 0, 10                       /* 137 */
    MEMINFO14 11
    DONE14 p_t_ctrlc_done

/* N10: every malformed file is refused, nothing leaks; then bad names */
.globl p_t_elfbad
p_t_elfbad:
    mv s11, a0
    MEMINFO14 0
    la s0, p_bad_elf_names
    li s1, 0
1:  lbu t0, 0(s0)
    beqz t0, 2f
    mv a0, s0
    call p_run
    slli t0, s1, 3
    add t0, t0, s11
    sd a0, D_R14+16(t0)                     /* words 2.. in list order */
    mv a0, s0
    call p_strlen
    add s0, s0, a0
    addi s0, s0, 1
    addi s1, s1, 1
    j 1b
2:  sd s1, D_R14+8(s11)
    RUN_TO n_nosuch, 64                     /* -2 */
    la a0, n_ok
    li a1, 0                                /* empty name -> -22 */
    li a7, SYS_RUN
    ecall
    R14 65
    la a0, n_ok
    li a1, 16                               /* name too long -> -22 */
    li a7, SYS_RUN
    ecall
    R14 66
    SYSCALL2_TO SYS_RUN, B_DATA, 4, 67      /* name in B's memory -> -14 */
    SYSCALL2_TO SYS_RUN, 0x80200000, 4, 68  /* name in the kernel -> -14 */
    RUN_TO n_ok, 69                         /* the valid file -> pid */
    mv s0, a0
    WAIT_TO s0, 0, 70                       /* 42 */
    MEMINFO14 71
    DONE14 p_t_elfbad_done

/* N11: booted with a malformed archive: no programs at all */
.globl p_t_arcbad
p_t_arcbad:
    mv s11, a0
    li a7, SYS_MEMINFO
    ecall
    R14 0
    sd a1, D_R14+8(s11)
    li a0, 0
    addi a1, s11, D_NAME
    li a7, SYS_PROGS
    ecall
    R14 2                                   /* -2 */
    RUN_TO n_ok, 3                          /* -2 */
    DONE14 p_t_arcbad_done

/* N12: programs that break the rules fault alone; wait() reports 139 */
.globl p_t_userbad
p_t_userbad:
    mv s11, a0
    MEMINFO14 0
    la s0, p_bad_user_names
    li s1, 0
1:  lbu t0, 0(s0)
    beqz t0, 2f
    mv a0, s0
    call p_run
    slli s2, s1, 4
    add s2, s2, s11
    sd a0, D_R14+16(s2)                     /* pid */
    li a1, 0
    li a7, SYS_WAIT
    ecall
    sd a0, D_R14+24(s2)                     /* status */
    mv a0, s0
    call p_strlen
    add s0, s0, a0
    addi s0, s0, 1
    addi s1, s1, 1
    j 1b
2:  sd s1, D_R14+8(s11)
    MEMINFO14 40
    DONE14 p_t_userbad_done

p_nl:           .asciz "\n"
p_hello_msg:    .asciz "hello from pid "
p_pid_prefix:   .asciz "pid "
p_count_mid:    .asciz ": count "
p_crash_msg1:   .asciz "crash: pid "
p_crash_msg2:   .asciz " writes into pid 2's memory...\n"
p_sh_banner:    .ascii "ProtoOS shell: process A in U-mode, B works in the background. Type help.\n"
p_sh_banner_end:
p_sh_prompt_str: .ascii "proto> "
p_sh_prompt_end:
p_sh_bs_seq:    .ascii "\b \b"
p_sh_bs_seq_end:
p_sh_nl:        .ascii "\n"
p_sh_nl_end:
p_sh_help_msg:  .ascii "commands: help, ps, uptime, echo <text>, attack\n"
                .ascii "processes: run <worker|hello|counter|crash>, kill <pid>, mem\n"
                .ascii "programs: ls, run <program> (Ctrl-C stops it), run <program> &\n"
p_sh_help_msg_end:
p_sh_unknown_msg: .ascii "unknown command (try help)\n"
p_sh_unknown_msg_end:
p_sh_run_usage: .ascii "run: usage: run <program> [&] (built in: worker, hello, counter, crash; more: ls)\n"
p_sh_run_usage_end:
p_sh_kill_usage: .ascii "kill: usage: kill <pid>\n"
p_sh_kill_usage_end:
p_sh_att_msg:   .ascii "attack: asking the kernel to print 8 bytes of B's memory...\n"
p_sh_att_msg_end:
p_sh_c_help:    .asciz "help"
p_sh_c_ps:      .asciz "ps"
p_sh_c_uptime:  .asciz "uptime"
p_sh_c_attack:  .asciz "attack"
p_sh_c_mem:     .asciz "mem"
p_sh_c_ls:      .asciz "ls"
p_sh_c_echo:    .asciz "echo"
p_sh_c_run:     .asciz "run "
p_sh_c_kill:    .asciz "kill "
p_sh_up_prefix: .asciz "uptime: "
p_sh_dot:       .asciz "."
p_sh_zero:      .asciz "0"
p_sh_secs:      .asciz " s\n"
p_sh_mem_prefix: .asciz "free pages: "
p_sh_of:        .asciz " of "
p_sh_pid:       .asciz "pid "
p_sh_started:   .asciz "started pid "
p_sh_run_failed: .asciz "run failed: "
p_sh_killed:    .asciz "killed pid "
p_sh_killed2:   .asciz ", "
p_sh_returned:  .asciz " pages returned\n"
p_sh_kill_failed: .asciz "kill failed: "
p_sh_states:    .asciz "UNUSED"
                .asciz "READY"
                .asciz "RUNNING"
                .asciz "EXITED"
                .asciz "FAULTED"
                .asciz "SLEEPING"
                .asciz "BLOCKED"
/* index = program id (src/proc.h); 0 is a placeholder */
p_sh_progs:     .asciz "-"
                .asciz "worker"
                .asciz "hello"
                .asciz "counter"
                .asciz "crash"
                .asciz "memcheck"
p_sh_preempted: .asciz " preempted "
p_sh_syscalls:  .asciz " syscalls "
p_sh_att_ans:   .asciz "kernel answered "
p_sh_att_tail:  .asciz ": access denied, B's memory is not mapped for A\n"
/* v1.4 */
p_sh_intr:      .ascii "^C\n"
p_sh_intr_end:
p_sh_ls_none:   .ascii "no programs (start QEMU with a program archive: -initrd programs.img)\n"
p_sh_ls_none_end:
p_sh_ls_gap:    .asciz "  "
p_sh_bytes:     .asciz " bytes\n"
p_sh_ended:     .asciz " ended with status "
p_sh_noprog:    .asciz "run: no program "
p_sh_tryls:     .asciz " (try ls)\n"
n_t_elf:        .asciz "t_elf"
n_t_sbrk:       .asciz "t_sbrk"
n_t_exit42:     .asciz "t_exit42"
n_t_spin:       .asciz "t_spin"
n_t_sleepy:     .asciz "t_sleepy"
n_t_reader:     .asciz "t_reader"
n_t_fgbusy:     .asciz "t_fgbusy"
n_b_text:       .asciz "b_text"
n_nosuch:       .asciz "nosuch"
n_ok:           .asciz "ok"
/* N10: the order of build-scripts/mkimages.py bad_elfs() */
p_bad_elf_names:
    .asciz "e_magic", "e_class32", "e_bigend", "e_osabi", "e_dyn", "e_machine"
    .asciz "e_float", "e_rvc", "e_phentsize", "e_phoff", "e_nophdr", "e_trunc"
    .asciz "e_interp", "e_dynamic", "e_tls", "e_wx", "e_noread", "e_badflag"
    .asciz "e_memsz", "e_offset", "e_low", "e_kernel", "e_unalign", "e_overlap"
    .asciz "e_wrap", "e_noload", "e_entry", "e_entry_odd", "e_big", "e_empty"
    .byte 0
/* N12 */
p_bad_user_names:
    .asciz "b_text", "b_rodata", "b_kernel", "b_exec", "b_stack", "b_heap"
    .asciz "b_fpu", "b_csr"
    .byte 0
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
