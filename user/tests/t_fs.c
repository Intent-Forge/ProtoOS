/* T24: write, read back, partial reads, replace, delete, disk full, the
 * 64th file. Booted on an empty 1 MiB disk (256 blocks). */
#include "ptest.h"

static const unsigned long sizes[6] = { 0, 1, 4095, 4096, 4097, FILE_MAX };
static const char *const names[6] = { "s0", "s1", "s2", "s3", "s4", "s5" };

static void numbered(char *n, char c, long i)
{
    n[0] = c;
    n[1] = (char)('0' + i / 10);
    n[2] = (char)('0' + i % 10);
    n[3] = 0;
}

int main(void)
{
    unsigned char *buf = malloc(FILE_MAX + 1);
    if (!buf) return 1;
    char n[4];
    for (int k = 0; k < 6; ++k) {                       /* r0-5: write */
        fill(buf, sizes[k], (unsigned long)k + 1, 0);
        r[k] = fs_write(names[k], buf, sizes[k]);
    }
    for (int k = 0; k < 6; ++k) {                       /* r6-11 read, r12-17 same */
        memset(buf, 0xee, FILE_MAX);
        r[6 + k] = fs_read(names[k], 0, buf, FILE_MAX);
        r[12 + k] = same(buf, sizes[k], (unsigned long)k + 1, 0);
    }
    r[18] = fs_write("s6", buf, FILE_MAX + 1);          /* -27 */
    r[19] = fs_read("s4", 4000, buf, 200);              /* 97 */
    r[20] = same(buf, 97, 5, 4000);
    r[21] = fs_read("s4", 5000, buf, 10);               /* 0: past the end */
    r[22] = fs_read("s3", 1000, buf, 100);              /* 100 */
    r[23] = same(buf, 100, 4, 1000);
    r[24] = count_files(&r[25]);                        /* 6, 143361 bytes */
    fill(buf, 3000, 99, 0);                             /* replace s4 whole */
    r[26] = fs_write("s4", buf, 3000);
    memset(buf, 0, 3000);
    r[27] = fs_read("s4", 0, buf, FILE_MAX);            /* 3000 */
    r[28] = same(buf, 3000, 99, 0);
    r[29] = fs_write("tmp", "x", 1);                    /* delete */
    r[30] = fs_delete("tmp");
    r[31] = fs_read("tmp", 0, buf, 1);                  /* -2 */
    r[32] = fs_delete("tmp");                           /* -2 */
    long e = 0, i = 0;                                  /* fill the disk */
    for (; i < 20; ++i) {
        numbered(n, 'b', i);
        fill(buf, FILE_MAX, 50 + (unsigned long)i, 0);
        if ((e = fs_write(n, buf, FILE_MAX)) != 0) break;
    }
    r[33] = i;                                          /* 6 */
    r[34] = e;                                          /* -28 */
    long del = 0;
    for (long j = 0; j < i; ++j) {
        numbered(n, 'b', j);
        del += fs_delete(n) == 0;
    }
    r[35] = del;                                        /* 6 */
    for (i = 0; i < 60; ++i) {                          /* 57 empty files: 63 in all */
        numbered(n, 'z', i);
        if ((e = fs_write(n, buf, 0)) != 0) break;
    }
    r[36] = i;                                          /* 57 */
    r[37] = e;                                          /* -28: the 64th file */
    char name[16];
    r[38] = fs_list(63, name);                          /* -2 */
    r[39] = fs_list(62, name);                          /* 0: z56 is empty */
    del = 0;
    for (long j = 0; j < i; ++j) {
        numbered(n, 'z', j);
        del += fs_delete(n) == 0;
    }
    r[40] = del;                                        /* 57 */
    r[41] = count_files(&r[42]);                        /* 6 files, 0+1+4095+4096+3000+131072 */
    DONE();
}
