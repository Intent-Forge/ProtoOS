/* guess: the computer picks a number from 1 to 100, you guess it. */
#include "proto.h"

int main(void)
{
    long secret = (long)(sys_uptime() % 100u) + 1;
    char line[32];
    print("I picked a number from 1 to 100. Guess it!\n");
    for (unsigned long tries = 1;;) {
        long v;
        print("Your guess: ");
        read_line(line, sizeof line);
        if (!to_int(line, &v) || v < 1 || v > 100) {
            print("Please type a number from 1 to 100.\n");
            continue;
        }
        if (v < secret) {
            print("Too small.\n");
        } else if (v > secret) {
            print("Too big.\n");
        } else {
            print("Correct! The number was ");
            print_dec((unsigned long)secret);
            print(". Tries: ");
            print_dec(tries);
            print("\n");
            return 0;
        }
        ++tries;
    }
}
