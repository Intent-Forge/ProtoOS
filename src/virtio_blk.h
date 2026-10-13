#ifndef PROTOOS_VIRTIO_BLK_H
#define PROTOOS_VIRTIO_BLK_H
#include <stdint.h>

/* v1.5 virtio-blk over virtio-mmio version 2 (virtio 1.x), polled, one
 * request at a time (docs/SPEC_V1_5.md). Descriptors only ever point at
 * the driver's own kernel buffers: a 4 KiB bounce buffer, the request
 * header and the status byte. */

enum vblk_state {
    VBLK_NONE = 0,      /* no virtio block device */
    VBLK_READY = 1,
    VBLK_FAILED = 2,    /* init failed, timeout, device error: EIO until reboot */
    VBLK_LEGACY = 3,    /* a legacy (version 1) block device: refused */
};
/* Why the device failed (vblk.reason). */
enum vblk_reason {
    VBLK_R_RESET = 1,       /* status did not read 0 after reset */
    VBLK_R_NO_V1 = 2,       /* VIRTIO_F_VERSION_1 not offered */
    VBLK_R_FEATURES = 3,    /* FEATURES_OK not kept by the device */
    VBLK_R_QUEUE = 4,       /* queue 0 busy or smaller than 3 */
    VBLK_R_CAPACITY = 5,    /* capacity unstable or below 8 sectors */
    VBLK_R_TIMEOUT = 6,     /* a request did not complete in time */
    VBLK_R_STATUS = 7,      /* the device reported an error for a request */
    VBLK_R_NEEDS_RESET = 8, /* the device set NEEDS_RESET or FAILED */
};

#define VBLK_QSIZE 8u
#define VIRTIO_F_VERSION_1  (UINT64_C(1) << 32)
#define VIRTIO_BLK_F_RO     (UINT64_C(1) << 5)
#define VIRTIO_BLK_F_FLUSH  (UINT64_C(1) << 9)

struct vblk {
    uint64_t base;          /* transport in use (0: none) */
    uint64_t state, reason;
    uint64_t device_features, features;   /* offered, negotiated */
    uint64_t capacity;      /* 512-byte sectors */
    uint64_t qsize, avail_idx, used_seen;
    uint64_t reads, writes, flushes;      /* completed requests */
    uint64_t legacy_base;   /* a refused legacy block device */
};
extern struct vblk vblk;

/* Probe the DT transports: use the first virtio-mmio v2 block device,
 * refuse a legacy one. Returns vblk.state. */
uint64_t vblk_probe(const uint64_t *bases, unsigned n);
/* 4 KiB block I/O (8 sectors) and flush: 0 or -5 (EIO). */
int vblk_read(uint32_t block, uint8_t *buf);
int vblk_write(uint32_t block, const uint8_t *buf);
int vblk_flush(void);
#endif
