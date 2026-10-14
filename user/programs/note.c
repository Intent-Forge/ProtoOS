/* note <file>: type lines; an empty line saves them as the file (replacing
 * it whole if it exists). Nothing is saved before that. */
#include "proto.h"

int main(void)
{
    char name[ARG_MAX + 1], line[128];
    if (get_arg(name, sizeof(name)) <= 0) {
        print("usage: run note <file>\n");
        return 2;
    }
    char *buf = malloc(FILE_MAX);
    if (!buf) {
        print_error("note", E_NOMEM);
        return 1;
    }
    unsigned long used = 0;
    print("note: type lines; an empty line saves.\n");
    for (;;) {
        long n = read_line(line, sizeof(line));
        if (n <= 0) break;
        if (used + (unsigned long)n + 1 > FILE_MAX) {
            print_error("note", E_FBIG);
            return 1;
        }
        memcpy(buf + used, line, (size_t)n);
        used += (unsigned long)n;
        buf[used++] = '\n';
    }
    /* An empty first line: nothing typed, so nothing is saved (an existing
     * file is not replaced by an empty one by mistake). */
    if (used == 0) {
        print("note: nothing typed, nothing saved\n");
        return 0;
    }
    long r = fs_write(name, buf, used);
    if (r < 0) {
        print_error("note", r);
        return 1;
    }
    print("saved ");
    print(name);
    print(": ");
    print_dec(used);
    print(" bytes\n");
    return 0;
}
