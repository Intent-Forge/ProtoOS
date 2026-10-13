/* ProtoOS v1.4 program entry. The kernel starts a program at e_entry with
 * sp = 0x40200000 (top of its stack) and every other register 0; main's
 * return value becomes the exit code. */
.option norvc
.section .text.start,"ax",@progbits
.globl _start
_start:
    call main
    li a7, 93
    ecall
1:  j 1b

/* Every program has all three segments (the kernel refuses empty ones). */
.section .rodata.proto,"a",@progbits
.globl proto_program_tag
proto_program_tag:
    .asciz "ProtoOS v1.4 program"
.section .data.proto,"aw",@progbits
.balign 8
.globl proto_data_tag
proto_data_tag:
    .dword 0x50524f544f4f5331
