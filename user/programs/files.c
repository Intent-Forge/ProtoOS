/* files: every file on the disk with its size. */
#include "proto.h"

int main(void)
{
    char name[17];
    unsigned long n = 0;
    for (;;) {
        long size = fs_list(n, name);
        if (size < 0) {
            if (size != E_NOENT) {
                print_error("files", size);
                return 1;
            }
            break;
        }
        name[16] = 0;
        print(name);
        print("  ");
        print_dec((unsigned long)size);
        print(" bytes\n");
        ++n;
    }
    if (n == 0) print("files: the disk is empty\n");
    return 0;
}
