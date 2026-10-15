/* N18 boot 1 (v1.6): the disk fails in the middle of a change. Booted on a
 * copy of disk.img (hello; copy A mounted, seq 1) whose writes to directory
 * copy A fail (QEMU blkdebug, EIO): the first change (to copy B) succeeds,
 * the second fails when its directory is written. From then on every file
 * call gives EIO, a program from the archive still runs, a snapshot taken
 * before still reads, and every page comes back. Boot 2 is t_diska. */
#include "ptest.h"

static unsigned char buf[20000];

int main(void)
{
    char name[16];
    r[0] = sys_meminfo(0);
    fill(buf, 3000, 1, 0);
    r[1] = fs_write("a", buf, 3000);                    /* 0: copy B, seq 2 */
    r[2] = fd_open("a", FD_READ);                       /* 3: a snapshot before the failure */
    r[3] = fd_open("b", FD_WRITE);                      /* 4 */
    fill(buf, 20000, 2, 0);
    r[4] = fd_write(4, buf, 20000);                     /* 20000 */
    r[5] = fd_close(4);                                 /* -5: copy A cannot be written */
    r[6] = fs_list(0, name);                            /* -5 from here on */
    r[7] = fs_read("a", 0, buf, sizeof(buf));           /* -5 */
    r[8] = fs_read("none", 0, buf, sizeof(buf));        /* -5, not -2 */
    r[9] = fs_write("c", buf, 10);                      /* -5 */
    r[10] = fs_delete("a");                             /* -5 */
    r[11] = fd_open("a", FD_READ);                      /* -5 */
    r[12] = fd_open("d", FD_WRITE);                     /* -5 */
    r[13] = sys_run("hello");                           /* -5: a program from the disk */
    long pid = sys_run("t_exit42");                     /* from the archive: still runs */
    r[14] = pid > 0;                                    /* 1 */
    r[15] = sys_wait(pid, 0);                           /* 42 */
    r[16] = fd_read(3, buf, sizeof(buf));               /* 3000: the snapshot is in memory */
    r[17] = same(buf, 3000, 1, 0);                      /* 1 */
    r[18] = fd_close(3);                                /* 0 */
    r[19] = sys_meminfo(0) == r[0];                     /* 1: every page back */
    DONE();
}
