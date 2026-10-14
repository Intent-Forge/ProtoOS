/* T27: replace files "g" and "h" forever, each time with a new generation
 * number. QEMU is killed at a random moment. Content of generation gen: the
 * number in the first 8 bytes, then the pattern of tag (gen & 255), size
 * 1000 + (gen % 7) * 9000. "saved g <gen>" is printed only after fs_write
 * returned 0, i.e. after the new directory is durable. */
#include "ptest.h"

static unsigned long size_of(unsigned long gen)
{
    return 1000u + (gen % 7u) * 9000u;
}

static void make(unsigned char *b, unsigned long gen)
{
    unsigned long n = size_of(gen);
    fill(b, n, gen & 255u, 0);
    for (int i = 0; i < 8; ++i) b[i] = (unsigned char)(gen >> (8 * i));
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
    unsigned char *buf = malloc(FILE_MAX);
    if (!buf) return 1;
    unsigned long gen = 0;
    if (fs_read("g", 0, buf, 8) == 8)
        for (int i = 0; i < 8; ++i) gen |= (unsigned long)buf[i] << (8 * i);
    print("t_crash: starting after generation ");
    print_dec(gen);
    print("\n");
    for (;;) {
        ++gen;
        make(buf, gen);
        long e = fs_write("g", buf, size_of(gen));
        if (e) { print_error("t_crash g", e); return 1; }
        saved("g", gen);
        e = fs_write("h", buf, size_of(gen));
        if (e) { print_error("t_crash h", e); return 1; }
        saved("h", gen);
    }
}
