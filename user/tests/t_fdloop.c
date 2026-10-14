/* T29 part 2 (v1.6): the T27 loop through descriptors. Replace "g" and "h"
 * forever with a new generation, written in pieces of 1000 bytes and saved
 * by close; QEMU is killed at a random moment, so also around close. The
 * content is exactly that of t_crash (the harness checks it the same way);
 * "saved g <gen>" is printed only after close returned 0. */
#include "ptest.h"

static unsigned char buf[1000u + 6u * 9000u];     /* the largest generation: 55000 bytes */

static unsigned long size_of(unsigned long gen)
{
    return 1000u + (gen % 7u) * 9000u;
}

static long save(const char *f, unsigned long n)
{
    long fd = fd_open(f, FD_WRITE);
    if (fd < 0) return fd;
    for (unsigned long off = 0; off < n;) {
        unsigned long k = n - off < 1000 ? n - off : 1000;
        long w = fd_write(fd, buf + off, k);
        if (w != (long)k) return w < 0 ? w : -1000;
        off += k;
    }
    return fd_close(fd);
}

static void saved(const char *f, unsigned long gen)
{
    print("saved ");
    print(f);
    print(" ");
    print_dec(gen);
    print("\n");
}

int main(void)
{
    unsigned long gen = 0;
    long fd = fd_open("g", FD_READ);
    if (fd >= 0) {
        if (fd_read(fd, buf, 8) == 8)
            for (int i = 0; i < 8; ++i) gen |= (unsigned long)buf[i] << (8 * i);
        fd_close(fd);
    }
    print("t_fdloop: starting after generation ");
    print_dec(gen);
    print("\n");
    for (;;) {
        ++gen;
        unsigned long n = size_of(gen);
        fill(buf, n, gen & 255u, 0);
        for (int i = 0; i < 8; ++i) buf[i] = (unsigned char)(gen >> (8 * i));
        long e = save("g", n);
        if (e) { print_error("t_fdloop g", e); return 1; }
        saved("g", gen);
        e = save("h", n);
        if (e) { print_error("t_fdloop h", e); return 1; }
        saved("h", gen);
    }
}
