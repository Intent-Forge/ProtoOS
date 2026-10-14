/* T28 (v1.6): files through descriptors on an empty 1 MiB disk. Written in
 * pieces, read back in other pieces, lseek, a snapshot that keeps its
 * content while the file is replaced, a write buffer invisible until close,
 * the fifth open, the 64-page limit, every page back. The buffer is static
 * and small: a 128 KiB snapshot alone takes 32 of the 64 pages. */
#include "ptest.h"

#define PIECE_MAX 65536u
static unsigned char buf[PIECE_MAX];

static const unsigned long sizes[6] = { 0, 1, 4095, 4096, 4097, FILE_MAX };
static const char *const names[6] = { "f0", "f1", "f2", "f3", "f4", "f5" };
static const unsigned long wpiece[6] = { 7, 1, 1, 4096, 7, 65536 };
static const unsigned long rpiece[6] = { 5, 3, 1000, 4096, 40, 40000 };

/* Write file k in pieces; 0 if every write took its piece and close gave 0. */
static long write_file(int k)
{
    long fd = fd_open(names[k], FD_WRITE);
    if (fd < 0) return fd;
    for (unsigned long off = 0; off < sizes[k];) {
        unsigned long n = sizes[k] - off < wpiece[k] ? sizes[k] - off : wpiece[k];
        fill(buf, n, (unsigned long)k + 1, off);
        if (fd_write(fd, buf, n) != (long)n) return -1000;
        off += n;
    }
    return fd_close(fd);
}

/* Read file k back in pieces: its size if every byte matched and the read
 * after the end gave 0. */
static long read_file(int k, unsigned long tag)
{
    long fd = fd_open(names[k], FD_READ);
    if (fd < 0) return fd;
    unsigned long got = 0;
    for (;;) {
        long n = fd_read(fd, buf, rpiece[k]);
        if (n < 0) return n;
        if (n == 0) break;
        if (!same(buf, (unsigned long)n, tag, got)) return -2000;
        got += (unsigned long)n;
    }
    if (fd_read(fd, buf, 10) != 0) return -3000;
    long c = fd_close(fd);
    return c ? c : (long)got;
}

int main(void)
{
    r[0] = sys_meminfo(0);
    for (int k = 0; k < 6; ++k) r[1 + k] = write_file(k);                       /* r1-6: 0 */
    for (int k = 0; k < 6; ++k) r[7 + k] = read_file(k, (unsigned long)k + 1); /* r7-12: sizes */

    /* lseek on f4 (4097 bytes, tag 5) */
    long fd = fd_open("f4", FD_READ);
    r[13] = fd;                                         /* 3 */
    r[14] = fd_seek(fd, 0, 2);                          /* 4097 */
    r[15] = fd_seek(fd, -1, 2);                         /* 4096 */
    r[16] = fd_read(fd, buf, 10);                       /* 1 */
    r[17] = same(buf, 1, 5, 4096);                      /* 1 */
    r[18] = fd_seek(fd, 100, 0);                        /* 100 */
    r[19] = fd_seek(fd, 50, 1);                         /* 150 */
    r[20] = fd_read(fd, buf, 10) == 10 && same(buf, 10, 5, 150);     /* 1 */
    r[21] = fd_seek(fd, -1, 0);                         /* -22 */
    r[22] = fd_seek(fd, 4098, 0);                       /* -22 */
    r[23] = fd_seek(fd, 0, 3);                          /* -22 */
    r[24] = fd_close(fd);                               /* 0 */

    /* a snapshot keeps its content while the file is replaced */
    fd = fd_open("f3", FD_READ);                        /* tag 4 */
    fill(buf, 4096, 9, 0);
    r[25] = fs_write("f3", buf, 4096);                  /* 0: f3 is now tag 9 */
    r[26] = fd_read(fd, buf, 4096) == 4096 && same(buf, 4096, 4, 0); /* 1: still tag 4 */
    r[27] = fd_close(fd);                               /* 0 */
    r[28] = fs_read("f3", 0, buf, 4096) == 4096 && same(buf, 4096, 9, 0);   /* 1 */

    /* a write buffer is invisible until close */
    fd = fd_open("f1", FD_WRITE);
    fill(buf, 3, 7, 0);
    r[29] = fd_write(fd, buf, 3);                       /* 3 */
    r[30] = fs_read("f1", 0, buf, 10) == 1 && same(buf, 1, 2, 0);    /* 1: still the old byte */
    r[31] = fd_close(fd);                               /* 0 */
    r[32] = fs_read("f1", 0, buf, 10) == 3 && same(buf, 3, 7, 0);    /* 1: now new */

    /* four open files, the fifth -24 (f0 is empty: no pages) */
    long f[5];
    for (int i = 0; i < 5; ++i) f[i] = fd_open("f0", FD_READ);
    r[33] = f[0] == 3 && f[1] == 4 && f[2] == 5 && f[3] == 6;        /* 1 */
    r[34] = f[4];                                       /* -24 */
    r[35] = 0;
    for (int i = 0; i < 4; ++i) r[35] += fd_close(f[i]);            /* 0 */

    /* the 64-page limit: one 128 KiB snapshot fits, a second does not */
    long a = fd_open("f5", FD_READ);
    r[36] = a;                                          /* 3 */
    r[37] = fd_open("f5", FD_READ);                     /* -12 */
    long w = fd_open("w1", FD_WRITE);
    r[38] = w;                                          /* 4 */
    fill(buf, PIECE_MAX, 8, 0);
    r[39] = fd_write(w, buf, PIECE_MAX);                /* -12: all or nothing */
    r[40] = fd_write(w, buf, 1);                        /* 1: the buffer still works */
    r[41] = fd_close(a);                                /* 0: the snapshot gave back */
    r[42] = fd_write(w, buf + 1, PIECE_MAX - 1);        /* 65535: room again */
    r[43] = fd_close(w);                                /* 0: w1 saved, 65536 bytes */
    r[44] = fs_read("w1", 0, buf, PIECE_MAX) == (long)PIECE_MAX && same(buf, PIECE_MAX, 8, 0); /* 1 */

    r[45] = sys_meminfo(0);                             /* = r[0]: every page back */
    DONE();
}
