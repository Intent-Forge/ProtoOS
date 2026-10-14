/* v1.5: glue between the virtio-blk driver and PFS, and the boot lines. */
#include "kernel.h"
#include "disk.h"
#include "virtio_blk.h"
#include "vm.h"

struct disk_state disk;
struct pfs disk_fs;
uint8_t disk_buf[PFS_FILE_MAX] __attribute__((aligned(4096)));

static int dev_read(void *ctx, uint32_t block, uint8_t *buf)
{
    (void)ctx;
    return vblk_read(block, buf);
}

static int dev_write(void *ctx, uint32_t block, const uint8_t *buf)
{
    (void)ctx;
    return vblk_write(block, buf);
}

static int dev_flush(void *ctx)
{
    (void)ctx;
    return vblk_flush();
}

static struct pfs_dev dev = { 0, dev_read, dev_write, dev_flush, 0, 0 };

void disk_init(void)
{
    const struct boot_config *bc = &boot_configuration;
    uint64_t usable[BOOT_VIRTIO_MAX];
    unsigned n = 0;
    for (unsigned i = 0; i < bc->virtio_count; ++i)
        if (vm_virtio_usable(bc->virtio[i])) usable[n++] = bc->virtio[i];
    uint64_t st = vblk_probe(usable, n);
    if (st == VBLK_NONE) return;
    if (st == VBLK_LEGACY) {
        uart_puts("DISK refused: legacy virtio-mmio block device at ");
        uart_hex(vblk.legacy_base); uart_puts("\n");
        return;
    }
    if (st != VBLK_READY) {
        uart_puts("DISK failed: virtio-blk at "); uart_hex(vblk.base);
        uart_puts(" reason="); uart_dec(vblk.reason); uart_puts("\n");
        return;
    }
    int flush = (vblk.features & VIRTIO_BLK_F_FLUSH) != 0;
    int ro = (vblk.features & VIRTIO_BLK_F_RO) != 0;
    uart_puts("DISK virtio-blk at "); uart_hex(vblk.base);
    uart_puts(": "); uart_dec(vblk.capacity); uart_puts(" sectors, ");
    uart_puts(flush ? "flush" : "no flush");
    /* Decision C: without flush the write order cannot be made durable,
     * so the disk is read-only even if it is write-through. */
    uart_puts(!flush || ro ? ", read-only\n" : ", read-write\n");
    dev.blocks = (uint32_t)(vblk.capacity / 8u > PFS_MAX_BLOCKS ? PFS_MAX_BLOCKS
                                                                : vblk.capacity / 8u);
    dev.readonly = !flush || ro;
    int r = pfs_mount(&disk_fs, &dev);
    if (r) {
        disk.reject = (uint64_t)r;
        uart_puts("DISK rejected code=-"); uart_dec((uint64_t)r); uart_puts("\n");
        return;
    }
    disk.mounted = 1;
    uart_puts("DISK "); uart_dec(disk_fs.blocks); uart_puts(" blocks, ");
    uart_dec(disk_fs.files); uart_puts(" files\n");
}
