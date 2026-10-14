/* N17 (v1.6): fuzz <seed> <calls>. Random system calls 0..1100 (and random
 * 64-bit numbers) with hostile arguments; calls that may legitimately block
 * forever are tamed. Every child it starts is killed at the end. The harness
 * judges the kernel from outside: QEMU trace (no S-mode exception), GDB (no
 * page lost, A and B alive), pfsck (the disk). r[0] calls made, r[1] odd
 * results (an unknown number that did not give -38), r[2] the first meminfo. */
#include "ptest.h"

static uint64_t rng;
static uint64_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

/* The kernel may write here; nothing the fuzzer needs lives after it. */
static struct { uint64_t guard[512]; char scratch[3 * 4096]; } mem __attribute__((aligned(4096)));

static long kids[64];
static unsigned nkids;
static long dead[64];
static unsigned ndead;
static long self;
static unsigned long hist[8];       /* 0 ok/positive, then error classes */
static unsigned long odd;

static const long known[] = { 63, 64, 124, 172, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008,
                              1009, 1010, 1011, 1012, 1013, 1014, 1015, 1016, 1017,
                              1018, 1019, 1020 };          /* v1.6: open, close, lseek */
#define NKNOWN (sizeof(known) / sizeof(known[0]))

static const char *const names[] = { "primes", "clock", "hello", "cat", "files", "note", "rm",
                                     "guess", "a", "memo", "x.txt", "t_fs", "b_kernel",
                                     "zzzzzzzzzzzzzzz", "Bad", "", "nosuch" };
#define NNAMES (sizeof(names) / sizeof(names[0]))

static uint64_t bad_ptr(void)
{
    static const uint64_t p[] = {
        0, 1, 8, 0xfff, 0x1000, 0x10000000u, 0x10008000u, 0x0c000000u, 0x80000000u, 0x80200000u,
        0x80400000u, 0x87e00000u, 0x4000000u, 0x400000u, 0x800000u, 0xc00000u, 0x1000000u,
        0x401fb000u, 0x401fb800u, 0x40000000u, 0x40200000u, (1ull << 38) - 1, 1ull << 38,
        (1ull << 38) - 16, 0xffffffffffffffffull, 0x8000000000000000ull, 0xffffffc000000000ull };
    return p[rnd() % (sizeof(p) / sizeof(p[0]))];
}

static uint64_t good_ptr(void)
{
    uint64_t base = (uint64_t)(uintptr_t)mem.scratch;
    switch (rnd() % 4) {
    case 0: return base;
    case 1: return base + 4096 - (rnd() % 64);
    case 2: return base + sizeof(mem.scratch) - 1 - (rnd() % 16);
    default: return base + (rnd() % sizeof(mem.scratch));
    }
}

static uint64_t ptr(void) { return rnd() % 2 ? good_ptr() : bad_ptr(); }

static uint64_t length(void)
{
    static const uint64_t l[] = { 0, 1, 15, 16, 17, 55, 56, 63, 64, 127, 128, 129, 4095, 4096,
                                  4097, 65536, 131072, 131073, 1ull << 32, 1ull << 38,
                                  0x7fffffffffffffffull, 0xffffffffffffffffull };
    return rnd() % 3 ? l[rnd() % (sizeof(l) / sizeof(l[0]))] : rnd() % 200;
}

static uint64_t number(void)
{
    switch (rnd() % 5) {
    case 0: return rnd() % 10;
    case 1: return rnd() % 300;
    case 2: return (uint64_t)-(int64_t)(rnd() % 10);
    case 3: return rnd();
    default: return length();
    }
}

static void started(long pid)
{
    if (pid > 0 && nkids < 64) kids[nkids++] = pid;
}

/* Kill every child still alive and remember it as dead. */
static void reap(void)
{
    for (unsigned i = 0; i < nkids; ++i) {
        sys_kill(kids[i]);
        if (ndead < 64) dead[ndead++] = kids[i];
        else dead[rnd() % 64] = kids[i];
    }
    nkids = 0;
}

static long call(void)
{
    long nr = rnd() % 8 ? known[rnd() % NKNOWN] : (long)(rnd() % 4 ? rnd() % 1100 : rnd());
    uint64_t a[6];
    for (int i = 0; i < 6; ++i) {
        switch (rnd() % 3) {
        case 0: a[i] = ptr(); break;
        case 1: a[i] = length(); break;
        default: a[i] = number(); break;
        }
    }
    /* names: sometimes a real name string */
    if (rnd() % 2) {
        const char *n = names[rnd() % NNAMES];
        a[0] = (uint64_t)(uintptr_t)n;
        a[1] = rnd() % 4 ? strlen(n) : length();
    }
    switch (nr) {
    case 93: return 0;                                  /* exit: never */
    case 63:                                            /* read: never block on the console */
        if (a[0] == 0) a[2] = 0;
        break;
    case 64:                                            /* write: little console noise */
        if (a[0] == 1 && a[2] > 8) a[2] = rnd() % 9;
        break;
    case 1001: a[0] %= 11; break;                       /* sleep: at most 10 ms */
    case 1005:                                          /* kill: not myself */
        if (rnd() % 2 && nkids) a[0] = (uint64_t)kids[rnd() % nkids];
        if ((long)a[0] == self) a[0] = 0;
        break;
    case 1009: {                                        /* wait: only for what cannot block */
        static const long w[] = { 0, -1, 1, 2, 999999 };
        long pid = ndead && rnd() % 2 ? dead[rnd() % ndead] : w[rnd() % 5];
        if (rnd() % 4 == 0) pid = self;
        a[0] = (uint64_t)pid;
        if (pid == 1 || pid == 2) a[1] = 1;             /* foreground: refused (-1) */
        a[1] &= 1;
        break;
    }
    case 1004: a[0] = rnd() % 4 ? rnd() % 8 : a[0]; break;
    default: break;
    }
    long r = proto_call5(nr, (long)a[0], (long)a[1], (long)a[2], (long)a[3], (long)a[4]);
    if (nr == 1004 || nr == 1008 || nr == 1017) started(r);
    int known_nr = 0;
    for (unsigned i = 0; i < NKNOWN; ++i) known_nr |= known[i] == nr;
    if (!known_nr && nr != 93 && r != -38) {
        print("FUZZ ODD unknown nr="); print_int(nr); print(" returned "); print_int(r); print("\n");
        odd++;
    }
    return r;
}

int main(void)
{
    char arg[ARG_MAX + 1], *p = arg;
    get_arg(arg, sizeof(arg));
    long seed = 1, calls = 20000;
    while (*p && *p != ' ') ++p;
    if (*p == ' ') { *p++ = 0; to_int(p, &calls); }
    to_int(arg, &seed);
    r[2] = sys_meminfo(0);                  /* the probe's first stop */
    rng = (uint64_t)seed * 0x9e3779b97f4a7c15ull + 1;
    self = sys_getpid();
    print("FUZZ START seed="); print_int(seed); print(" calls="); print_int(calls); print("\n");
    for (long i = 1; i <= calls; ++i) {
        long res = call();
        hist[res >= 0 ? 0 : (-res < 7 ? (unsigned)-res : 7)]++;
        if (sys_getpid() != self) { print("FUZZ ODD getpid changed\n"); odd++; }
        if (nkids >= 4 || i % 64 == 0) reap();
        if (i % 2000 == 0) { print("FUZZ "); print_int(i); print("\n"); }
    }
    reap();
    print("FUZZ DONE ok="); print_dec(hist[0]);
    print(" odd="); print_dec(odd); print("\n");
    r[0] = calls;
    r[1] = (long)odd;
    DONE();
}
