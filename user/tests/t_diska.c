/* N18 boot 2 (v1.6): the same disk after the failure of boot 1, now healthy.
 * It holds exactly the state before the failed change: hello and "a"
 * (seq 2, copy B); "b" was never saved. The disk takes changes again. */
#include "ptest.h"

static unsigned char buf[20000];

int main(void)
{
    r[0] = count_files(0);                              /* 2 */
    r[1] = fs_read("a", 0, buf, sizeof(buf));           /* 3000 */
    r[2] = same(buf, 3000, 1, 0);                       /* 1 */
    r[3] = fs_read("b", 0, buf, sizeof(buf));           /* -2 */
    long pid = sys_run("hello");                        /* from the disk */
    r[4] = pid > 0;                                     /* 1 */
    r[5] = sys_wait(pid, 0);                            /* 0 */
    fill(buf, 10, 3, 0);
    r[6] = fs_write("c", buf, 10);                      /* 0: copy A, seq 3 */
    r[7] = count_files(0);                              /* 3 */
    DONE();
}
