# ProtoOS v1.1 — specification addendum

Owner decision (2026-10-12): extend the frozen v1.0 specification with two system calls and a demonstration mode. The v1.0 documents ([`TECHNICAL_SPECIFICATION_RU.md`](../TECHNICAL_SPECIFICATION_RU.md), [`DECISION_REGISTER_RU.md`](../DECISION_REGISTER_RU.md), [`ACCEPTANCE_MATRIX.csv`](../ACCEPTANCE_MATRIX.csv)) stay unchanged and **every v1.0 requirement and check (T1–T7, N1–N5) still applies to v1.1**.

## Additions
### `write(fd, buf, len)` — `a7 = 64`
- `fd` must be 1 (the console); otherwise `-9` (EBADF).
- `len` ≤ 256; otherwise `-22` (EINVAL). `len = 0` returns 0.
- Every byte of `[buf, buf + len)` must lie in pages of the **calling process's own** Sv39 root that are valid, user-accessible (U) and readable (R). The kernel checks this with its own page-table walk **before** touching memory; otherwise `-14` (EFAULT) and nothing is written. This covers kernel pages (U=0), the other process's pages (unmapped), guard pages, address overflow and non-canonical addresses.
- The copy runs with `sstatus.SUM` set only for its duration; on return to U-mode `SUM` is 0.
- Bytes are written to the UART; `\n` becomes CR LF; bytes outside printable ASCII (other than `\n`, `\t`) are replaced by `?`. Returns `len`. One call is written atomically (the kernel is not preemptible).

### `sleep(ms)` — `a7 = 1001` (ProtoOS-specific number)
- `ms` ≤ 60 000; otherwise `-22`. `ms = 0` behaves like `yield`. Returns 0.
- The process enters the new state **SLEEPING** and is not scheduled until `ms` milliseconds of `time` have elapsed; the wake-up is noticed at the next scheduling point (timer quantum 10 ms), so the delay is at least `ms` and normally at most `ms` + one quantum.
- If every remaining process is SLEEPING, the kernel waits in S-mode (`wfi` with the timer armed for the earliest wake-up, `sstatus.SIE` still 0) and then resumes scheduling. This is not the terminal idle state: `sched_idle` is entered only when no process is READY, RUNNING or SLEEPING.

### Demonstration mode — `bootargs = protoos.mode=demo`
A prints a numbered line every 400 ms, B every 700 ms (both via `write` and `sleep`); after four lines B announces and performs a write into A's memory, takes a store page fault and is marked FAULTED, while A keeps printing. Normal mode (`protoos.mode=normal`) is unchanged from v1.0.

## Additional acceptance checks (3 clean boots each)
| ID | Scenario (`bootargs`) | PASS criterion | Independent evidence |
|---|---|---|---|
| T8 | `protoos.test=write` | A's two `write` calls trap with `a7=64`, `a0=1`, a buffer the PT walk shows as U+R in A's root; each returns `len`; the exact message appears exactly twice on the UART; `SUM=0` back in U-mode; A continues | GDB trap frame, PT walk, CSR `sstatus`, UART bytes, QEMU trace (exactly two `user_ecall` from A) |
| T9 | `protoos.test=sleep`, `protoos.test=sleep-all` | A sleeping 200 ms is SLEEPING and not dispatched while B runs; A resumes after ≥ 200 ms and ≤ 200 ms + 2 quanta with `a0=0`; when both sleep, the kernel waits (no idle, no fault) and both keep making progress | GDB process state and dispatch counts, kernel `time` stamps, physical counters |
| N6 | `protoos.test=write-bad` | wrong `fd` → −9; kernel page, other process's page, page-crossing into a guard page, NULL and overflowing address → −14; `len > 256` → −22; nothing is printed for rejected calls; A is not faulted and a following valid `write` succeeds | per-call results in A's physical memory, UART between markers, process states |

v1.1 is accepted when T1–T9 and N1–N6 all PASS in 3 clean boots each.
