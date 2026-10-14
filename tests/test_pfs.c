/* Host tests for src/pfs.c (v1.5), plain and with ASan/UBSan. The main,
 * deterministic proof of the crash rule "every file is entirely old or
 * entirely new" within the fault model of docs/SPEC_V1_5.md:
 *
 *   - a scripted run of creates, replaces and deletes over several files;
 *   - it is stopped at every device call (each 4 KiB write and each flush);
 *   - the writes not yet flushed are then lost, kept, kept in reverse
 *     order, kept with the in-flight write stopped after every one of its
 *     8 sectors, or kept/lost/torn at random per sector (old, new or
 *     garbage bytes);
 *   - a failed write or flush at every call (EIO, the file system stops).
 * After every stop the image is mounted again; the directory must be
 * exactly the one before or the one after the interrupted operation, every
 * file must read back with the content of that state, and the file system
 * must accept new operations. Also: API behaviour, directory fuzzing, and
 * images for pfsck.py to agree on (tests/test_pfsck_agree.py).
 *
 * Usage: test_pfs [dump-dir]   (dump-dir: write the fuzz images there) */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/pfs.h"

#define BLOCKS 256u
#define SECTOR 512u
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* ------------------------------------------------------- simulated disk */
static uint8_t durable[BLOCKS * PFS_BLOCK];
static struct { uint32_t block; uint8_t data[PFS_BLOCK]; } pending[64];
static unsigned npending;
static long calls, stop_at = -1, fail_at = -1;
static jmp_buf stopped;

static uint8_t *view(uint32_t block)
{
    for (unsigned i = npending; i-- > 0;)
        if (pending[i].block == block) return pending[i].data;
    return durable + (size_t)block * PFS_BLOCK;
}

/* Every device call counts; call number stop_at never happens (power
 * gone just before it), call number fail_at returns an error. */
static int gate(void)
{
    long n = calls++;
    if (n == stop_at) longjmp(stopped, 1);
    return n == fail_at ? -5 : 0;
}

static int sim_read(void *ctx, uint32_t block, uint8_t *buf)
{
    (void)ctx;
    if (block >= BLOCKS) return -5;
    memcpy(buf, view(block), PFS_BLOCK);
    return 0;
}

static int sim_write(void *ctx, uint32_t block, const uint8_t *buf)
{
    (void)ctx;
    if (gate() || block >= BLOCKS) return -5;
    CHECK(npending < 64);
    pending[npending].block = block;
    memcpy(pending[npending++].data, buf, PFS_BLOCK);
    return 0;
}

static int sim_flush(void *ctx)
{
    (void)ctx;
    if (gate()) return -5;
    for (unsigned i = 0; i < npending; ++i)
        memcpy(durable + (size_t)pending[i].block * PFS_BLOCK, pending[i].data, PFS_BLOCK);
    npending = 0;
    return 0;
}

static struct pfs_dev sim = { 0, sim_read, sim_write, sim_flush, BLOCKS, 0 };

/* A plain device over an image buffer (for mounting after a stop). */
static uint8_t *img_buf;
static int img_read(void *c, uint32_t b, uint8_t *o) { (void)c; if (b >= BLOCKS) return -5; memcpy(o, img_buf + (size_t)b * PFS_BLOCK, PFS_BLOCK); return 0; }
static int img_write(void *c, uint32_t b, const uint8_t *i) { (void)c; if (b >= BLOCKS) return -5; memcpy(img_buf + (size_t)b * PFS_BLOCK, i, PFS_BLOCK); return 0; }
static int img_flush(void *c) { (void)c; return 0; }
static struct pfs_dev imgdev = { 0, img_read, img_write, img_flush, BLOCKS, 0 };

/* --------------------------------------------------------- random, data */
static uint64_t rng = 0x9e3779b97f4a7c15u;
static uint64_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static uint8_t content(uint32_t seed, uint32_t i)
{
    uint32_t x = seed * 2654435761u + i * 40503u;
    return (uint8_t)(x ^ (x >> 13) ^ (x >> 7));
}

static uint8_t data[PFS_FILE_MAX], rbuf[PFS_FILE_MAX];
static void make(uint32_t seed, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) data[i] = content(seed, i);
}

/* ---------------------------------------------------- empty image (mkpfs) */
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static void fix_crc(uint8_t *b, unsigned at)
{
    put32(b + at, 0);
    put32(b + at, pfs_crc32(0, b, PFS_BLOCK));
}

static void empty_image(uint8_t *img)
{
    memset(img, 0, BLOCKS * PFS_BLOCK);
    memcpy(img, "PFS1", 4);
    put32(img + 4, 1);
    put32(img + 8, BLOCKS);
    fix_crc(img, 12);
    for (unsigned s = 1; s <= 2; ++s) {
        uint8_t *d = img + s * PFS_BLOCK;
        memcpy(d, "PFSDIR01", 8);
        put64(d + 8, s == 1 ? 1 : 0);
        fix_crc(d, 16);
    }
}

/* ------------------------------------------------------------- the model */
/* The script's 6 names, then 60 empty filler files written before it
 * (audit: the directory's later sectors must take part too). After the
 * fillers in slots 9, 18, 27, 36, 45 and 57 are deleted, the script's files
 * land in directory sectors 1-5 and 7, and fillers fill every sector. */
#define SCRIPT_NAMES 6
#define FILLERS 60
#define NAMES (SCRIPT_NAMES + FILLERS)
static const char *names[NAMES] = { "a", "b", "c", "dd", "e.txt", "f_9" };
static char filler_name[FILLERS][4];
static const int freed_slot[6] = { 9, 18, 27, 36, 45, 57 };
struct model { int exists[NAMES]; uint32_t size[NAMES], seed[NAMES]; };
struct op { int kind; int name; uint32_t size, seed; };     /* kind 0 write, 1 delete */

#define OPS 70
static struct op script[OPS];
static int expect_ok[OPS];

static void apply(struct model *m, const struct op *o)
{
    if (o->kind == 0) {
        m->exists[o->name] = 1;
        m->size[o->name] = o->size;
        m->seed[o->name] = o->seed;
    } else {
        m->exists[o->name] = 0;
    }
}

static uint32_t pick_size(void)
{
    switch (rnd() % 6) {
    case 0: return 0;
    case 1: return (uint32_t)(rnd() % 100);
    case 2: return (uint32_t)(rnd() % 10000);
    case 3: return PFS_BLOCK * (uint32_t)(1 + rnd() % 4);
    case 4: return PFS_FILE_MAX - (uint32_t)(rnd() % 3);
    default: return (uint32_t)(rnd() % PFS_FILE_MAX);
    }
}

static void make_script(void)
{
    for (int i = 0; i < OPS; ++i) {
        script[i].kind = rnd() % 5 == 0;
        script[i].name = (int)(rnd() % SCRIPT_NAMES);
        script[i].size = pick_size();
        script[i].seed = (uint32_t)rnd();
    }
}

static int run_op(struct pfs *fs, const struct op *o)
{
    const char *n = names[o->name];
    if (o->kind) return pfs_delete(fs, n, strlen(n));
    make(o->seed, o->size);
    return pfs_write(fs, n, strlen(n), data, o->size);
}

/* 1 if the mounted fs holds exactly the files of m with their content */
static int matches(struct pfs *fs, const struct model *m)
{
    unsigned want = 0;
    for (int k = 0; k < NAMES; ++k) want += (unsigned)m->exists[k];
    if (fs->files != want) return 0;
    for (int k = 0; k < NAMES; ++k) {
        int i = pfs_find(fs, names[k], strlen(names[k]));
        if (!m->exists[k]) { if (i >= 0) return 0; continue; }
        if (i < 0) return 0;
        int n = pfs_read(fs, (uint32_t)i, rbuf);
        if (n != (int)m->size[k]) return 0;
        for (uint32_t b = 0; b < m->size[k]; ++b)
            if (rbuf[b] != content(m->seed[k], b)) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------ the checks */
static long mounts, variants;

/* Mount img (a stopped disk), require state `before` or `after`, then
 * require that the file system takes a new write. */
static void verify(uint8_t *img, const struct model *before, const struct model *after)
{
    static struct pfs fs;
    img_buf = img;
    int r = pfs_mount(&fs, &imgdev);
    CHECK(r == 0);
    int ok_before = matches(&fs, before), ok_after = matches(&fs, after);
    if (!ok_before && !ok_after) {
        fprintf(stderr, "FAIL: mounted state is neither the old nor the new one\n");
        exit(1);
    }
    make(12345, 777);
    int w = pfs_write(&fs, "zz", 2, data, 777);
    CHECK(w == 0 || w == PFS_ENOSPC);
    if (w == 0) {
        CHECK(pfs_read(&fs, (uint32_t)pfs_find(&fs, "zz", 2), rbuf) == 777);
        CHECK(memcmp(rbuf, data, 777) == 0);
    }
    mounts++;
}

static uint8_t img2[BLOCKS * PFS_BLOCK];

/* Apply pending[0..n) in `order` to a copy of durable; sector choice per
 * write: mode 0 new, 1 keep only the first `cut` sectors of the last one,
 * 2 random per sector (old / new / garbage), 3 random subset of writes. */
static void build(int reverse, int mode, unsigned cut)
{
    memcpy(img2, durable, sizeof(img2));
    for (unsigned j = 0; j < npending; ++j) {
        unsigned i = reverse ? npending - 1 - j : j;
        uint8_t *dst = img2 + (size_t)pending[i].block * PFS_BLOCK;
        if (mode == 3 && rnd() % 2) continue;
        for (unsigned s = 0; s < PFS_BLOCK / SECTOR; ++s) {
            const uint8_t *src = pending[i].data + s * SECTOR;
            int choice = 1;                                     /* new */
            if (mode == 1 && i == npending - 1 && s >= cut) choice = 0;
            if (mode == 2) choice = (int)(rnd() % 3);           /* old, new, garbage */
            if (choice == 1) memcpy(dst + s * SECTOR, src, SECTOR);
            if (choice == 2)
                for (unsigned b = 0; b < SECTOR; ++b) dst[s * SECTOR + b] = (uint8_t)rnd();
        }
    }
}

static void every_variant(const struct model *before, const struct model *after)
{
    memcpy(img2, durable, sizeof(img2));            /* all unflushed writes lost */
    verify(img2, before, after);
    build(0, 0, 0); verify(img2, before, after);    /* all kept */
    build(1, 0, 0); verify(img2, before, after);    /* kept in reverse order */
    if (npending)
        for (unsigned cut = 0; cut <= PFS_BLOCK / SECTOR; ++cut) {
            build(0, 1, cut);                       /* last write stopped after sector cut */
            verify(img2, before, after);
        }
    for (int k = 0; k < 6; ++k) { build(0, 2, 0); verify(img2, before, after); }
    for (int k = 0; k < 4; ++k) { build(k & 1, 3, 0); verify(img2, before, after); }
    variants++;
}

static void reset_disk(void)
{
    empty_image(durable);
    npending = 0;
    calls = 0;
}

/* Before the script: 60 empty fillers, 6 of them deleted again, then 3
 * files, and the disk mounted again: 69 changes, so the mounted copy is B
 * (seq 70) and copy A (seq 69) differs from it. Device calls are counted
 * from here. */
static void prepare(struct pfs *fs, struct model *m)
{
    static const struct op files[3] = { { 0, 0, 5000, 11 }, { 0, 1, 100, 12 }, { 0, 2, 0, 13 } };
    struct op prefix[FILLERS + 6 + 3];
    int n = 0;
    for (int i = 0; i < FILLERS; ++i) prefix[n++] = (struct op){ 0, SCRIPT_NAMES + i, 0, 0 };
    for (int i = 0; i < 6; ++i) prefix[n++] = (struct op){ 1, SCRIPT_NAMES + freed_slot[i], 0, 0 };
    for (int i = 0; i < 3; ++i) prefix[n++] = files[i];
    reset_disk();
    stop_at = fail_at = -1;
    memset(m, 0, sizeof(*m));
    CHECK(pfs_mount(fs, &sim) == 0);
    for (int i = 0; i < n; ++i) {
        CHECK(run_op(fs, &prefix[i]) == 0);
        apply(m, &prefix[i]);
    }
    CHECK(pfs_mount(fs, &sim) == 0 && fs->slot == 2 && fs->seq == 70 && matches(fs, m));
    /* the script's first files took freed slots spread over the sectors */
    CHECK(pfs_find(fs, "a", 1) == 9 && pfs_find(fs, "b", 1) == 18 && pfs_find(fs, "c", 1) == 27);
    calls = 0;
}

/* The reference run: every operation's result without any stop. */
static long reference(void)
{
    static struct pfs fs;
    struct model m;
    prepare(&fs, &m);
    for (int i = 0; i < OPS; ++i) {
        int r = run_op(&fs, &script[i]);
        CHECK(r == 0 || r == PFS_ENOSPC || (script[i].kind == 1 && r == PFS_ENOENT));
        expect_ok[i] = r == 0;
        if (r == 0) apply(&m, &script[i]);
        CHECK(matches(&fs, &m));
    }
    return calls;
}

/* Replay until device call `k`; then check every fault variant there. */
static void stop_test(long k, int as_failure)
{
    /* static: they change between setjmp and longjmp */
    static struct pfs fs;
    static struct model before, after;
    prepare(&fs, &before);
    after = before;
    stop_at = as_failure ? -1 : k;
    fail_at = as_failure ? k : -1;
    volatile int i = 0;
    if (!setjmp(stopped)) {
        for (; i < OPS; ++i) {
            after = before;
            if (expect_ok[i]) apply(&after, &script[i]);
            int r = run_op(&fs, &script[i]);
            if (as_failure && r == PFS_EIO) {
                /* stops here: everything after this is EIO */
                CHECK(fs.failed);
                CHECK(run_op(&fs, &script[(i + 1) % OPS]) == PFS_EIO);
                break;
            }
            CHECK((r == 0) == expect_ok[i]);
            before = after;
        }
    }
    stop_at = fail_at = -1;
    if (i < OPS) every_variant(&before, &after);
}

/* ------------------------------------------------------------- API tests */
static void api_tests(void)
{
    static struct pfs fs;
    reset_disk();
    CHECK(pfs_mount(&fs, &sim) == 0 && fs.files == 0 && fs.slot == 1 && fs.seq == 1);
    CHECK(pfs_write(&fs, "", 0, data, 1) == PFS_EINVAL);
    CHECK(pfs_write(&fs, "Abc", 3, data, 1) == PFS_EINVAL);
    CHECK(pfs_write(&fs, "abcdefghijklmnop", 16, data, 1) == PFS_EINVAL);
    CHECK(pfs_write(&fs, "abcdefghijklmno", 15, data, 1) == 0);
    CHECK(pfs_write(&fs, "x", 1, data, PFS_FILE_MAX + 1) == PFS_EFBIG);
    CHECK(pfs_delete(&fs, "nosuch", 6) == PFS_ENOENT);
    make(1, PFS_FILE_MAX);
    int n;
    for (n = 0; n < 10; ++n) {
        char nm[3] = { 'b', (char)('0' + n), 0 };
        if (pfs_write(&fs, nm, 2, data, PFS_FILE_MAX)) break;
    }
    CHECK(n == 7);                                  /* 253 data blocks, 1 used: 7 x 32 */
    CHECK(pfs_write(&fs, "b9", 2, data, PFS_FILE_MAX) == PFS_ENOSPC);
    /* a replace needs room for the new copy while the old one exists */
    CHECK(pfs_write(&fs, "b0", 2, data, PFS_FILE_MAX) == PFS_ENOSPC);
    CHECK(pfs_write(&fs, "b0", 2, data, 100) == 0);
    CHECK(pfs_read(&fs, (uint32_t)pfs_find(&fs, "b0", 2), rbuf) == 100);
    for (n = 0; n < 7; ++n) { char nm[3] = { 'b', (char)('0' + n), 0 }; CHECK(pfs_delete(&fs, nm, 2) == 0); }
    CHECK(fs.files == 1);
    for (n = 0; n < 70; ++n) {
        char nm[4] = { 'z', (char)('0' + n / 10), (char)('0' + n % 10), 0 };
        if (pfs_write(&fs, nm, 3, data, 0)) break;
    }
    CHECK(n == 62 && fs.files == PFS_FILES);        /* 63 files in all */
    CHECK(pfs_nth(&fs, 62) && !pfs_nth(&fs, 63));
    /* read-only device */
    sim.readonly = 1;
    CHECK(pfs_write(&fs, "q", 1, data, 1) == PFS_EROFS && pfs_delete(&fs, "z00", 3) == PFS_EROFS);
    sim.readonly = 0;
    /* CRC damage of durable data: detected on read, not repaired */
    reset_disk();
    CHECK(pfs_mount(&fs, &sim) == 0);
    make(7, 9000);
    CHECK(pfs_write(&fs, "dmg", 3, data, 9000) == 0);
    int i = pfs_find(&fs, "dmg", 3);
    durable[(size_t)(fs.ent[i].first + 2) * PFS_BLOCK + 100] ^= 1;
    CHECK(pfs_read(&fs, (uint32_t)i, rbuf) == PFS_EIO && !fs.failed);
    /* equal seq: copy A wins; only B valid: B */
    reset_disk();
    CHECK(pfs_mount(&fs, &sim) == 0 && fs.slot == 1);
    put64(durable + 2 * PFS_BLOCK + 8, 1);
    fix_crc(durable + 2 * PFS_BLOCK, 16);
    CHECK(pfs_mount(&fs, &sim) == 0 && fs.slot == 1);
    durable[PFS_BLOCK + 30] ^= 1;
    CHECK(pfs_mount(&fs, &sim) == 0 && fs.slot == 2);
    /* seq at its maximum: no more changes (seq + 1 would wrap to 0 and
     * lose to the old copy after a reboot) */
    reset_disk();
    put64(durable + PFS_BLOCK + 8, UINT64_MAX);
    fix_crc(durable + PFS_BLOCK, 16);
    CHECK(pfs_mount(&fs, &sim) == 0 && fs.slot == 1 && fs.seq == UINT64_MAX);
    CHECK(pfs_write(&fs, "q", 1, data, 1) == PFS_ENOSPC && pfs_delete(&fs, "q", 1) == PFS_ENOSPC);
    CHECK(calls == 0);
    printf("api: ok\n");
}

/* ------------------------------------------------------ directory fuzzing */
static void fuzz(const char *dump)
{
    static struct pfs fs;
    static uint8_t base[BLOCKS * PFS_BLOCK];
    reset_disk();
    CHECK(pfs_mount(&fs, &sim) == 0);
    for (int k = 0; k < 5; ++k) {
        make((uint32_t)k, 1000u * (uint32_t)k + 1);
        CHECK(pfs_write(&fs, names[k], strlen(names[k]), data, 1000u * (uint32_t)k + 1) == 0);
    }
    memcpy(base, durable, sizeof(base));
    FILE *man = 0;
    if (dump) {
        char p[512];
        snprintf(p, sizeof(p), "%s/manifest.txt", dump);
        man = fopen(p, "w");
        CHECK(man);
    }
    long rejected = 0;
    for (int k = 0; k < 4000; ++k) {
        memcpy(durable, base, sizeof(base));
        unsigned blk = (unsigned)(rnd() % 3), edits = 1 + (unsigned)(rnd() % 4);
        for (unsigned e = 0; e < edits; ++e) {
            unsigned off = (unsigned)(rnd() % (blk ? 512 : 32));
            if (rnd() % 4 == 0) off = (unsigned)(rnd() % PFS_BLOCK);
            durable[blk * PFS_BLOCK + off] = (uint8_t)rnd();
        }
        if (rnd() % 2) fix_crc(durable + blk * PFS_BLOCK, blk ? 16 : 12);  /* past the CRC */
        sim.blocks = rnd() % 8 == 0 ? (uint32_t)(rnd() % BLOCKS) : BLOCKS;
        int r = pfs_mount(&fs, &sim);
        if (r) rejected++;
        else
            for (unsigned i = 0; i < PFS_FILES; ++i)
                if (fs.ent[i].name[0]) (void)pfs_read(&fs, i, rbuf);
        if (man) {
            char p[512];
            snprintf(p, sizeof(p), "%s/f%04d.img", dump, k);
            FILE *f = fopen(p, "wb");
            CHECK(f && fwrite(durable, PFS_BLOCK, 3, f) == 3);
            fclose(f);
            fprintf(man, "f%04d.img %u %d", k, sim.blocks, r);
            if (!r) {
                fprintf(man, " %u %llu", fs.slot, (unsigned long long)fs.seq);
                for (unsigned i = 0; i < PFS_FILES; ++i)
                    if (fs.ent[i].name[0])
                        fprintf(man, " %s:%u:%u:%u:%u", fs.ent[i].name, fs.ent[i].size,
                                fs.ent[i].first, fs.ent[i].count, fs.ent[i].crc);
            }
            fprintf(man, "\n");
        }
    }
    sim.blocks = BLOCKS;
    if (man) fclose(man);
    printf("fuzz: 4000 mutated directories/superblocks, %ld rejected, no crash\n", rejected);
}

int main(int argc, char **argv)
{
    for (int i = 0; i < FILLERS; ++i) {
        filler_name[i][0] = 'p';
        filler_name[i][1] = (char)('0' + i / 10);
        filler_name[i][2] = (char)('0' + i % 10);
        names[SCRIPT_NAMES + i] = filler_name[i];
    }
    api_tests();
    make_script();
    long total = reference();
    for (long k = 0; k < total; ++k) stop_test(k, 0);
    printf("stops: %ld device calls, %ld stop points, %ld mounts checked\n", total, variants, mounts);
    long m0 = mounts;
    for (long k = 0; k < total; ++k) stop_test(k, 1);
    printf("failures: %ld failed writes/flushes, %ld mounts checked\n", total, mounts - m0);
    fuzz(argc > 1 ? argv[1] : 0);
    printf("PFS_TESTS_PASS\n");
    return 0;
}
