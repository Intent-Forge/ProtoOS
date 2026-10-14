/* v1.5 test drivers: shared helpers. Results go into r[] (read by the GDB
 * probe when the program exits, through its own page table); r[63] = 0x600d
 * marks a driver that ran to the end. The data pattern is the one the
 * harness checks on the disk image: byte i of tag t is (i*131 + t*17 + 7) & 255. */
#ifndef PTEST_H
#define PTEST_H
#include "proto.h"

long r[64];

static inline unsigned char pat(unsigned long i, unsigned long tag)
{
    return (unsigned char)((i * 131u + tag * 17u + 7u) & 255u);
}

static inline void fill(unsigned char *b, unsigned long n, unsigned long tag, unsigned long from)
{
    for (unsigned long i = 0; i < n; ++i) b[i] = pat(from + i, tag);
}

/* 1 if b[0..n) is the pattern of tag starting at offset `from` */
static inline long same(const unsigned char *b, unsigned long n, unsigned long tag, unsigned long from)
{
    for (unsigned long i = 0; i < n; ++i)
        if (b[i] != pat(from + i, tag)) return 0;
    return 1;
}

static inline long count_files(long *total)
{
    char name[16];
    long n = 0, sum = 0, s;
    while ((s = fs_list((unsigned long)n, name)) >= 0) {
        sum += s;
        ++n;
    }
    if (total) *total = sum;
    return n;
}

#define DONE() do { r[63] = 0x600d; return 0; } while (0)
#endif
