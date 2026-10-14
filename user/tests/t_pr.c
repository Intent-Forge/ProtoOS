/* T25, boot 2 on the disk image boot 1 (t_pw) left behind. */
#include "ptest.h"

int main(void)
{
    unsigned char *buf = malloc(FILE_MAX);
    if (!buf) return 1;
    long total = 0;
    r[0] = count_files(&total);                         /* 2 */
    r[1] = total;                                       /* 200 + 131072 */
    r[2] = fs_read("p1", 0, buf, FILE_MAX);             /* 200 */
    r[3] = same(buf, 200, 4, 0);
    r[4] = fs_read("p3", 0, buf, FILE_MAX);             /* 131072 */
    r[5] = same(buf, FILE_MAX, 3, 0);
    r[6] = fs_read("p2", 0, buf, FILE_MAX);             /* -2 */
    DONE();
}
