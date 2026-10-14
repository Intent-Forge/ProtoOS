/* ProtoOS v1.4/v1.5 user library: system calls and a few helpers for programs
 * in the program archive or on the disk. Freestanding C, no libc. System call numbers
 * and error codes as in src/proc.h (errors are small negative numbers). */
#ifndef PROTO_H
#define PROTO_H
#include <stddef.h>
#include <stdint.h>

#define E_PERM    (-1)
#define E_NOENT   (-2)
#define E_SRCH    (-3)
#define E_NOEXEC  (-8)
#define E_AGAIN   (-11)
#define E_NOMEM   (-12)
#define E_FAULT   (-14)
#define E_BUSY    (-16)
#define E_INVAL   (-22)
/* v1.5 */
#define E_IO      (-5)
#define E_NODEV   (-19)
#define E_FBIG    (-27)
#define E_NOSPC   (-28)
#define E_ROFS    (-30)
#define WAIT_FOREGROUND 1
#define FILE_MAX  (128 * 1024)      /* v1.5: largest file on the disk */
#define ARG_MAX   55                /* v1.5: longest argument line */

/* v1.5: five arguments (fs_read) */
static inline long proto_call5(long nr, long a0, long a1, long a2, long a3, long a4)
{
    register long r0 __asm__("a0") = a0;
    register long r1 __asm__("a1") = a1;
    register long r2 __asm__("a2") = a2;
    register long r3 __asm__("a3") = a3;
    register long r4 __asm__("a4") = a4;
    register long r7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r7)
                     : "memory");
    return r0;
}

static inline long proto_call(long nr, long a0, long a1, long a2)
{
    register long r0 __asm__("a0") = a0;
    register long r1 __asm__("a1") = a1;
    register long r2 __asm__("a2") = a2;
    register long r7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

static inline long sys_read(void *buf, size_t len) { return proto_call(63, 0, (long)buf, (long)len); }
static inline long sys_write(const void *buf, size_t len) { return proto_call(64, 1, (long)buf, (long)len); }
static inline long sys_getpid(void) { return proto_call(172, 0, 0, 0); }
static inline long sys_yield(void) { return proto_call(124, 0, 0, 0); }
static inline long sys_sleep(unsigned long ms) { return proto_call(1001, (long)ms, 0, 0); }
static inline unsigned long sys_uptime(void) { return (unsigned long)proto_call(1002, 0, 0, 0); }
static inline long sys_kill(long pid) { return proto_call(1005, pid, 0, 0); }
static inline long sys_wait(long pid, unsigned long flags) { return proto_call(1009, pid, (long)flags, 0); }
static inline long sys_sbrk(long bytes) { return proto_call(1010, bytes, 0, 0); }
static inline long sys_progs(unsigned long index, char name[16]) { return proto_call(1011, (long)index, (long)name, 0); }
__attribute__((noreturn)) static inline void sys_exit(long code)
{
    proto_call(93, code, 0, 0);
    __builtin_unreachable();
}
/* meminfo: free pages, *total = pages the allocator manages */
static inline long sys_meminfo(long *total)
{
    register long r0 __asm__("a0") = 0;
    register long r1 __asm__("a1") = 0;
    register long r7 __asm__("a7") = 1006;
    __asm__ volatile("ecall" : "+r"(r0), "+r"(r1) : "r"(r7) : "memory");
    if (total) *total = r1;
    return r0;
}

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);

long sys_run(const char *name);                 /* pid or error (run_arg, no argument) */
/* v1.5 */
long sys_run_arg(const char *name, const char *arg);   /* pid or error */
/* The argument line this program was started with, NUL-terminated
 * (at most max-1 characters); returns its length. */
long get_arg(char *buf, size_t max);
long fs_list(unsigned long index, char name[16]);      /* size, or E_NOENT past the end */
long fs_read(const char *name, unsigned long off, void *buf, unsigned long len);
long fs_write(const char *name, const void *buf, unsigned long len);   /* 0 when durable */
long fs_delete(const char *name);
/* "<prog>: <what went wrong>" for an error code */
void print_error(const char *prog, long err);
void print(const char *s);                      /* any length */
void print_dec(unsigned long v);
void print_int(long v);
/* Read one line with echo and backspace editing; the line ends with Enter
 * and is NUL-terminated (at most max-1 characters). Returns its length. */
long read_line(char *buf, size_t max);       /* -1 if max is 0 */
/* Decimal number with an optional '-' and nothing else: 1 and *out, else 0. */
int to_int(const char *s, long *out);
/* Heap memory from sbrk (never freed); 0 if the kernel refuses. */
void *malloc(size_t n);
#endif
