/* T24, read-only part: booted on ro.img (file "ro.txt", 5000 bytes of
 * tag 5) as a disk without flush, or with readonly=on: reads work, every
 * change gives EROFS and changes nothing. */
#include "ptest.h"

int main(void)
{
    unsigned char *buf = malloc(FILE_MAX);
    if (!buf) return 1;
    char name[16];
    r[0] = fs_read("ro.txt", 0, buf, FILE_MAX);        /* 5000 */
    r[1] = same(buf, 5000, 5, 0);
    r[2] = fs_write("new", buf, 10);                    /* -30 */
    r[3] = fs_write("ro.txt", buf, 10);                 /* -30 */
    r[4] = fs_delete("ro.txt");                         /* -30 */
    r[5] = fs_list(0, name);                            /* 5000 */
    r[6] = fs_read("ro.txt", 0, buf, FILE_MAX);        /* 5000, unchanged */
    r[7] = same(buf, 5000, 5, 0);
    r[8] = count_files(0);                              /* 1 */
    DONE();
}
