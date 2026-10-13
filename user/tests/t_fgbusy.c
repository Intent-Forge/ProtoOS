/* While another process waits in the foreground, a second foreground
 * wait() is refused with -16. Exit code 16 if so (its helper is killed).
 * The first sleep lets the process that started it reach its own
 * foreground wait first. */
#include "proto.h"

int main(void)
{
    sys_sleep(200);
    long pid = sys_run("t_spin");
    if (pid < 0) return 1;
    long r = sys_wait(pid, WAIT_FOREGROUND);
    if (sys_kill(pid) != 0) return 2;
    if (sys_wait(pid, 0) != 137) return 3;
    return r == E_BUSY ? 16 : 4;
}
