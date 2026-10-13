#include "archive.h"

/* cpio newc header: magic[6] then 13 fields of 8 hex digits. */
#define F_MODE     1u
#define F_FILESIZE 6u
#define F_NAMESIZE 11u
#define F_CHECK    12u
#define MODE_TYPE  UINT64_C(0170000)
#define MODE_FILE  UINT64_C(0100000)

static int hex8(const uint8_t *p, uint64_t *out)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; ++i) {
        uint8_t c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10u;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10u;
        else return 0;
        v = v << 4 | d;
    }
    *out = v;
    return 1;
}

static uint64_t align4(uint64_t v)
{
    return (v + 3u) & ~UINT64_C(3);
}

static int name_char(uint8_t c, int first)
{
    if (c >= 'a' && c <= 'z') return 1;
    if (first) return 0;
    return (c >= '0' && c <= '9') || c == '_';
}

static int same(const char *a, const char *b, uint64_t len)
{
    for (uint64_t i = 0; i < len; ++i)
        if (a[i] != b[i]) return 0;
    return a[len] == 0 && b[len] == 0;
}

static int trailer(const uint8_t *name, uint64_t namesize)
{
    static const char t[] = "TRAILER!!!";
    if (namesize != sizeof(t)) return 0;
    for (unsigned i = 0; i < sizeof(t); ++i)
        if (name[i] != (uint8_t)t[i]) return 0;
    return 1;
}

int archive_parse(const uint8_t *img, uint64_t size, struct archive *out)
{
    out->count = 0;
    if (size == 0 || size > ARCHIVE_MAX_BYTES) return ARCHIVE_E_SIZE;
    uint64_t pos = 0, count = 0;
    for (;;) {
        /* pos is 4-aligned and < size + 4 here; every range below is
         * checked against what remains, so no sum can overflow. */
        if (pos > size || size - pos < 6u) return ARCHIVE_E_TRAILER;
        static const char magic[] = "070701";
        for (unsigned i = 0; i < 6u; ++i)
            if (img[pos + i] != (uint8_t)magic[i]) return ARCHIVE_E_MAGIC;
        if (size - pos < ARCHIVE_HEADER) return ARCHIVE_E_HEADER;
        uint64_t f[13];
        for (unsigned i = 0; i < 13u; ++i)
            if (!hex8(img + pos + 6u + 8u * i, &f[i])) return ARCHIVE_E_HEADER;
        uint64_t namesize = f[F_NAMESIZE], filesize = f[F_FILESIZE];
        const uint8_t *name = img + pos + ARCHIVE_HEADER;
        if (namesize < 2u || namesize > size - pos - ARCHIVE_HEADER)
            return ARCHIVE_E_NAME;
        if (name[namesize - 1u] != 0) return ARCHIVE_E_NAME;
        uint64_t data = align4(pos + ARCHIVE_HEADER + namesize);
        if (f[F_CHECK] != 0) return ARCHIVE_E_TYPE;
        if (trailer(name, namesize)) {
            if (filesize != 0) return ARCHIVE_E_DATA;
            for (uint64_t i = pos + ARCHIVE_HEADER + namesize; i < size; ++i)
                if (img[i] != 0) return ARCHIVE_E_TAIL;
            out->count = count;
            return ARCHIVE_OK;
        }
        if (namesize - 1u > ARCHIVE_NAME_MAX) return ARCHIVE_E_NAME;
        for (uint64_t i = 0; i + 1u < namesize; ++i)
            if (!name_char(name[i], i == 0)) return ARCHIVE_E_NAME;
        if ((f[F_MODE] & MODE_TYPE) != MODE_FILE) return ARCHIVE_E_TYPE;
        if (data > size || filesize > size - data) return ARCHIVE_E_DATA;
        if (count == ARCHIVE_MAX_FILES) return ARCHIVE_E_COUNT;
        struct archive_file *file = &out->files[count];
        for (uint64_t i = 0; i < namesize; ++i) file->name[i] = (char)name[i];
        for (uint64_t i = 0; i < count; ++i)
            if (same(out->files[i].name, file->name, namesize - 1u)) return ARCHIVE_E_DUP;
        file->offset = data;
        file->size = filesize;
        count++;
        pos = align4(data + filesize);
    }
}

int archive_find(const struct archive *a, const char *name, uint64_t len)
{
    if (len == 0 || len > ARCHIVE_NAME_MAX) return -1;
    for (uint64_t i = 0; i < a->count; ++i) {
        const char *n = a->files[i].name;
        uint64_t k = 0;
        while (k < len && n[k] && n[k] == name[k]) ++k;
        if (k == len && n[k] == 0) return (int)i;
    }
    return -1;
}

int archive_range_ok(uint64_t start, uint64_t end, uint64_t lo, uint64_t hi)
{
    /* Containment only; the size limit is archive_parse's (audit: an oversized
     * archive inside the pool is reserved, then rejected with ARCHIVE_E_SIZE). */
    return start < end && start >= lo && end <= hi;
}
