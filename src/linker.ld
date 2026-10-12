OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x80200000;
    __kernel_start = .;
    .text : ALIGN(4096) { KEEP(*(.text.boot)) *(.text .text.*) }
    . = ALIGN(4096);
    __text_end = .;
    .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) }
    . = ALIGN(4096);
    __rodata_end = .;
    .data : { *(.data .data.*) *(.sdata .sdata.*) }
    PROVIDE(__global_pointer$ = ADDR(.data) + 0x800);
    . = ALIGN(4096);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss.kstack) *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
    . = ALIGN(8);
    __bss_end = .;
    . = ALIGN(4096);
    __kernel_end = .;
    /* User images. Mapped ONLY into the owning process (U=1); the kernel
     * identity map ends at __kernel_end and never covers these pages. */
    .user_a_text : ALIGN(4096) { __user_a_text = .; KEEP(*(.user.a.text)) }
    . = ALIGN(4096);
    __user_a_text_end = .;
    .user_b_text : { __user_b_text = .; KEEP(*(.user.b.text)) }
    . = ALIGN(4096);
    __user_b_text_end = .;
    .user_bss (NOLOAD) : { __user_bss_start = .; KEEP(*(.user.bss)) }
    . = ALIGN(4096);
    __user_bss_end = .;
    __image_end = .;
    /DISCARD/ : { *(.eh_frame*) *(.comment) }
}
ASSERT(__user_a_text_end - __user_a_text == 4096, "user A text must fit one page")
ASSERT(__user_b_text_end - __user_b_text == 4096, "user B text must fit one page")
ASSERT(__user_bss_end - __user_bss_start == 4 * 4096, "user data/stack pages")
ASSERT(__image_end <= 0x80400000, "kernel image exceeds reserved 2MiB")
