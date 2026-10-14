/* N16 (v1.6): descriptors misused. Every call gets its exact error, nothing
 * on the disk changes except the two files written on purpose, and no page
 * is leaked. Booted on an empty 1 MiB disk. */
#include "ptest.h"

#define KPTR 0x80200000L                /* kernel text: never a user buffer */
static unsigned char buf[4096];

int main(void)
{
    r[0] = sys_meminfo(0);
    fill(buf, 100, 3, 0);
    r[1] = fs_write("held", buf, 100);                          /* 0 */

    /* descriptors that are not open files */
    r[2] = fd_read(-1, buf, 10);                                /* -9 */
    r[3] = proto_call(64, 0, (long)buf, 1);                     /* write to fd 0: -9 */
    r[4] = proto_call(63, 1, (long)buf, 1);                     /* read from fd 1: -9 */
    r[5] = fd_read(2, buf, 10);                                 /* -9 */
    r[6] = fd_read(7, buf, 10);                                 /* -9 */
    r[7] = fd_write(1000, buf, 10);                             /* -9 */
    r[8] = fd_close(0) == -9 && fd_close(1) == -9 && fd_close(2) == -9 && fd_close(7) == -9; /* 1 */
    r[9] = fd_seek(3, 0, 0);                                    /* -9: nothing open */

    /* another process's descriptor: t_fdhold has "held" open as its 3 */
    long holder = sys_run_arg("t_fdhold", "");
    r[10] = holder > 0;                                         /* 1 */
    sys_sleep(50);                                              /* it runs and opens */
    r[11] = fd_read(3, buf, 10);                                /* -9: not ours */
    r[12] = fd_close(3);                                        /* -9 */
    r[13] = sys_kill(holder);                                   /* 0: its snapshot goes back */

    /* a closed descriptor, twice closed */
    long fd = fd_open("held", FD_READ);
    r[14] = fd;                                                 /* 3 */
    r[15] = fd_close(fd);                                       /* 0 */
    r[16] = fd_read(fd, buf, 10);                               /* -9 */
    r[17] = fd_close(fd);                                       /* -9 */

    /* the wrong mode */
    fd = fd_open("held", FD_READ);
    r[18] = fd_write(fd, buf, 1);                               /* -9: read mode */
    r[19] = fd_close(fd);                                       /* 0 */
    long w = fd_open("big", FD_WRITE);
    r[20] = fd_read(w, buf, 1);                                 /* -9: write mode */
    r[21] = fd_seek(w, 0, 0);                                   /* -9: write mode */

    /* bad arguments come before the descriptor */
    r[22] = proto_call(1018, (long)"held", 4, 2);               /* flags 2: -22 */
    r[23] = proto_call(1018, (long)"held", 4, -1);              /* flags -1: -22 */
    r[24] = proto_call(1018, (long)"Held", 4, 0);               /* bad name: -22 */
    r[25] = proto_call(1018, (long)"held", 0, 0);               /* length 0: -22 */
    r[26] = proto_call(1018, (long)"abcdefghijklmnop", 16, 0);  /* 16 characters: -22 */
    r[27] = fd_seek(99, 0, 3);                                  /* whence 3 first: -22 */
    r[28] = fd_open("nosuch", FD_READ);                         /* -2 */

    /* kernel pointers */
    r[29] = proto_call(1018, KPTR, 4, 0);                       /* -14 */
    r[30] = fd_write(w, (const void *)KPTR, 16);                /* -14 */
    fd = fd_open("held", FD_READ);
    r[31] = fd_read(fd, (void *)KPTR, 16);                      /* -14 */
    r[32] = fd_read(fd, buf, 200);                              /* 100: the descriptor still works */
    r[33] = fd_close(fd);                                       /* 0 */

    /* past 128 KiB */
    long ok = 1;
    for (int k = 0; k < 32; ++k) {
        fill(buf, 4096, 4, (unsigned long)k * 4096);
        ok &= fd_write(w, buf, 4096) == 4096;
    }
    r[34] = ok;                                                 /* 1: exactly 128 KiB */
    r[35] = fd_write(w, buf, 1);                                /* -27 */
    r[36] = fd_close(w);                                        /* 0: big saved, 128 KiB */
    r[37] = count_files(&r[38]);                                /* 2 files, 131172 bytes */
    r[39] = sys_meminfo(0);                                     /* = r[0] */
    DONE();
}
