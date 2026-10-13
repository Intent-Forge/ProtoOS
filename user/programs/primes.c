/* primes: the sieve of Eratosthenes on memory taken from the heap. */
#include "proto.h"

#define LIMIT 10000u

int main(void)
{
    char *sieve = malloc(LIMIT);
    if (!sieve) {
        print("primes: the kernel gave no memory\n");
        return 1;
    }
    sieve[0] = sieve[1] = 0;
    for (unsigned i = 2; i < LIMIT; ++i) sieve[i] = 1;
    for (unsigned p = 2; p * p < LIMIT; ++p)
        if (sieve[p])
            for (unsigned m = p * p; m < LIMIT; m += p) sieve[m] = 0;
    print("Primes below 100:");
    for (unsigned i = 0; i < 100; ++i) {
        if (!sieve[i]) continue;
        print(" ");
        print_dec(i);
    }
    unsigned count = 0;
    for (unsigned i = 0; i < LIMIT; ++i) count += sieve[i] ? 1u : 0u;
    print("\nThere are ");
    print_dec(count);
    print(" primes below 10000 (sieve of 10000 bytes on the heap).\n");
    return 0;
}
