/* v1.5 PFS: see pfs.h and docs/SPEC_V1_5.md. No allocation; the state
 * lives in the caller's struct pfs, plus two static directory tables used
 * only inside one call (one operation at a time). */
#include "pfs.h"

#define SB_MAGIC   0x31534650u          /* "PFS1" */
#define DIR_HEADER 64u
#define ENTRY_SIZE 64u
_Static_assert(DIR_HEADER + PFS_FILES * ENTRY_SIZE == PFS_BLOCK, "directory fills one block");

static const uint8_t dir_magic[8] = { 'P', 'F', 'S', 'D', 'I', 'R', '0', '1' };

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void fill(uint8_t *p, uint8_t v, uint64_t n)
{
    for (uint64_t i = 0; i < n; ++i) p[i] = v;
}

static int all_zero(const uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; ++i)
        if (p[i]) return 0;
    return 1;
}

/* CRC-32 (IEEE 802.3, reflected 0xEDB88320, as zlib.crc32), 4 bits a step.
 * Start with crc = 0; it can be continued over several pieces. */
uint32_t pfs_crc32(uint32_t crc, const uint8_t *p, uint64_t n)
{
    static const uint32_t nib[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u,
        0x4db26158u, 0x5005713cu, 0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu };
    crc = ~crc;
    for (uint64_t i = 0; i < n; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ nib[crc & 15u];
        crc = (crc >> 4) ^ nib[crc & 15u];
    }
    return ~crc;
}

/* CRC of a block whose 4-byte CRC field at `at` counts as zero. */
static uint32_t block_crc(const uint8_t *b, unsigned at)
{
    static const uint8_t zero[4];
    uint32_t c = pfs_crc32(0, b, at);
    c = pfs_crc32(c, zero, 4);
    return pfs_crc32(c, b + at + 4, PFS_BLOCK - at - 4);
}

static int name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

int pfs_name_ok(const char *name, uint64_t len)
{
    if (len == 0 || len > PFS_NAME_MAX) return 0;
    for (uint64_t i = 0; i < len; ++i)
        if (!name_char(name[i])) return 0;
    return 1;
}

static int same_name(const char *a, const char *b, uint64_t len)
{
    for (uint64_t i = 0; i < len; ++i)
        if (a[i] != b[i]) return 0;
    return a[len] == 0;
}

/* Two NUL-padded 16-byte name fields are the same name. */
static int same16(const char *a, const char *b)
{
    for (unsigned i = 0; i < 16; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* Parse and validate directory block b: 0 and ent[] filled, or a reject code. */
static int parse_dir(const uint8_t *b, uint32_t blocks, uint64_t *seq, struct pfs_entry *ent)
{
    for (unsigned i = 0; i < 8; ++i)
        if (b[i] != dir_magic[i]) return PFS_R_DIR_MAGIC;
    if (get32(b + 16) != block_crc(b, 16)) return PFS_R_DIR_CRC;
    if (!all_zero(b + 20, DIR_HEADER - 20)) return PFS_R_DIR_RESERVED;
    *seq = get64(b + 8);
    for (unsigned i = 0; i < PFS_FILES; ++i) {
        const uint8_t *e = b + DIR_HEADER + ENTRY_SIZE * i;
        struct pfs_entry *x = &ent[i];
        fill((uint8_t *)x, 0, sizeof(*x));
        if (e[0] == 0) {
            if (!all_zero(e, ENTRY_SIZE)) return PFS_R_EMPTY;
            continue;
        }
        unsigned n = 0;
        while (n < 16 && e[n]) ++n;
        if (n > PFS_NAME_MAX || !pfs_name_ok((const char *)e, n) || !all_zero(e + n, 16 - n))
            return PFS_R_NAME;
        if (!all_zero(e + 32, ENTRY_SIZE - 32)) return PFS_R_DIR_RESERVED;
        for (unsigned k = 0; k < n; ++k) x->name[k] = (char)e[k];
        x->size = get32(e + 16);
        x->first = get32(e + 20);
        x->count = get32(e + 24);
        x->crc = get32(e + 28);
        if (x->size > PFS_FILE_MAX || x->count != (x->size + PFS_BLOCK - 1) / PFS_BLOCK)
            return PFS_R_SIZE;
        if (x->count == 0 ? x->first != 0
                          : (x->first < PFS_DATA_START || x->first > blocks - x->count))
            return PFS_R_RANGE;
    }
    for (unsigned i = 0; i < PFS_FILES; ++i) {
        if (!ent[i].name[0]) continue;
        for (unsigned j = i + 1; j < PFS_FILES; ++j) {
            if (!ent[j].name[0]) continue;
            if (same16(ent[i].name, ent[j].name)) return PFS_R_DUP;
            if (ent[i].count && ent[j].count && ent[i].first < ent[j].first + ent[j].count &&
                ent[j].first < ent[i].first + ent[i].count)
                return PFS_R_OVERLAP;
        }
    }
    return 0;
}

int pfs_mount(struct pfs *fs, const struct pfs_dev *dev)
{
    fill((uint8_t *)fs, 0, sizeof(*fs));
    fs->dev = dev;
    uint8_t *b = fs->blk;
    if (dev->read(dev->ctx, 0, b)) return PFS_R_SB_IO;
    if (get32(b) != SB_MAGIC) return PFS_R_SB_MAGIC;
    if (get32(b + 4) != PFS_VERSION) return PFS_R_SB_VERSION;
    if (get32(b + 12) != block_crc(b, 12)) return PFS_R_SB_CRC;
    if (!all_zero(b + 16, PFS_BLOCK - 16)) return PFS_R_SB_RESERVED;
    uint32_t blocks = get32(b + 8);
    if (blocks < PFS_MIN_BLOCKS || blocks > PFS_MAX_BLOCKS) return PFS_R_SB_RANGE;
    if (blocks > dev->blocks) return PFS_R_SB_DEVICE;
    fs->blocks = blocks;
    /* Copy A straight into ent[]; copy B is checked with the same parser
     * into a second table only if it might win. */
    uint64_t seq_a = 0, seq_b = 0;
    static struct pfs_entry tmp[PFS_FILES];     /* one mount at a time */
    int ra = dev->read(dev->ctx, 1, b) ? PFS_R_DIR_IO : parse_dir(b, blocks, &seq_a, fs->ent);
    int rb = dev->read(dev->ctx, 2, b) ? PFS_R_DIR_IO : parse_dir(b, blocks, &seq_b, tmp);
    fs->reject_a = (uint32_t)ra;
    fs->reject_b = (uint32_t)rb;
    if (ra && rb) return ra;
    /* B only if it is valid and (A is not, or B is newer); equal seq: A. */
    if (ra || (!rb && seq_b > seq_a)) {
        for (unsigned i = 0; i < PFS_FILES; ++i) fs->ent[i] = tmp[i];
        fs->slot = 2;
        fs->seq = seq_b;
    } else {
        fs->slot = 1;
        fs->seq = seq_a;
    }
    for (unsigned i = 0; i < PFS_FILES; ++i)
        if (fs->ent[i].name[0]) fs->files++;
    return 0;
}

const struct pfs_entry *pfs_nth(const struct pfs *fs, uint32_t index)
{
    for (unsigned i = 0; i < PFS_FILES; ++i)
        if (fs->ent[i].name[0] && index-- == 0) return &fs->ent[i];
    return 0;
}

int pfs_find(const struct pfs *fs, const char *name, uint64_t len)
{
    if (!pfs_name_ok(name, len)) return PFS_ENOENT;
    for (unsigned i = 0; i < PFS_FILES; ++i)
        if (fs->ent[i].name[0] && same_name(fs->ent[i].name, name, len)) return (int)i;
    return PFS_ENOENT;
}

static int failed(struct pfs *fs)
{
    fs->failed = 1;
    return PFS_EIO;
}

int pfs_read(struct pfs *fs, uint32_t i, uint8_t *out)
{
    if (fs->failed) return PFS_EIO;
    if (i >= PFS_FILES || !fs->ent[i].name[0]) return PFS_ENOENT;
    const struct pfs_entry *e = &fs->ent[i];
    for (uint32_t k = 0; k < e->count; ++k)
        if (fs->dev->read(fs->dev->ctx, e->first + k, out + (uint64_t)k * PFS_BLOCK))
            return failed(fs);
    /* Durable data that rotted: detected, reported, not repaired. */
    if (pfs_crc32(0, out, e->size) != e->crc) return PFS_EIO;
    return (int)e->size;
}

/* First fit: the lowest start of `need` blocks free in the mounted
 * directory (old versions of files included: they stay intact until the
 * new directory is durable), or 0 if there is none. */
static uint32_t find_free(const struct pfs *fs, uint32_t need)
{
    uint32_t start = PFS_DATA_START;
    for (;;) {
        if (start > fs->blocks || need > fs->blocks - start) return 0;
        int moved = 0;
        for (unsigned i = 0; i < PFS_FILES; ++i) {
            const struct pfs_entry *e = &fs->ent[i];
            if (!e->name[0] || !e->count) continue;
            if (e->first < start + need && start < e->first + e->count) {
                start = e->first + e->count;
                moved = 1;
            }
        }
        if (!moved) return start;
    }
}

/* Write directory ent[] with seq into the copy that is not mounted; flush. */
static int write_dir(struct pfs *fs, const struct pfs_entry *ent, uint64_t seq)
{
    uint8_t *b = fs->blk;
    fill(b, 0, PFS_BLOCK);
    for (unsigned i = 0; i < 8; ++i) b[i] = dir_magic[i];
    put64(b + 8, seq);
    for (unsigned i = 0; i < PFS_FILES; ++i) {
        const struct pfs_entry *x = &ent[i];
        if (!x->name[0]) continue;
        uint8_t *e = b + DIR_HEADER + ENTRY_SIZE * i;
        for (unsigned k = 0; k < 16; ++k) e[k] = (uint8_t)x->name[k];
        put32(e + 16, x->size);
        put32(e + 20, x->first);
        put32(e + 24, x->count);
        put32(e + 28, x->crc);
    }
    put32(b + 16, block_crc(b, 16));
    if (fs->dev->write(fs->dev->ctx, 3 - fs->slot, b) || fs->dev->flush(fs->dev->ctx))
        return failed(fs);
    return 0;
}

/* The new directory is durable: it is now the mounted one. */
static void commit(struct pfs *fs, const struct pfs_entry *ent)
{
    fs->files = 0;
    for (unsigned i = 0; i < PFS_FILES; ++i) {
        fs->ent[i] = ent[i];
        if (ent[i].name[0]) fs->files++;
    }
    fs->seq++;
    fs->slot = 3 - fs->slot;
}

int pfs_write(struct pfs *fs, const char *name, uint64_t len, const uint8_t *data, uint64_t size)
{
    if (!pfs_name_ok(name, len)) return PFS_EINVAL;
    if (size > PFS_FILE_MAX) return PFS_EFBIG;
    if (fs->failed) return PFS_EIO;
    if (fs->dev->readonly) return PFS_EROFS;
    if (fs->seq == UINT64_MAX) return PFS_ENOSPC;   /* seq + 1 would lose to the old copy */
    int idx = pfs_find(fs, name, len);
    if (idx < 0) {
        for (unsigned i = 0; i < PFS_FILES && idx < 0; ++i)
            if (!fs->ent[i].name[0]) idx = (int)i;
        if (idx < 0) return PFS_ENOSPC;         /* 63 files */
    }
    uint32_t need = (uint32_t)((size + PFS_BLOCK - 1) / PFS_BLOCK);
    uint32_t first = need ? find_free(fs, need) : 0;
    if (need && !first) return PFS_ENOSPC;
    uint8_t *b = fs->blk;
    for (uint32_t k = 0; k < need; ++k) {
        uint64_t off = (uint64_t)k * PFS_BLOCK, n = size - off < PFS_BLOCK ? size - off : PFS_BLOCK;
        for (uint64_t i = 0; i < n; ++i) b[i] = data[off + i];
        fill(b + n, 0, PFS_BLOCK - n);
        if (fs->dev->write(fs->dev->ctx, first + k, b)) return failed(fs);
    }
    /* The data is durable before any directory points at it. */
    if (need && fs->dev->flush(fs->dev->ctx)) return failed(fs);
    static struct pfs_entry next[PFS_FILES];    /* one operation at a time */
    for (unsigned i = 0; i < PFS_FILES; ++i) next[i] = fs->ent[i];
    struct pfs_entry *x = &next[idx];
    fill((uint8_t *)x, 0, sizeof(*x));
    for (uint64_t k = 0; k < len; ++k) x->name[k] = name[k];
    x->size = (uint32_t)size;
    x->first = first;
    x->count = need;
    x->crc = pfs_crc32(0, data, size);
    int err = write_dir(fs, next, fs->seq + 1);
    if (err) return err;
    commit(fs, next);
    return 0;
}

int pfs_delete(struct pfs *fs, const char *name, uint64_t len)
{
    if (!pfs_name_ok(name, len)) return PFS_EINVAL;
    if (fs->failed) return PFS_EIO;
    if (fs->dev->readonly) return PFS_EROFS;
    if (fs->seq == UINT64_MAX) return PFS_ENOSPC;
    int idx = pfs_find(fs, name, len);
    if (idx < 0) return PFS_ENOENT;
    static struct pfs_entry next[PFS_FILES];
    for (unsigned i = 0; i < PFS_FILES; ++i) next[i] = fs->ent[i];
    fill((uint8_t *)&next[idx], 0, sizeof(next[idx]));
    int err = write_dir(fs, next, fs->seq + 1);
    if (err) return err;
    commit(fs, next);
    return 0;
}
