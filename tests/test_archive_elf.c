/* Host tests for the v1.4 program archive parser (src/archive.c) and the
 * ELF checker (src/elf.c): synthetic fixtures, one test per defect,
 * every truncation, and mutation fuzzing whose accepted results must
 * satisfy the documented guarantees. No guest result is implied. */
#include "../src/archive.h"
#include "../src/elf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); abort(); } ++checks; } while (0)

/* ------------------------------------------------------------- ELF fixture */
#define MAXP 57u
static uint8_t elf[0x4000];
static size_t elf_len;

static void w16(uint8_t *p, uint64_t v) { for (int i = 0; i < 2; ++i) p[i] = (uint8_t)(v >> 8 * i); }
static void w32(uint8_t *p, uint64_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> 8 * i); }
static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> 8 * i); }

static uint8_t *ph(unsigned i) { return elf + 64 + 56 * i; }

/* text R X at 0x40000000 (0x800 bytes), rodata R at 0x40001000 (0x100),
 * data R W at 0x40002000 (filesz 0x80, memsz 0x2100: .bss spans pages),
 * plus a PT_NOTE and a PT_GNU_STACK that must be ignored. File data of
 * the last segment ends the file, so every shorter prefix is invalid. */
static void make_elf(void)
{
    memset(elf, 0, sizeof elf);
    memcpy(elf, "\x7f" "ELF", 4);
    elf[4] = 2; elf[5] = 1; elf[6] = 1;
    w16(elf + 16, 2); w16(elf + 18, 243); w32(elf + 20, 1);
    w64(elf + 24, 0x40000010);
    w64(elf + 32, 64);
    w16(elf + 52, 64); w16(elf + 54, 56); w16(elf + 56, 5);
    struct { uint32_t type, flags; uint64_t off, va, fs, ms; } s[5] = {
        {1, 5, 0x1000, 0x40000000, 0x800, 0x800},
        {4, 4, 0x300, 0, 0x20, 0x20},
        {1, 4, 0x1800, 0x40001000, 0x100, 0x100},
        {0x6474e551, 6, 0, 0, 0, 0},
        {1, 6, 0x1900, 0x40002000, 0x80, 0x2100},
    };
    for (unsigned i = 0; i < 5; ++i) {
        w32(ph(i), s[i].type); w32(ph(i) + 4, s[i].flags); w64(ph(i) + 8, s[i].off);
        w64(ph(i) + 16, s[i].va); w64(ph(i) + 24, s[i].va); w64(ph(i) + 32, s[i].fs);
        w64(ph(i) + 40, s[i].ms); w64(ph(i) + 48, 0x1000);
    }
    for (size_t i = 0x1000; i < 0x1980; ++i) elf[i] = (uint8_t)(i * 13);
    elf_len = 0x1980;
}

static int check_elf(const uint8_t *img, size_t n, struct elf_plan *plan)
{
    uint8_t *copy = malloc(n ? n : 1);      /* exact-size buffer: ASan sees overreads */
    memcpy(copy, img, n);
    int r = elf_check(copy, n, MAXP, plan);
    free(copy);
    return r;
}

/* What an accepted plan guarantees, checked independently of elf.c. */
static void plan_invariants(const uint8_t *img, size_t n, const struct elf_plan *p)
{
    uint64_t pages = 0, prev_end = ELF_LOAD_BASE;
    int entry_ok = 0;
    CHECK(p->nsegs >= 1 && p->nsegs <= ELF_MAX_LOADS);
    for (uint64_t i = 0; i < p->nsegs; ++i) {
        const struct elf_segment *s = &p->seg[i];
        CHECK(s->vaddr % 4096 == 0 && s->vaddr >= prev_end && s->memsz > 0);
        CHECK(s->vaddr + s->memsz <= ELF_LOAD_END && s->vaddr + s->memsz > s->vaddr);
        CHECK(s->filesz <= s->memsz && s->offset <= n && s->filesz <= n - s->offset);
        CHECK((s->flags & ELF_PF_R) && !(s->flags & ~7u) &&
              !((s->flags & ELF_PF_W) && (s->flags & ELF_PF_X)));
        uint64_t np = (s->memsz + 4095) / 4096;
        pages += np;
        prev_end = s->vaddr + np * 4096;
        if ((s->flags & ELF_PF_X) && p->entry >= s->vaddr && p->entry < s->vaddr + s->filesz)
            entry_ok = 1;
    }
    CHECK(entry_ok && p->entry % 4 == 0 && pages == p->pages && pages <= MAXP);
    CHECK(p->image_end == prev_end);
    CHECK(n >= 64 && img[0] == 0x7f);
}

static void elf_tests(void)
{
    struct elf_plan p;
    make_elf();
    CHECK(check_elf(elf, elf_len, &p) == ELF_OK);
    CHECK(p.nsegs == 3 && p.entry == 0x40000010 && p.pages == 1 + 1 + 3);
    CHECK(p.seg[0].flags == 5 && p.seg[1].flags == 4 && p.seg[2].flags == 6);
    CHECK(p.seg[2].filesz == 0x80 && p.seg[2].memsz == 0x2100 && p.image_end == 0x40005000);
    plan_invariants(elf, elf_len, &p);
    for (size_t n = 0; n < elf_len; ++n)
        CHECK(check_elf(elf, n, &p) < 0);
    struct { size_t off; unsigned bytes; uint64_t val; int want; } m[] = {
        {0, 1, 0x7e, ELF_E_IDENT}, {3, 1, 'G', ELF_E_IDENT}, {4, 1, 1, ELF_E_IDENT},
        {5, 1, 2, ELF_E_IDENT}, {6, 1, 0, ELF_E_IDENT}, {7, 1, 3, ELF_E_IDENT},
        {8, 1, 1, ELF_E_IDENT}, {16, 2, 3, ELF_E_TYPE}, {16, 2, 1, ELF_E_TYPE},
        {18, 2, 62, ELF_E_TYPE}, {20, 4, 2, ELF_E_TYPE}, {48, 4, 1, ELF_E_FLAGS},
        {48, 4, 2, ELF_E_FLAGS}, {48, 4, 4, ELF_E_FLAGS}, {48, 4, 8, ELF_E_FLAGS},
        {48, 4, 0x10, ELF_E_FLAGS}, {52, 2, 52, ELF_E_PHDRS}, {54, 2, 32, ELF_E_PHDRS},
        {56, 2, 0, ELF_E_PHDRS}, {56, 2, 17, ELF_E_PHDRS}, {32, 8, 0x1980, ELF_E_PHDRS},
        {32, 8, UINT64_MAX - 8, ELF_E_PHDRS}, {32, 8, 0x1980 - 5 * 56 + 1, ELF_E_PHDRS},
        {64 + 56 * 2, 4, 3, ELF_E_SEGTYPE}, {64 + 56 * 2, 4, 2, ELF_E_SEGTYPE},
        {64 + 56 * 4, 4, 7, ELF_E_SEGTYPE}, {64 + 56 * 4, 4, 5, ELF_E_SEGTYPE},
        {64 + 4, 4, 7, ELF_E_PERM}, {64 + 4, 4, 1, ELF_E_PERM}, {64 + 4, 4, 0, ELF_E_PERM},
        {64 + 56 * 2 + 4, 4, 0x14, ELF_E_PERM}, {64 + 56 * 4 + 4, 4, 0x80000006, ELF_E_PERM},
        {64 + 56 * 4 + 40, 8, 0, ELF_E_RANGE}, {64 + 56 * 4 + 40, 8, 0x7f, ELF_E_RANGE},
        {64 + 8, 8, 0x1980, ELF_E_RANGE}, {64 + 8, 8, UINT64_MAX, ELF_E_RANGE},
        {64 + 32, 8, UINT64_MAX, ELF_E_RANGE},
        {64 + 16, 8, 0x3ffff000, ELF_E_ADDR}, {64 + 16, 8, 0x40000004, ELF_E_ADDR},
        {64 + 56 * 2 + 16, 8, 0x40000000, ELF_E_ADDR}, {64 + 56 * 4 + 16, 8, 0x40100000, ELF_E_ADDR},
        {64 + 56 * 4 + 16, 8, 0xfffffffffffff000, ELF_E_ADDR},
        {64 + 56 * 4 + 40, 8, 0x100000, ELF_E_ADDR}, {64 + 56 * 4 + 40, 8, UINT64_MAX, ELF_E_ADDR},
        {64 + 56 * 2 + 16, 8, 0x80200000, ELF_E_ADDR},
        {24, 8, 0x40001000, ELF_E_ENTRY}, {24, 8, 0x40000012, ELF_E_ENTRY},
        {24, 8, 0x40000800, ELF_E_ENTRY}, {24, 8, 0x3ffffffc, ELF_E_ENTRY}, {24, 8, 0, ELF_E_ENTRY},
        {64 + 56 * 4 + 40, 8, 0x40000, ELF_E_SIZE},
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; ++i) {
        make_elf();
        if (m[i].bytes == 1) elf[m[i].off] = (uint8_t)m[i].val;
        if (m[i].bytes == 2) w16(elf + m[i].off, m[i].val);
        if (m[i].bytes == 4) w32(elf + m[i].off, m[i].val);
        if (m[i].bytes == 8) w64(elf + m[i].off, m[i].val);
        int r = check_elf(elf, elf_len, &p);
        if (r != m[i].want) fprintf(stderr, "mutation %u: got %d want %d\n", i, r, m[i].want);
        CHECK(r == m[i].want);
    }
    make_elf();                         /* no PT_LOAD at all */
    for (unsigned i = 0; i < 5; ++i) if (ph(i)[0] == 1) w32(ph(i), 4);
    CHECK(check_elf(elf, elf_len, &p) == ELF_E_LOADS);
    make_elf();                         /* four PT_LOADs are fine, a fifth is not */
    w32(ph(1), 1); w32(ph(1) + 4, 4); w64(ph(1) + 8, 0x1800); w64(ph(1) + 16, 0x40001000);
    w64(ph(1) + 32, 0x20); w64(ph(1) + 40, 0x20);
    w64(ph(2) + 16, 0x40002000);
    w64(ph(4) + 16, 0x40004000);
    CHECK(check_elf(elf, elf_len, &p) == ELF_OK && p.nsegs == 4);
    plan_invariants(elf, elf_len, &p);
    w32(ph(3), 1); w32(ph(3) + 4, 4); w64(ph(3) + 8, 0x1800); w64(ph(3) + 16, 0x40003000);
    w64(ph(3) + 32, 8); w64(ph(3) + 40, 8);
    CHECK(check_elf(elf, elf_len, &p) == ELF_E_LOADS);
    w64(ph(3) + 16, 0x40002000);        /* ... and overlap is reported before the count */
    CHECK(check_elf(elf, elf_len, &p) == ELF_E_ADDR);
    make_elf();                         /* exactly the page limit is fine, one more is not */
    w64(ph(4) + 40, (MAXP - 2) * 4096);
    CHECK(check_elf(elf, elf_len, &p) == ELF_OK && p.pages == MAXP);
    w64(ph(4) + 40, (MAXP - 2) * 4096 + 1);
    CHECK(check_elf(elf, elf_len, &p) == ELF_E_SIZE);
    /* mutation fuzz: never a crash; anything accepted satisfies the guarantees */
    srand(140);
    unsigned accepted = 0;
    for (unsigned k = 0; k < 200000; ++k) {
        make_elf();
        unsigned edits = 1 + (unsigned)rand() % 4;
        for (unsigned e = 0; e < edits; ++e) {
            size_t off = (size_t)rand() % (64 + 56 * 5);
            elf[off] = (uint8_t)(rand() % 3 ? rand() : (rand() % 2 ? 0 : 0xff));
        }
        size_t n = rand() % 8 ? elf_len : (size_t)rand() % elf_len;
        if (check_elf(elf, n, &p) == ELF_OK) { plan_invariants(elf, n, &p); ++accepted; }
    }
    printf("elf: fuzz accepted %u of 200000 mutated files (all satisfied the guarantees)\n", accepted);
}

/* --------------------------------------------------------- archive fixture */
static uint8_t arc[0x10000];
static size_t arc_len, trailer_end;

static void entry(const char *name, const uint8_t *data, size_t n, unsigned mode)
{
    char hdr[160];
    size_t ns = strlen(name) + 1;
    snprintf(hdr, sizeof hdr, "070701%08X%08X%08X%08X%08X%08X%08zX%08X%08X%08X%08X%08zX%08X",
             1u, mode, 0u, 0u, 1u, 0u, n, 0u, 0u, 0u, 0u, ns, 0u);
    assert(strlen(hdr) == 110);
    memcpy(arc + arc_len, hdr, 110); arc_len += 110;
    memcpy(arc + arc_len, name, ns); arc_len += ns;
    while (arc_len % 4) arc[arc_len++] = 0;
    memcpy(arc + arc_len, data, n); arc_len += n;
    while (arc_len % 4) arc[arc_len++] = 0;
}

static const uint8_t blob[300] = {1, 2, 3};

static void make_arc(void)
{
    memset(arc, 0, sizeof arc);
    arc_len = 0;
    entry("alpha", blob, 300, 0100555);
    entry("b2", blob, 7, 0100644);
    entry("c_3", blob, 0, 0100555);
    trailer_end = arc_len + 110 + 11;
    entry("TRAILER!!!", blob, 0, 0);
}

static int parse(const uint8_t *img, size_t n, struct archive *a)
{
    uint8_t *copy = malloc(n ? n : 1);
    memcpy(copy, img, n);
    int r = archive_parse(copy, n, a);
    free(copy);
    return r;
}

static int name_ok(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > ARCHIVE_NAME_MAX || s[0] < 'a' || s[0] > 'z') return 0;
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return 0;
    return 1;
}

static void arc_invariants(size_t n, const struct archive *a)
{
    CHECK(a->count <= ARCHIVE_MAX_FILES);
    for (uint64_t i = 0; i < a->count; ++i) {
        const struct archive_file *f = &a->files[i];
        CHECK(name_ok(f->name) && f->offset <= n && f->size <= n - f->offset);
        for (uint64_t j = 0; j < i; ++j) CHECK(strcmp(a->files[j].name, f->name) != 0);
    }
}

static void archive_tests(void)
{
    static struct archive a;
    make_arc();
    CHECK(parse(arc, arc_len, &a) == ARCHIVE_OK && a.count == 3);
    CHECK(strcmp(a.files[0].name, "alpha") == 0 && a.files[0].size == 300 &&
          a.files[0].offset == 116 && a.files[0].offset % 4 == 0);
    CHECK(strcmp(a.files[1].name, "b2") == 0 && a.files[1].size == 7);
    CHECK(strcmp(a.files[2].name, "c_3") == 0 && a.files[2].size == 0);
    CHECK(archive_find(&a, "alpha", 5) == 0 && archive_find(&a, "b2", 2) == 1 &&
          archive_find(&a, "c_3", 3) == 2 && archive_find(&a, "alph", 4) == -1 &&
          archive_find(&a, "alphax", 6) == -1 && archive_find(&a, "b2", 0) == -1 &&
          archive_find(&a, "b2xxxxxxxxxxxxxx", 16) == -1);
    arc_invariants(arc_len, &a);
    for (size_t n = 0; n < arc_len; ++n) {
        int r = parse(arc, n, &a);
        CHECK(n >= trailer_end ? r == ARCHIVE_OK : r < 0);
        if (r < 0) CHECK(a.count == 0);
    }
    CHECK(parse(arc, 0, &a) == ARCHIVE_E_SIZE);
    CHECK(archive_parse(arc, ARCHIVE_MAX_BYTES + 1, &a) == ARCHIVE_E_SIZE);
    struct { size_t off; const char *bytes; int want; } m[] = {
        {5, "2", ARCHIVE_E_MAGIC}, {0, "1", ARCHIVE_E_MAGIC}, {6 + 8 * 6 + 7, "g", ARCHIVE_E_HEADER},
        {6 + 8 * 1, " ", ARCHIVE_E_HEADER}, {6 + 8 * 11, "Z", ARCHIVE_E_HEADER},
        {6 + 8 * 11, "00000000", ARCHIVE_E_NAME}, {6 + 8 * 11, "00000001", ARCHIVE_E_NAME},
        {6 + 8 * 11, "0000FFFF", ARCHIVE_E_NAME}, {110, "A", ARCHIVE_E_NAME},
        {110, "1", ARCHIVE_E_NAME}, {111, "-", ARCHIVE_E_NAME}, {112, ".", ARCHIVE_E_NAME},
        {6 + 8 * 11, "00000005", ARCHIVE_E_NAME},  /* NUL not at namesize-1 */
        {6 + 8 * 1, "00040755", ARCHIVE_E_TYPE}, {6 + 8 * 1, "00120777", ARCHIVE_E_TYPE},
        {6 + 8 * 12, "00000001", ARCHIVE_E_TYPE}, {6 + 8 * 6, "0000FFFF", ARCHIVE_E_DATA},
        {6 + 8 * 6, "FFFFFFFF", ARCHIVE_E_DATA},
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; ++i) {
        make_arc();
        memcpy(arc + m[i].off, m[i].bytes, strlen(m[i].bytes));
        int r = parse(arc, arc_len, &a);
        if (r != m[i].want) fprintf(stderr, "archive mutation %u: got %d want %d\n", i, r, m[i].want);
        CHECK(r == m[i].want && a.count == 0);
    }
    /* names of exactly 15 and 16 characters */
    memset(arc, 0, sizeof arc); arc_len = 0;
    entry("abcdefghijklmno", blob, 1, 0100555); entry("TRAILER!!!", blob, 0, 0);
    CHECK(parse(arc, arc_len, &a) == ARCHIVE_OK && a.count == 1);
    memset(arc, 0, sizeof arc); arc_len = 0;
    entry("abcdefghijklmnop", blob, 1, 0100555); entry("TRAILER!!!", blob, 0, 0);
    CHECK(parse(arc, arc_len, &a) == ARCHIVE_E_NAME);
    /* duplicates, too many files, no trailer, bytes after the trailer */
    memset(arc, 0, sizeof arc); arc_len = 0;
    entry("x", blob, 1, 0100555); entry("x", blob, 2, 0100555); entry("TRAILER!!!", blob, 0, 0);
    CHECK(parse(arc, arc_len, &a) == ARCHIVE_E_DUP);
    memset(arc, 0, sizeof arc); arc_len = 0;
    entry("ab", blob, 1, 0100555); entry("abc", blob, 2, 0100555); entry("a", blob, 2, 0100555);
    entry("TRAILER!!!", blob, 0, 0);
    CHECK(parse(arc, arc_len, &a) == ARCHIVE_OK && a.count == 3);
    for (unsigned files = 32; files <= 33; ++files) {
        memset(arc, 0, sizeof arc); arc_len = 0;
        for (unsigned i = 0; i < files; ++i) {
            char nm[8];
            snprintf(nm, sizeof nm, "f%u", i);
            entry(nm, blob, 1, 0100555);
        }
        entry("TRAILER!!!", blob, 0, 0);
        int r = parse(arc, arc_len, &a);
        CHECK(files == 32 ? (r == ARCHIVE_OK && a.count == 32) : r == ARCHIVE_E_COUNT);
    }
    make_arc();
    CHECK(parse(arc, trailer_end - 11 - 110, &a) == ARCHIVE_E_TRAILER);
    arc[arc_len] = 'j';
    CHECK(parse(arc, arc_len + 1, &a) == ARCHIVE_E_TAIL);
    arc[arc_len] = 0;
    CHECK(parse(arc, arc_len + 64, &a) == ARCHIVE_OK);      /* zero padding is fine */
    memset(arc, 0, sizeof arc); arc_len = 0;
    entry("TRAILER!!!", blob, 0, 0);
    CHECK(parse(arc, arc_len, &a) == ARCHIVE_OK && a.count == 0);
    memcpy(arc + 6 + 8 * 6, "00000001", 8);
    CHECK(parse(arc, arc_len, &a) == ARCHIVE_E_DATA);       /* trailer with data */
    /* archive_range_ok */
    CHECK(archive_range_ok(0x84200000, 0x84207530, 0x80400000, 0x87e00000));
    CHECK(!archive_range_ok(0x84200000, 0x84200000, 0x80400000, 0x87e00000));
    CHECK(!archive_range_ok(0x84200000, 0x841fffff, 0x80400000, 0x87e00000));
    CHECK(!archive_range_ok(0x803fffff, 0x80400100, 0x80400000, 0x87e00000));
    CHECK(!archive_range_ok(0x87dfff00, 0x87e00001, 0x80400000, 0x87e00000));
    CHECK(archive_range_ok(0x87dfff00, 0x87e00000, 0x80400000, 0x87e00000));
    CHECK(archive_range_ok(0x80400000, 0x80400000 + ARCHIVE_MAX_BYTES + 1, 0x80400000, 0x88000000));
    CHECK(archive_range_ok(0x80400000, 0x87e00000, 0x80400000, 0x87e00000));  /* the whole pool */
    CHECK(archive_range_ok(0x80400000, 0x80400000 + ARCHIVE_MAX_BYTES, 0x80400000, 0x88000000));
    CHECK(!archive_range_ok(UINT64_MAX - 10, UINT64_MAX, 0x80400000, UINT64_MAX - 1));
    /* mutation fuzz */
    srand(141);
    unsigned accepted = 0;
    for (unsigned k = 0; k < 200000; ++k) {
        make_arc();
        unsigned edits = 1 + (unsigned)rand() % 4;
        for (unsigned e = 0; e < edits; ++e) {
            size_t off = (size_t)rand() % arc_len;
            static const char pick[] = "0123456789ABCDEFabcdef!_xyzT\0\377";
            arc[off] = rand() % 2 ? (uint8_t)pick[rand() % (sizeof pick - 1)] : (uint8_t)rand();
        }
        size_t n = rand() % 8 ? arc_len : (size_t)rand() % arc_len;
        if (parse(arc, n, &a) == ARCHIVE_OK) { arc_invariants(n, &a); ++accepted; }
        else CHECK(a.count == 0);
    }
    printf("archive: fuzz accepted %u of 200000 mutated archives (all satisfied the guarantees)\n",
           accepted);
}

int main(void)
{
    elf_tests();
    archive_tests();
    printf("ARCHIVE_ELF_HOST_TESTS_PASS checks=%u\n", checks);
    return 0;
}
