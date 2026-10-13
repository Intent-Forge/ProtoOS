/* Sleeps 300 ms, then exits with code 7 (a wait() that really blocks). */
#include "proto.h"

int main(void)
{
    sys_sleep(300);
    print("t_sleepy: exiting with 7\n");
    return 7;
}
