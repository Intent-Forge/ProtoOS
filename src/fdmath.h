#ifndef PROTOOS_FDMATH_H
#define PROTOOS_FDMATH_H
#include <stdint.h>

/* v1.6: the arithmetic of file descriptors (positions, bounds, pages), pure
 * so the host tests can check it (tests/test_fdmath.c). A read descriptor is
 * a snapshot of `size` bytes with a position; a write descriptor is a buffer
 * that grows by appending. */

#define FD_PAGE 4096u

/* Pages that hold `size` bytes. */
static inline uint64_t fd_pages_for(uint64_t size)
{
    return size / FD_PAGE + (size % FD_PAGE != 0);
}

/* lseek: the new position, or -1 if whence is not 0/1/2 or the position
 * would fall outside 0 .. size. off is a signed byte offset. */
static inline int64_t fd_seek(uint64_t size, uint64_t pos, int64_t off, uint64_t whence)
{
    uint64_t base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = pos;
    else if (whence == 2) base = size;
    else return -1;
    if (base > size) return -1;
    if (off < 0) {
        uint64_t back = (uint64_t)0 - (uint64_t)off;   /* no overflow for INT64_MIN */
        if (back > base) return -1;
        return (int64_t)(base - back);
    }
    if ((uint64_t)off > size - base) return -1;
    return (int64_t)(base + (uint64_t)off);
}

/* Bytes a read of len at pos returns: 0 at or after the end. */
static inline uint64_t fd_read_count(uint64_t size, uint64_t pos, uint64_t len)
{
    if (pos >= size) return 0;
    return size - pos < len ? size - pos : len;
}

/* 1 if appending len bytes to size keeps the file at most max bytes. */
static inline int fd_write_fits(uint64_t size, uint64_t len, uint64_t max)
{
    return size <= max && len <= max - size;
}
#endif
