/* clock: the time since boot, once a second, until Ctrl-C. */
#include "proto.h"

int main(void)
{
    print("clock: time since boot every second. Ctrl-C stops it.\n");
    for (;;) {
        unsigned long us = sys_uptime();
        unsigned long cs = us / 10000u % 100u;
        print("uptime ");
        print_dec(us / 1000000u);
        print(cs < 10 ? ".0" : ".");
        print_dec(cs);
        print(" s\n");
        sys_sleep(1000);
    }
}
