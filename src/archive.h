#ifndef PROTOOS_ARCHIVE_H
#define PROTOOS_ARCHIVE_H
#include <stdint.h>

/* v1.4 program archive: the boot loader's initrd (QEMU -initrd) holding a
 * cpio "newc" archive (SVR4 portable format without CRC, magic 070701),
 * as written by build-scripts/mkarchive.py or GNU cpio -H newc. The whole
 * archive is validated once at boot; a rejected archive provides no
 * programs at all. Pure functions: no kernel state, host-testable. */
#define ARCHIVE_MAX_FILES 32u
#define ARCHIVE_NAME_MAX  15u                     /* characters, NUL excluded */
#define ARCHIVE_MAX_BYTES (UINT64_C(4) << 20)
#define ARCHIVE_HEADER    110u

struct archive_file {
    char name[ARCHIVE_NAME_MAX + 1];
    uint64_t offset;            /* of the file data, from the archive start */
    uint64_t size;
};
struct archive {
    uint64_t count;
    struct archive_file files[ARCHIVE_MAX_FILES];
};

enum archive_result {
    ARCHIVE_OK = 0,
    ARCHIVE_E_SIZE = -1,        /* empty or larger than ARCHIVE_MAX_BYTES */
    ARCHIVE_E_MAGIC = -2,       /* not 070701 */
    ARCHIVE_E_HEADER = -3,      /* truncated header or a field that is not 8 hex digits */
    ARCHIVE_E_NAME = -4,        /* name length, terminator or characters */
    ARCHIVE_E_TYPE = -5,        /* not a regular file, or check field not 0 */
    ARCHIVE_E_DATA = -6,        /* file data runs past the end */
    ARCHIVE_E_COUNT = -7,       /* more than ARCHIVE_MAX_FILES files */
    ARCHIVE_E_DUP = -8,         /* two files with the same name */
    ARCHIVE_E_TRAILER = -9,     /* no TRAILER!!! entry */
    ARCHIVE_E_TAIL = -10,       /* non-zero bytes after the trailer */
};

/* Validate img[0..size) completely and index its files (count may be 0).
 * File names: 1..15 characters [a-z0-9_], starting with a letter.
 * On error *out is left with count = 0. */
int archive_parse(const uint8_t *img, uint64_t size, struct archive *out);
/* Index of the file whose name is exactly name[0..len), or -1. */
int archive_find(const struct archive *a, const char *name, uint64_t len);
/* 1 if [start, end) is a non-empty range of at most ARCHIVE_MAX_BYTES that
 * lies inside [lo, hi). */
int archive_range_ok(uint64_t start, uint64_t end, uint64_t lo, uint64_t hi);
#endif
