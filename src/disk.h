#ifndef PROTOOS_DISK_H
#define PROTOOS_DISK_H
#include "pfs.h"

/* v1.5: the disk as the kernel uses it: the virtio-blk driver under PFS.
 * disk.mounted == 0 (no disk, refused, failed or rejected): fs_* give ENODEV. */
struct disk_state {
    uint64_t mounted;
    uint64_t reject;        /* PFS mount refusal code (0: none) */
    uint64_t ops, errors;   /* fs syscalls that touched the disk; ones that failed */
};
extern struct disk_state disk;
extern struct pfs disk_fs;
/* One file at a time: fs_read/fs_write data and programs loaded from the
 * disk pass through this kernel buffer (contiguous, identity mapped). */
extern uint8_t disk_buf[PFS_FILE_MAX];

/* Find, start and mount the disk; boot lines as in docs/SPEC_V1_5.md.
 * Without a virtio block device nothing is printed (v1.4 behaviour). */
void disk_init(void);
#endif
