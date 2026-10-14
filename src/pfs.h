#ifndef PROTOOS_PFS_H
#define PROTOOS_PFS_H
#include <stdint.h>

/* v1.5 PFS, the flat ProtoOS file system (docs/SPEC_V1_5.md). Pure code
 * over a block interface: the kernel gives it the virtio-blk driver, the
 * host tests give it a simulated disk with faults.
 *
 * Disk layout, 4 KiB blocks, all integers little-endian:
 *   block 0   superblock: "PFS1" magic, version 1, block count, CRC32
 *   block 1   directory copy A  } each: "PFSDIR01", seq, CRC32 and 63
 *   block 2   directory copy B  } entries of 64 bytes
 *   block 3.. file data; every file is one contiguous extent
 * A file is replaced whole: its data goes to blocks free in the mounted
 * directory, flush, the new directory goes into the other copy with
 * seq + 1, flush. Mount uses the valid copy with the highest seq. */

#define PFS_BLOCK        4096u
#define PFS_FILES        63u
#define PFS_NAME_MAX     15u
#define PFS_FILE_MAX     (128u * 1024u)
#define PFS_FILE_BLOCKS  (PFS_FILE_MAX / PFS_BLOCK)
#define PFS_DATA_START   3u
#define PFS_MIN_BLOCKS   256u            /* 1 MiB */
#define PFS_MAX_BLOCKS   16384u          /* 64 MiB */
#define PFS_VERSION      1u

/* Errors returned by the operations (the syscall error numbers). */
#define PFS_ENOENT  (-2)
#define PFS_EIO     (-5)
#define PFS_EINVAL  (-22)
#define PFS_EFBIG   (-27)
#define PFS_ENOSPC  (-28)
#define PFS_EROFS   (-30)

/* Mount refusal codes: "DISK rejected code=-N" (N13 checks each one). For a
 * directory defect the code is that of copy A when neither copy is valid. */
enum pfs_reject {
    PFS_R_SB_IO = 1,        /* superblock could not be read */
    PFS_R_SB_MAGIC = 2,
    PFS_R_SB_VERSION = 3,
    PFS_R_SB_CRC = 4,
    PFS_R_SB_RANGE = 5,     /* block count outside 256..16384 */
    PFS_R_SB_DEVICE = 6,    /* block count larger than the device */
    PFS_R_SB_RESERVED = 7,  /* nonzero reserved bytes */
    PFS_R_DIR_IO = 10,
    PFS_R_DIR_MAGIC = 11,
    PFS_R_DIR_CRC = 12,
    PFS_R_DIR_RESERVED = 13,    /* nonzero header or entry reserved bytes */
    PFS_R_NAME = 14,            /* bad name, or a used entry with padding garbage */
    PFS_R_DUP = 15,
    PFS_R_RANGE = 16,           /* extent outside the data blocks */
    PFS_R_SIZE = 17,            /* size > 128 KiB or block count != ceil(size / 4096) */
    PFS_R_OVERLAP = 18,
    PFS_R_EMPTY = 19,           /* an unused entry that is not all zero */
};

/* Block device: 4 KiB blocks; every call returns 0 or a negative error.
 * flush() makes every completed write durable. */
struct pfs_dev {
    void *ctx;
    int (*read)(void *ctx, uint32_t block, uint8_t *buf);
    int (*write)(void *ctx, uint32_t block, const uint8_t *buf);
    int (*flush)(void *ctx);
    uint32_t blocks;        /* device capacity in 4 KiB blocks */
    int readonly;           /* no flush support or read-only: writes give EROFS */
};

struct pfs_entry {
    char name[PFS_NAME_MAX + 1];    /* NUL padded; name[0] == 0: unused */
    uint32_t size, first, count, crc;
};

struct pfs {
    const struct pfs_dev *dev;
    uint32_t blocks;                /* from the superblock */
    uint32_t slot;                  /* block (1 or 2) of the mounted copy */
    uint64_t seq;
    uint32_t files;                 /* used entries */
    int failed;                     /* an I/O error happened: EIO until reboot */
    uint32_t reject_a, reject_b;    /* why a copy was not used (0: valid) */
    struct pfs_entry ent[PFS_FILES];
    uint8_t blk[PFS_BLOCK];         /* scratch block */
};

uint32_t pfs_crc32(uint32_t crc, const uint8_t *p, uint64_t n);
/* Validate the superblock and both directory copies, mount the newest
 * valid copy. 0 or a positive enum pfs_reject code. */
int pfs_mount(struct pfs *fs, const struct pfs_dev *dev);
/* 1 if name[0..len) is a valid file name (1-15 of [a-z0-9_.]). */
int pfs_name_ok(const char *name, uint64_t len);
/* Entry of the index-th used file in directory order, or 0. */
const struct pfs_entry *pfs_nth(const struct pfs *fs, uint32_t index);
/* Index into ent[] of file `name`, or PFS_ENOENT. */
int pfs_find(const struct pfs *fs, const char *name, uint64_t len);
/* Read file ent[i] whole into out (PFS_FILE_MAX bytes) and check its CRC
 * over all of it: the size, or PFS_EIO. */
int pfs_read(struct pfs *fs, uint32_t i, uint8_t *out);
/* Create or replace file `name` with data[0..size): 0 when the new
 * directory is durable, else an error (nothing reported done). */
int pfs_write(struct pfs *fs, const char *name, uint64_t len, const uint8_t *data, uint64_t size);
int pfs_delete(struct pfs *fs, const char *name, uint64_t len);
#endif
