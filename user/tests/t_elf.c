/* T18 test program: the kernel loaded every segment correctly. Exit code
 * 0 if all self-checks pass, else the number of the first failed check.
 * t_elf_checkpoint() is called before anything in .data or .bss is
 * written, so the harness can compare those pages with the file. */
#include "proto.h"

extern char _end[];
static const char ro_text[] = "ProtoOS v1.4 constant data";
long data_words[64] = {
#define W(i) (0x0101010101010101L * (i) + 0x5a)
    W(0), W(1), W(2), W(3), W(4), W(5), W(6), W(7), W(8), W(9), W(10), W(11), W(12),
    W(13), W(14), W(15), W(16), W(17), W(18), W(19), W(20), W(21), W(22), W(23), W(24),
    W(25), W(26), W(27), W(28), W(29), W(30), W(31), W(32), W(33), W(34), W(35), W(36),
    W(37), W(38), W(39), W(40), W(41), W(42), W(43), W(44), W(45), W(46), W(47), W(48),
    W(49), W(50), W(51), W(52), W(53), W(54), W(55), W(56), W(57), W(58), W(59), W(60),
    W(61), W(62), W(63),
};
static unsigned char bss_area[3 * 4096 + 123];

__attribute__((noinline)) void t_elf_checkpoint(long pid)
{
    __asm__ volatile("" :: "r"(pid) : "memory");
}

__attribute__((noinline)) static long depth(long n)
{
    volatile char pad[256];
    pad[0] = (char)n;
    pad[255] = (char)(n + 1);
    if (n == 0) return pad[0];
    return depth(n - 1) + pad[255];
}

int main(void)
{
    for (long i = 0; i < 64; ++i)
        if (data_words[i] != W(i)) return 1;
    for (unsigned i = 0; i < sizeof bss_area; ++i)
        if (bss_area[i]) return 2;
    if (strlen(ro_text) != 26 || ro_text[0] != 'P' || ro_text[25] != 'a') return 3;
    long pid = sys_getpid();
    if (pid < 3) return 4;
    if (sys_sbrk(0) != (long)_end) return 5;
    t_elf_checkpoint(pid);
    /* 33 frames of > 256 bytes: about 10 KiB of the 16 KiB stack */
    if (depth(32) != 560) return 6;         /* 2 + 3 + ... + 33 */
    data_words[0] = 1;
    bss_area[5000] = 9;
    if (data_words[0] != 1 || bss_area[5000] != 9) return 7;
    print("t_elf: all checks passed\n");
    return 0;
}
