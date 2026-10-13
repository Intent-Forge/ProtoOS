/* Reads lines and answers each one, forever: Ctrl-C must end it while it
 * is blocked in read(). */
#include "proto.h"

int main(void)
{
    char line[64];
    for (;;) {
        print("t_reader> ");
        read_line(line, sizeof line);
        print("got: ");
        print(line);
        print("\n");
    }
}
