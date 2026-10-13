#include "proto.h"

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}

void *memset(void *dst, int c, size_t n)
{
    volatile unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    volatile unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

long sys_run(const char *name)
{
    return proto_call(1008, (long)name, (long)strlen(name), 0);
}

/* write() takes at most 256 bytes per call */
void print(const char *s)
{
    size_t n = strlen(s);
    while (n) {
        size_t chunk = n < 256 ? n : 256;
        sys_write(s, chunk);
        s += chunk;
        n -= chunk;
    }
}

void print_dec(unsigned long v)
{
    char buf[24];
    int i = 23;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    print(buf + i);
}

void print_int(long v)
{
    if (v < 0) { print("-"); print_dec(0ul - (unsigned long)v); }
    else print_dec((unsigned long)v);
}

long read_line(char *buf, size_t max)
{
    size_t n = 0;
    for (;;) {
        char c;
        if (sys_read(&c, 1) != 1) continue;
        if (c == '\r' || c == '\n') {
            sys_write("\n", 1);
            buf[n] = 0;
            return (long)n;
        }
        if (c == 8 || c == 127) {
            if (n) { --n; sys_write("\b \b", 3); }
        } else if (c >= 32 && c < 127 && n + 1 < max) {
            buf[n++] = c;
            sys_write(&c, 1);
        }
    }
}

int to_int(const char *s, long *out)
{
    int neg = (*s == '-');
    long v = 0;
    if (neg) ++s;
    if (!*s) return 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || v > 100000000) return 0;
        v = v * 10 + (*s - '0');
    }
    *out = neg ? -v : v;
    return 1;
}

void *malloc(size_t n)
{
    n = (n + 15u) & ~(size_t)15u;
    long p = sys_sbrk((long)n);
    return p < 0 ? 0 : (void *)p;
}
