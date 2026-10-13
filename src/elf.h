#ifndef PROTOOS_ELF_H
#define PROTOOS_ELF_H
#include <stdint.h>

/* v1.4 program files: static ELF64 RISC-V executables (ET_EXEC), little
 * endian, soft-float, no compressed instructions (e_flags = 0), linked to
 * load inside [ELF_LOAD_BASE, ELF_LOAD_END) (see user/user.ld). Every
 * check happens before the kernel allocates anything. Pure function: no
 * kernel state, host-testable; the image is read bytewise (no alignment
 * assumptions about where the archive put the file). */
#define ELF_LOAD_BASE UINT64_C(0x40000000)
#define ELF_LOAD_END  UINT64_C(0x40100000)
#define ELF_MAX_LOADS 4u
#define ELF_MAX_PHDRS 16u
#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u

struct elf_segment {
    uint64_t vaddr, memsz;      /* vaddr is page aligned */
    uint64_t offset, filesz;    /* bytes copied from the file */
    uint64_t flags;             /* ELF_PF_R, optionally W or X (never both) */
};
struct elf_plan {
    uint64_t entry;
    uint64_t nsegs;
    struct elf_segment seg[ELF_MAX_LOADS];
    uint64_t pages;             /* 4 KiB pages the segments occupy */
    uint64_t image_end;         /* page after the last segment: initial break */
};

enum elf_result {
    ELF_OK = 0,
    ELF_E_IDENT = -1,           /* too short, magic, class, data, version, OS ABI */
    ELF_E_TYPE = -2,            /* not ET_EXEC, not RISC-V, bad e_version */
    ELF_E_FLAGS = -3,           /* e_flags != 0 (float ABI, RVC, RVE, TSO, ...) */
    ELF_E_PHDRS = -4,           /* header sizes, phdr count or table outside the file */
    ELF_E_SEGTYPE = -5,         /* PT_DYNAMIC, PT_INTERP, PT_SHLIB or PT_TLS */
    ELF_E_PERM = -6,            /* W and X together, no R, unknown flag bits */
    ELF_E_RANGE = -7,           /* memsz 0, filesz > memsz, file data outside the file */
    ELF_E_ADDR = -8,            /* unaligned, outside the window, unsorted or overlapping */
    ELF_E_LOADS = -9,           /* no PT_LOAD or more than ELF_MAX_LOADS */
    ELF_E_ENTRY = -10,          /* entry not 4-aligned inside an executable segment */
    ELF_E_SIZE = -11,           /* more pages than allowed */
};

/* Validate img[0..size) and describe what to load. max_pages bounds the
 * pages the segments may occupy. */
int elf_check(const uint8_t *img, uint64_t size, uint64_t max_pages, struct elf_plan *out);
#endif
