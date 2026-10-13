/* N15: without a usable disk (none, or a legacy device that was refused)
 * every fs call gives ENODEV and programs still run from the archive. */
#include "ptest.h"

int main(void)
{
    char name[16], arg[8];
    r[0] = fs_list(0, name);                    /* -19 */
    r[1] = fs_read("a", 0, name, 10);           /* -19 */
    r[2] = fs_write("a", name, 1);              /* -19 */
    r[3] = fs_delete("a");                      /* -19 */
    r[4] = sys_run("nosuch");                   /* -2 */
    r[5] = get_arg(arg, sizeof(arg));           /* 0 */
    long pid = sys_run("t_exit42");
    r[6] = pid > 0;
    r[7] = sys_wait(pid, 0);                    /* 42 */
    DONE();
}
