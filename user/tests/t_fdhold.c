/* N16 helper (v1.6): opens "held" for reading (descriptor 3) and keeps it
 * open until it is killed, so another process can try descriptor 3. */
#include "ptest.h"

int main(void)
{
    r[0] = fd_open("held", FD_READ);
    for (;;) sys_sleep(1000);
}
