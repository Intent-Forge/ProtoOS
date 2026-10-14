/* T25, boot 1: write, replace and delete; boot 2 (t_pr) reads it back. */
#include "ptest.h"

int main(void)
{
    unsigned char *buf = malloc(FILE_MAX);
    if (!buf) return 1;
    fill(buf, 100, 1, 0);
    r[0] = fs_write("p1", buf, 100);
    fill(buf, 5000, 2, 0);
    r[1] = fs_write("p2", buf, 5000);
    fill(buf, FILE_MAX, 3, 0);
    r[2] = fs_write("p3", buf, FILE_MAX);
    fill(buf, 200, 4, 0);
    r[3] = fs_write("p1", buf, 200);                    /* replaced whole */
    r[4] = fs_delete("p2");
    r[5] = count_files(0);                              /* 2 */
    DONE();
}
