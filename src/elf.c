#include "elf.h"

#define EHDR_SIZE 64u
#define PHDR_SIZE 56u
#define ET_EXEC 2u
#define EM_RISCV 243u
#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PT_SHLIB 5u
#define PT_TLS 7u
#define PAGE UINT64_C(4096)

static uint64_t rd(const uint8_t *p, unsigned bytes)
{
    uint64_t v = 0;
    while (bytes--) v = v << 8 | p[bytes];
    return v;
}

static uint64_t pages_of(uint64_t bytes)
{
    return bytes / PAGE + (bytes % PAGE != 0);
}

int elf_check(const uint8_t *img, uint64_t size, uint64_t max_pages, struct elf_plan *out)
{
    out->nsegs = 0;
    out->pages = 0;
    if (size < EHDR_SIZE || img[0] != 0x7f || img[1] != 'E' || img[2] != 'L' ||
        img[3] != 'F' || img[4] != 2 /* 64-bit */ || img[5] != 1 /* little endian */ ||
        img[6] != 1 /* version */ || img[7] != 0 /* System V ABI */ || img[8] != 0)
        return ELF_E_IDENT;
    if (rd(img + 16, 2) != ET_EXEC || rd(img + 18, 2) != EM_RISCV || rd(img + 20, 4) != 1)
        return ELF_E_TYPE;
    if (rd(img + 48, 4) != 0) return ELF_E_FLAGS;
    uint64_t phoff = rd(img + 32, 8), phnum = rd(img + 56, 2);
    if (rd(img + 52, 2) != EHDR_SIZE || rd(img + 54, 2) != PHDR_SIZE || phnum == 0 ||
        phnum > ELF_MAX_PHDRS || phoff > size || phnum * PHDR_SIZE > size - phoff)
        return ELF_E_PHDRS;
    uint64_t next = ELF_LOAD_BASE;      /* lowest address the next segment may use */
    for (uint64_t i = 0; i < phnum; ++i) {
        const uint8_t *ph = img + phoff + i * PHDR_SIZE;
        uint64_t type = rd(ph, 4);
        if (type == PT_DYNAMIC || type == PT_INTERP || type == PT_SHLIB || type == PT_TLS)
            return ELF_E_SEGTYPE;
        if (type != PT_LOAD) continue;  /* PT_NOTE, PT_GNU_STACK, attributes: not loaded */
        uint64_t flags = rd(ph + 4, 4), offset = rd(ph + 8, 8), vaddr = rd(ph + 16, 8);
        uint64_t filesz = rd(ph + 32, 8), memsz = rd(ph + 40, 8);
        if (!(flags & ELF_PF_R) || (flags & ~UINT64_C(7)) ||
            ((flags & ELF_PF_W) && (flags & ELF_PF_X)))
            return ELF_E_PERM;
        if (memsz == 0 || filesz > memsz || offset > size || filesz > size - offset)
            return ELF_E_RANGE;
        if (vaddr % PAGE || vaddr < next || vaddr >= ELF_LOAD_END ||
            memsz > ELF_LOAD_END - vaddr)
            return ELF_E_ADDR;
        if (out->nsegs == ELF_MAX_LOADS) return ELF_E_LOADS;
        struct elf_segment *s = &out->seg[out->nsegs++];
        s->vaddr = vaddr;
        s->memsz = memsz;
        s->offset = offset;
        s->filesz = filesz;
        s->flags = flags;
        out->pages += pages_of(memsz);
        next = vaddr + pages_of(memsz) * PAGE;
    }
    if (out->nsegs == 0) return ELF_E_LOADS;
    uint64_t entry = rd(img + 24, 8);
    int entry_ok = 0;
    for (uint64_t i = 0; i < out->nsegs; ++i) {
        const struct elf_segment *s = &out->seg[i];
        if ((s->flags & ELF_PF_X) && entry >= s->vaddr && entry - s->vaddr < s->filesz)
            entry_ok = 1;
    }
    if (!entry_ok || entry % 4u) return ELF_E_ENTRY;
    if (out->pages > max_pages) return ELF_E_SIZE;
    out->entry = entry;
    out->image_end = next;
    return ELF_OK;
}
