/* T29 helper (v1.6): opens "x" for writing, writes 5000 new bytes (tag 2)
 * in pieces, then by its argument: "kill" waits to be killed, "fault"
 * faults, "exit" exits without close, "close" saves the file. */
#include "ptest.h"

static unsigned char buf[5000];

int main(void)
{
    char how[ARG_MAX + 1];
    get_arg(how, sizeof(how));
    long fd = fd_open("x", FD_WRITE);
    if (fd < 0) return 2;
    fill(buf, 5000, 2, 0);
    for (unsigned long off = 0; off < 5000; off += 1000)
        if (fd_write(fd, buf + off, 1000) != 1000) return 3;
    if (!strcmp(how, "kill"))
        for (;;) sys_sleep(1000);
    if (!strcmp(how, "fault"))
        *(volatile long *)0 = 1;
    if (!strcmp(how, "exit"))
        return 0;
    return fd_close(fd) ? 4 : 0;
}
