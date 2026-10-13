/* rm <file>: delete a file from the disk. */
#include "proto.h"

int main(void)
{
    char name[ARG_MAX + 1];
    if (get_arg(name, sizeof(name)) <= 0) {
        print("usage: run rm <file>\n");
        return 2;
    }
    long r = fs_delete(name);
    if (r < 0) {
        print_error("rm", r);
        return 1;
    }
    print("removed ");
    print(name);
    print("\n");
    return 0;
}
