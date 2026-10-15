/* N18 boot 3 (v1.6): a read error. Booted on a copy of disk.img whose
 * reads of block 3 (the first block of hello) fail (QEMU blkdebug, EIO):
 * the mount does not read it; the first read of hello fails the device and
 * stops the file system, so every later file call gives EIO; the archive
 * still works and nothing is written. */
#include "ptest.h"

static unsigned char buf[20000];

int main(void)
{
    char name[16];
    r[0] = sys_meminfo(0);
    r[1] = fs_list(0, name);                            /* 15496: the directory is in memory */
    r[2] = fs_read("hello", 0, buf, sizeof(buf));       /* -5: block 3 cannot be read */
    r[3] = fs_list(0, name);                            /* -5 from here on */
    r[4] = fs_read("hello", 0, buf, sizeof(buf));       /* -5 */
    r[5] = fs_write("c", buf, 10);                      /* -5 */
    r[6] = fd_open("hello", FD_READ);                   /* -5 */
    r[7] = sys_run("hello");                            /* -5 */
    long pid = sys_run("t_exit42");
    r[8] = pid > 0;                                     /* 1 */
    r[9] = sys_wait(pid, 0);                            /* 42 */
    r[10] = sys_meminfo(0) == r[0];                     /* 1 */
    DONE();
}
