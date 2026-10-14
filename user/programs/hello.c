/* hello [words]: the demo program on the released disk image (disk.img),
 * not in the program archive: "run hello" starts it from the disk. */
#include "proto.h"

int main(void)
{
    char arg[ARG_MAX + 1];
    long n = get_arg(arg, sizeof(arg));
    print("Hello from the disk!");
    if (n > 0) {
        print(" Your argument: ");
        print(arg);
    }
    print("\n");
    return 0;
}
