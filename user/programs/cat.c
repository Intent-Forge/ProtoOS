/* cat <file>: print a file from the disk. It is read in one call: every
 * fs_read checks the CRC of the whole file first. */
#include "proto.h"

int main(void)
{
    char name[ARG_MAX + 1];
    if (get_arg(name, sizeof(name)) <= 0) {
        print("usage: run cat <file>\n");
        return 2;
    }
    char *buf = malloc(FILE_MAX);
    if (!buf) {
        print_error("cat", E_NOMEM);
        return 1;
    }
    long n = fs_read(name, 0, buf, FILE_MAX);
    if (n < 0) {
        print_error("cat", n);
        return 1;
    }
    for (long off = 0; off < n;) {
        long chunk = n - off < 256 ? n - off : 256;
        sys_write(buf + off, (size_t)chunk);
        off += chunk;
    }
    if (n > 0 && buf[n - 1] != '\n') print("\n");
    return 0;
}
