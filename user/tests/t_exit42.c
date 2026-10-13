/* Exits with code 42 right away (wait() tests; the valid control program
 * the malformed files of badelf.img are made from). */
#include "proto.h"

int main(void)
{
    print("t_exit42: exiting with 42\n");
    return 42;
}
