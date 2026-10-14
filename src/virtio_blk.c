/* v1.5 virtio-blk driver: virtio-mmio version 2, polled, bounded waits.
 * Register offsets and bits: Virtual I/O Device (VIRTIO) 1.2, 4.2.2
 * (MMIO), 5.2 (block device), 2.7 (split virtqueues). */
#include "kernel.h"
#include "virtio_blk.h"

#define MMIO_MAGIC          0x000
#define MMIO_VERSION        0x004
#define MMIO_DEVICE_ID      0x008
#define MMIO_DEV_FEATURES   0x010
#define MMIO_DEV_FEAT_SEL   0x014
#define MMIO_DRV_FEATURES   0x020
#define MMIO_DRV_FEAT_SEL   0x024
#define MMIO_QUEUE_SEL      0x030
#define MMIO_QUEUE_NUM_MAX  0x034
#define MMIO_QUEUE_NUM      0x038
#define MMIO_QUEUE_READY    0x044
#define MMIO_QUEUE_NOTIFY   0x050
#define MMIO_INT_STATUS     0x060
#define MMIO_INT_ACK        0x064
#define MMIO_STATUS         0x070
#define MMIO_QUEUE_DESC     0x080
#define MMIO_QUEUE_DRIVER   0x090
#define MMIO_QUEUE_DEVICE   0x0a0
#define MMIO_CONFIG_GEN     0x0fc
#define MMIO_CONFIG         0x100   /* virtio_blk_config: capacity first */

#define VIRTIO_MAGIC        0x74726976u     /* "virt" */
#define VIRTIO_ID_BLOCK     2u
#define ST_ACK              1u
#define ST_DRIVER           2u
#define ST_DRIVER_OK        4u
#define ST_FEATURES_OK      8u
#define ST_NEEDS_RESET      64u
#define ST_FAILED           128u

#define DESC_F_NEXT         1u
#define DESC_F_WRITE        2u
#define REQ_IN              0u
#define REQ_OUT             1u
#define REQ_FLUSH           4u

struct vq_desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct vq_avail { uint16_t flags, idx, ring[VBLK_QSIZE], used_event; };
struct vq_used_elem { uint32_t id, len; };
struct vq_used { uint16_t flags, idx; struct vq_used_elem ring[VBLK_QSIZE]; uint16_t avail_event; };
struct vblk_req { uint32_t type, reserved; uint64_t sector; };

/* One page for the split virtqueue: descriptors, driver (avail) and
 * device (used) areas, each at its required alignment. The kernel image is
 * identity mapped, so these virtual addresses are the bus addresses. */
static struct {
    struct vq_desc desc[VBLK_QSIZE];
    uint8_t pad0[256 - sizeof(struct vq_desc) * VBLK_QSIZE];
    struct vq_avail avail;
    uint8_t pad1[256 - sizeof(struct vq_avail)];
    struct vq_used used;
} __attribute__((aligned(4096))) vq;
_Static_assert(sizeof(struct vq_desc) * VBLK_QSIZE <= 256 && sizeof(struct vq_avail) <= 256,
               "virtqueue layout");
static uint8_t bounce[4096] __attribute__((aligned(4096)));
static struct vblk_req req __attribute__((aligned(16)));
static volatile uint8_t req_status;

struct vblk vblk;

static void iofence(void)
{
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

static uint32_t rd(uint64_t off)
{
    uint32_t v = *(volatile uint32_t *)(uintptr_t)(vblk.base + off);
    iofence();
    return v;
}

static void wr(uint64_t off, uint32_t v)
{
    iofence();
    *(volatile uint32_t *)(uintptr_t)(vblk.base + off) = v;
}

static uint64_t deadline(void)
{
    /* 2 s per request: generous for QEMU, still bounded. */
    return csr_read(time) + 2u * (uint64_t)boot_configuration.timebase_hz;
}

static uint32_t rd(uint64_t off);
static void wr(uint64_t off, uint32_t v);

/* The driver gives up on the device until reboot, and tells it so: FAILED
 * in the status register (virtio 1.2 2.2.1, 3.1.1). Buffers and descriptors
 * are never written again. */
static uint64_t fail(uint64_t reason)
{
    vblk.state = VBLK_FAILED;
    vblk.reason = reason;
    if (vblk.base) wr(MMIO_STATUS, rd(MMIO_STATUS) | ST_FAILED);
    return vblk.state;
}

static uint64_t init_device(void)
{
    wr(MMIO_STATUS, 0);                     /* reset */
    uint64_t until = deadline();
    while (rd(MMIO_STATUS) != 0)
        if (csr_read(time) > until) return fail(VBLK_R_RESET);
    wr(MMIO_STATUS, ST_ACK);
    wr(MMIO_STATUS, ST_ACK | ST_DRIVER);
    wr(MMIO_DEV_FEAT_SEL, 0);
    uint64_t f = rd(MMIO_DEV_FEATURES);
    wr(MMIO_DEV_FEAT_SEL, 1);
    f |= (uint64_t)rd(MMIO_DEV_FEATURES) << 32;
    vblk.device_features = f;
    if (!(f & VIRTIO_F_VERSION_1)) return fail(VBLK_R_NO_V1);
    /* Nothing else is negotiated: no multiqueue, no event index, no
     * indirect descriptors. RO and FLUSH only describe the device. */
    vblk.features = f & (VIRTIO_F_VERSION_1 | VIRTIO_BLK_F_RO | VIRTIO_BLK_F_FLUSH);
    wr(MMIO_DRV_FEAT_SEL, 0);
    wr(MMIO_DRV_FEATURES, (uint32_t)vblk.features);
    wr(MMIO_DRV_FEAT_SEL, 1);
    wr(MMIO_DRV_FEATURES, (uint32_t)(vblk.features >> 32));
    wr(MMIO_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(rd(MMIO_STATUS) & ST_FEATURES_OK)) return fail(VBLK_R_FEATURES);
    wr(MMIO_QUEUE_SEL, 0);
    uint32_t max = rd(MMIO_QUEUE_NUM_MAX);
    if (rd(MMIO_QUEUE_READY) || max < 4) return fail(VBLK_R_QUEUE);
    /* A split queue's size is a power of two: 8, or 4 (3 descriptors used). */
    vblk.qsize = max < VBLK_QSIZE ? 4 : VBLK_QSIZE;
    wr(MMIO_QUEUE_NUM, (uint32_t)vblk.qsize);
    uint64_t d = (uintptr_t)vq.desc, a = (uintptr_t)&vq.avail, u = (uintptr_t)&vq.used;
    wr(MMIO_QUEUE_DESC, (uint32_t)d);
    wr(MMIO_QUEUE_DESC + 4, (uint32_t)(d >> 32));
    wr(MMIO_QUEUE_DRIVER, (uint32_t)a);
    wr(MMIO_QUEUE_DRIVER + 4, (uint32_t)(a >> 32));
    wr(MMIO_QUEUE_DEVICE, (uint32_t)u);
    wr(MMIO_QUEUE_DEVICE + 4, (uint32_t)(u >> 32));
    wr(MMIO_QUEUE_READY, 1);
    /* Capacity, read until the configuration generation is stable. */
    uint64_t cap = 0;
    for (unsigned tries = 0;; ++tries) {
        if (tries == 8) return fail(VBLK_R_CAPACITY);
        uint32_t g = rd(MMIO_CONFIG_GEN);
        cap = rd(MMIO_CONFIG) | (uint64_t)rd(MMIO_CONFIG + 4) << 32;
        if (rd(MMIO_CONFIG_GEN) == g) break;
    }
    if (cap < 8) return fail(VBLK_R_CAPACITY);
    vblk.capacity = cap;
    wr(MMIO_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    if (rd(MMIO_STATUS) & (ST_NEEDS_RESET | ST_FAILED)) return fail(VBLK_R_NEEDS_RESET);
    vblk.state = VBLK_READY;
    return vblk.state;
}

uint64_t vblk_probe(const uint64_t *bases, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        uint64_t b = bases[i];
        uint32_t magic = *(volatile uint32_t *)(uintptr_t)(b + MMIO_MAGIC);
        uint32_t version = *(volatile uint32_t *)(uintptr_t)(b + MMIO_VERSION);
        uint32_t id = *(volatile uint32_t *)(uintptr_t)(b + MMIO_DEVICE_ID);
        if (magic != VIRTIO_MAGIC || id != VIRTIO_ID_BLOCK) continue;
        if (version != 2) {
            vblk.legacy_base = b;
            vblk.state = VBLK_LEGACY;
            return vblk.state;
        }
        vblk.base = b;
        return init_device();
    }
    return vblk.state;      /* VBLK_NONE */
}

/* Submit the chain that starts at descriptor 0 and wait for it. The used
 * element must name that chain and report at least min_len bytes written
 * into the device-writable buffers (virtio 1.2 2.7.8.3, 5.2.6.3). */
static int submit(uint32_t min_len)
{
    if (vblk.state != VBLK_READY) return -5;
    req_status = 0xff;
    vq.avail.ring[vblk.avail_idx % vblk.qsize] = 0;
    iofence();
    vblk.avail_idx = (uint16_t)(vblk.avail_idx + 1);
    vq.avail.idx = (uint16_t)vblk.avail_idx;
    wr(MMIO_QUEUE_NOTIFY, 0);
    uint64_t until = deadline();
    while (*(volatile uint16_t *)&vq.used.idx == (uint16_t)vblk.used_seen) {
        if (csr_read(time) > until) {
            /* The device may still own the buffers: never touch them again. */
            fail(VBLK_R_TIMEOUT);
            return -5;
        }
    }
    iofence();
    const volatile struct vq_used_elem *e = &vq.used.ring[vblk.used_seen % vblk.qsize];
    uint32_t id = e->id, len = e->len;
    vblk.used_seen = (uint16_t)(vblk.used_seen + 1);
    vblk.last_len = len;
    wr(MMIO_INT_ACK, rd(MMIO_INT_STATUS));
    if (id != 0 || len < min_len) {
        fail(VBLK_R_LENGTH);
        return -5;
    }
    if (rd(MMIO_STATUS) & (ST_NEEDS_RESET | ST_FAILED)) {
        fail(VBLK_R_NEEDS_RESET);
        return -5;
    }
    if (req_status != 0) {
        fail(VBLK_R_STATUS);
        return -5;
    }
    return 0;
}

static void header(uint32_t type, uint64_t sector)
{
    req.type = type;
    req.reserved = 0;
    req.sector = sector;
    vq.desc[0] = (struct vq_desc){ (uintptr_t)&req, sizeof(req), DESC_F_NEXT, 1 };
}

static int data_request(uint32_t type, uint32_t block)
{
    if (vblk.state != VBLK_READY) return -5;    /* the device may own them */
    if ((uint64_t)block * 8u + 8u > vblk.capacity) return -5;
    header(type, (uint64_t)block * 8u);
    vq.desc[1] = (struct vq_desc){ (uintptr_t)bounce, sizeof(bounce),
                                   (uint16_t)(DESC_F_NEXT | (type == REQ_IN ? DESC_F_WRITE : 0)), 2 };
    vq.desc[2] = (struct vq_desc){ (uintptr_t)&req_status, 1, DESC_F_WRITE, 0 };
    /* a read fills the bounce buffer and the status byte, a write only the status */
    return submit(type == REQ_IN ? (uint32_t)sizeof(bounce) + 1u : 1u);
}

int vblk_read(uint32_t block, uint8_t *buf)
{
    if (data_request(REQ_IN, block)) return -5;
    for (unsigned i = 0; i < sizeof(bounce); ++i) buf[i] = bounce[i];
    vblk.reads++;
    return 0;
}

int vblk_write(uint32_t block, const uint8_t *buf)
{
    if (vblk.state != VBLK_READY) return -5;
    for (unsigned i = 0; i < sizeof(bounce); ++i) bounce[i] = buf[i];
    if (data_request(REQ_OUT, block)) return -5;
    vblk.writes++;
    return 0;
}

int vblk_flush(void)
{
    if (vblk.state != VBLK_READY || !(vblk.features & VIRTIO_BLK_F_FLUSH)) return -5;
    header(REQ_FLUSH, 0);
    vq.desc[1] = (struct vq_desc){ (uintptr_t)&req_status, 1, DESC_F_WRITE, 0 };
    if (submit(1u)) return -5;
    vblk.flushes++;
    return 0;
}
