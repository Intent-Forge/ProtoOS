/* T29 part 1 (v1.6): a process that ends with a file open for writing, by
 * kill, fault or exit without close, leaves the old file whole and gives
 * every page back; one that closes leaves the new file. */
#include "ptest.h"

static unsigned char buf[5000];

/* 1 if "x" holds exactly n bytes of tag */
static long x_is(unsigned long n, unsigned long tag)
{
    return fs_read("x", 0, buf, sizeof(buf)) == (long)n && same(buf, n, tag, 0);
}

int main(void)
{
    static const char *const how[4] = { "kill", "fault", "exit", "close" };
    r[0] = sys_meminfo(0);
    fill(buf, 3000, 1, 0);
    r[1] = fs_write("x", buf, 3000);                    /* 0: the old file, tag 1 */
    for (int k = 0; k < 4; ++k) {                       /* r[2+4k] .. r[5+4k] */
        long pid = sys_run_arg("t_fdw", how[k]);
        r[2 + 4 * k] = pid > 0;                         /* 1 */
        if (k == 0) {
            sys_sleep(100);                             /* it has written by now */
            sys_kill(pid);
        }
        r[3 + 4 * k] = sys_wait(pid, 0);                /* 137, 139, 0, 0 */
        r[4 + 4 * k] = k < 3 ? x_is(3000, 1) : x_is(5000, 2);  /* 1 */
        r[5 + 4 * k] = sys_meminfo(0) == r[0];          /* 1: every page back */
    }
    DONE();
}
