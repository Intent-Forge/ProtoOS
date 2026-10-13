/* ProtoOS v1.4 programs: static ELF, three PT_LOAD segments with exact
 * permissions (code R X, constants R, data and bss R W), each starting on
 * its own page inside the kernel's ELF window [0x40000000, 0x40100000). */
OUTPUT_ARCH(riscv)
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x40000000;
    .text : { KEEP(*(.text.start)) *(.text .text.*) } :text
    . = ALIGN(4096);
    .rodata : { KEEP(*(.rodata.proto)) *(.rodata .rodata.*) *(.srodata .srodata.*) } :rodata
    . = ALIGN(4096);
    .data : { KEEP(*(.data.proto)) *(.data .data.*) *(.sdata .sdata.*) } :data
    .bss : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) } :data
    . = ALIGN(4096);
    _end = .;
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.riscv.attributes) *(.note*) }
}
ASSERT(_end <= 0x40100000, "program image exceeds the ELF window")
