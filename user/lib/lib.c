#include "proto.h"
#include <limits.h>

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

/* v1.5: every start goes through run_arg (1017); 1008 stays for old code. */
long sys_run(const char *name)
{
    return sys_run_arg(name, "");
}

long sys_run_arg(const char *name, const char *arg)
{
    return proto_call5(1017, (long)name, (long)strlen(name), (long)arg, (long)strlen(arg), 0);
}

long get_arg(char *buf, size_t max)
{
    if (max == 0) return -1;
    long n = proto_call(1016, (long)buf, (long)(max - 1), 0);
    if (n < 0) return n;
    if ((size_t)n > max - 1) n = (long)(max - 1);
    buf[n] = 0;
    return n;
}

long fs_list(unsigned long index, char name[16])
{
    return proto_call(1012, (long)index, (long)name, 0);
}

long fs_read(const char *name, unsigned long off, void *buf, unsigned long len)
{
    return proto_call5(1013, (long)name, (long)strlen(name), (long)off, (long)buf, (long)len);
}

long fs_write(const char *name, const void *buf, unsigned long len)
{
    return proto_call5(1014, (long)name, (long)strlen(name), (long)buf, (long)len, 0);
}

long fs_delete(const char *name)
{
    return proto_call(1015, (long)name, (long)strlen(name), 0);
}

long fd_open(const char *name, int flags)
{
    return proto_call(1018, (long)name, (long)strlen(name), flags);
}

long fd_read(long fd, void *buf, unsigned long len)
{
    return proto_call(63, fd, (long)buf, (long)len);
}

long fd_write(long fd, const void *buf, unsigned long len)
{
    return proto_call(64, fd, (long)buf, (long)len);
}

long fd_close(long fd)
{
    return proto_call(1019, fd, 0, 0);
}

long fd_seek(long fd, long off, int whence)
{
    return proto_call(1020, fd, off, whence);
}

void print_error(const char *prog, long err)
{
    const char *what = "error";
    switch (err) {
    case E_NOENT: what = "no such file"; break;
    case E_IO: what = "disk error (the file is damaged or the disk failed)"; break;
    case E_NODEV: what = "no disk"; break;
    case E_FBIG: what = "file too large (at most 128 KiB)"; break;
    case E_NOSPC: what = "the disk is full"; break;
    case E_ROFS: what = "the disk is read-only"; break;
    case E_INVAL: what = "bad name (1-15 of a-z 0-9 _ .)"; break;
    case E_NOEXEC: what = "not a program"; break;
    case E_NOMEM: what = "out of memory"; break;
    case E_BADF: what = "bad file descriptor"; break;
    case E_MFILE: what = "too many open files"; break;
    }
    print(prog);
    print(": ");
    print(what);
    if (what[0] == 'e' && what[1] == 'r') {     /* not a known code: show its number */
        print(" ");
        print_int(err);
    }
    print("\n");
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

/* 1 after a CR: an LF right after it ends no second line. It starts at 1
 * because the shell that started the program read the CR of its command
 * line; an LF sent with it must not become an empty first line here. */
static int after_cr = 1;

long read_line(char *buf, size_t max)
{
    size_t n = 0;
    if (max == 0) return -1;
    for (;;) {
        char c;
        if (sys_read(&c, 1) != 1) continue;
        int lf_of_crlf = (c == '\n' && after_cr);
        after_cr = (c == '\r');
        if (lf_of_crlf) continue;
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
    if (n > (size_t)LONG_MAX - 15u) return 0;       /* rounding would wrap */
    n = (n + 15u) & ~(size_t)15u;
    long p = sys_sbrk((long)n);
    return p < 0 ? 0 : (void *)p;
}
