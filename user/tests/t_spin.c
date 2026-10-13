/* Counts forever without system calls: only kill() or Ctrl-C end it. */
#include "proto.h"

volatile unsigned long t_spin_counter;

int main(void)
{
    for (;;) t_spin_counter++;
}
