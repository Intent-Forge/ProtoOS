/* Host tests for src/fdmath.h (v1.6 file descriptors), plain and with
 * ASan/UBSan: page counts, lseek bounds, read counts, append bounds,
 * exhaustively over small sizes and at the edges of 64-bit arithmetic. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/fdmath.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int main(void)
{
    /* pages */
    CHECK(fd_pages_for(0) == 0 && fd_pages_for(1) == 1 && fd_pages_for(4095) == 1);
    CHECK(fd_pages_for(4096) == 1 && fd_pages_for(4097) == 2 && fd_pages_for(131072) == 32);
    CHECK(fd_pages_for(UINT64_MAX) == UINT64_MAX / 4096 + 1);
    /* lseek: against a reference over small sizes, positions and offsets */
    long seeks = 0;
    for (uint64_t size = 0; size <= 40; ++size)
        for (uint64_t pos = 0; pos <= size; ++pos)
            for (int64_t off = -50; off <= 50; ++off)
                for (uint64_t wh = 0; wh <= 3; ++wh) {
                    int64_t base = wh == 0 ? 0 : wh == 1 ? (int64_t)pos : (int64_t)size;
                    int64_t want = wh > 2 ? -1 : base + off;
                    if (want < 0 || want > (int64_t)size) want = -1;
                    CHECK(fd_seek(size, pos, off, wh) == want);
                    ++seeks;
                }
    /* lseek: 64-bit edges never wrap */
    CHECK(fd_seek(100, 50, INT64_MIN, 1) == -1);
    CHECK(fd_seek(100, 50, INT64_MAX, 1) == -1);
    CHECK(fd_seek(100, 50, INT64_MAX, 0) == -1);
    CHECK(fd_seek(131072, 0, 131072, 0) == 131072 && fd_seek(131072, 0, 131073, 0) == -1);
    CHECK(fd_seek(10, 0, -10, 2) == 0 && fd_seek(10, 0, -11, 2) == -1);
    CHECK(fd_seek(10, 20, 0, 1) == -1);                /* a position past the end is refused */
    /* read counts */
    for (uint64_t size = 0; size <= 40; ++size)
        for (uint64_t pos = 0; pos <= 45; ++pos)
            for (uint64_t len = 0; len <= 45; ++len) {
                uint64_t want = pos >= size ? 0 : (size - pos < len ? size - pos : len);
                CHECK(fd_read_count(size, pos, len) == want);
            }
    CHECK(fd_read_count(10, 0, UINT64_MAX) == 10 && fd_read_count(10, UINT64_MAX, 5) == 0);
    /* append bounds */
    CHECK(fd_write_fits(0, 131072, 131072) && !fd_write_fits(0, 131073, 131072));
    CHECK(fd_write_fits(131071, 1, 131072) && !fd_write_fits(131071, 2, 131072));
    CHECK(!fd_write_fits(131072, 1, 131072) && fd_write_fits(131072, 0, 131072));
    CHECK(!fd_write_fits(1, UINT64_MAX, 131072) && !fd_write_fits(UINT64_MAX, 1, 131072));
    CHECK(!fd_write_fits(131073, 0, 131072));
    printf("fdmath: %ld lseek cases, read counts, append bounds: ok\nFDMATH_TESTS_PASS\n", seeks);
    return 0;
}
