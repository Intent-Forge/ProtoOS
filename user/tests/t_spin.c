/* Counts forever without system calls: only kill() or Ctrl-C end it.
 * t_spin_step() gives the harness a point where it is surely running. */
#include "proto.h"

volatile unsigned long t_spin_counter;

__attribute__((noinline)) void t_spin_step(void)
{
    t_spin_counter++;
}

int main(void)
{
    for (;;) t_spin_step();
}
