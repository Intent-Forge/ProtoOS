/* N14: bad pointers, bad names and lengths, a damaged file (CRC), a broken
 * and a damaged program on the disk; no page leaks. Booted on crcbad.img:
 * good (3000 bytes, tag 7), dmg (9000 bytes, tag 8, last block damaged),
 * badelf (t_exit42 with a broken ELF magic), dmgprog (t_exit42, its last
 * block damaged). */
#include "ptest.h"

#define KERNEL 0x80200000L      /* a kernel page: never U */

int main(void)
{
    unsigned char *buf = malloc(FILE_MAX);
    if (!buf) return 1;
    char name[16];
    r[0] = sys_meminfo(0);
    r[1] = proto_call5(1013, (long)"good", 0, 0, (long)buf, 10);       /* namelen 0: -22 */
    r[2] = fs_read("abcdefghijklmnop", 0, buf, 10);                    /* 16 chars: -22 */
    r[3] = fs_read("Bad", 0, buf, 10);                                 /* uppercase: -22 */
    r[4] = proto_call5(1013, KERNEL, 4, 0, (long)buf, 10);             /* name ptr: -14 */
    r[5] = proto_call5(1014, (long)"x", 1, KERNEL, 10, 0);             /* data ptr: -14 */
    r[6] = proto_call5(1013, (long)"good", 4, 0, KERNEL, 10);          /* out ptr: -14 */
    r[7] = proto_call(1012, 0, KERNEL, 0);                             /* list ptr: -14 */
    r[8] = fs_write("x", buf, FILE_MAX + 1);                           /* -27 */
    r[9] = proto_call5(1017, (long)"t_exit42", 8, (long)buf, ARG_MAX + 1, 0);   /* -22 */
    r[10] = proto_call5(1017, (long)"t_exit42", 8, KERNEL, 4, 0);      /* arg ptr: -14 */
    r[11] = proto_call(1016, KERNEL, 10, 0);                           /* args ptr: -14 */
    r[12] = fs_read("good", 0, buf, FILE_MAX);                         /* 3000 */
    r[13] = same(buf, 3000, 7, 0);
    r[14] = fs_read("dmg", 0, buf, 100);        /* -5: the whole file is checked */
    r[15] = fs_read("dmg", 8900, buf, 100);     /* -5 */
    r[16] = fs_read("good", 0, buf, FILE_MAX);  /* 3000: the system goes on */
    r[17] = sys_run("badelf");                  /* -8 */
    r[18] = sys_run("dmgprog");                 /* -5 */
    r[19] = sys_run("nosuch");                  /* -2 */
    r[20] = count_files(0);                     /* 4 */
    r[21] = fs_write("ok", "abc", 3);           /* 0: writing still works */
    r[22] = fs_read("ok", 0, name, 16);         /* 3 */
    r[23] = sys_meminfo(0);                     /* = r[0]: nothing leaked */
    DONE();
}
