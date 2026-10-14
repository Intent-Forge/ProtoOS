# ProtoOS v1.6 — specification addendum

Owner decision (2026-10-13, "Утверждаю v1.6", after reviewing the plan and this specification part by part). Status: **in implementation** on branch v1.6-dev; v1.5 is released first.

The v1.0 documents and the addenda `SPEC_V1_1.md` … `SPEC_V1_5.md` stay unchanged, and **every v1.0–v1.5 requirement and check (T1–T27, N1–N15) still applies to v1.6**. The only exception is the boot argument of five v1.5 checks (D2, below). The disk format of v1.5 is unchanged: a v1.5 `disk.img` works with v1.6, and the other way round.

## Owner decisions
- **D1.** New calls get ProtoOS numbers: `open` 1018, `close` 1019, `lseek` 1020. `read` (63) and `write` (64) are extended to file descriptors.
- **D2.** `protoos.run=<program> [argument]` replaces the seven v1.5 `SCENARIO_AUTORUN` boot arguments, with no aliases.
- **D3.** The shell in C moves to v1.7, together with pipes.
- **D4.** `run` (1008) stays forever, as `run_arg` with an empty argument.

## File descriptors

### Model
- Descriptors 0 and 1 are the console, exactly as in v1.4. Descriptors 3–6 are files; 2 is never used. A process has at most **4 open files**.
- **Read mode** (`flags` 0). `open` reads the whole file and checks its CRC, as `fs_read` does. It keeps a **snapshot** in kernel pages that belong to this descriptor:
  - `read` and `lseek` work on the snapshot;
  - a later change of the file on disk, by this or another process, does not affect it;
  - a damaged file fails at `open` with −5, never in the middle of reading.
- **Write mode** (`flags` 1). `open` creates an empty buffer; `write` appends to it.
  - `close` replaces the file whole, through exactly the path of `fs_write` in v1.5: data → flush → the other directory copy with seq + 1 → flush.
  - `close` returns 0 only once the new directory is durable.
  - Before `close`, nothing on the disk changes.
- **A process that ends** (exit, fault, kill, Ctrl-C) with files open: read snapshots are freed, write buffers are **discarded** without writing, and the disk keeps the old version. This extends the v1.5 crash rule — every file is entirely old or entirely new — to processes that die.
- No append, no editing in place, no shared descriptors between processes, no `dup`.

### Memory
- Snapshot and buffer pages come from the page pool. They are kernel-only (U=0): the process never maps them.
- They **count against the process's 64-page limit**, together with its tables, segments, stack and heap. A 128 KiB file needs 32 pages.
- If the limit or the pool would be exceeded:
  - `open` for reading gives −12;
  - a `write` that would need more pages than the limit or the pool allow gives −12 and appends nothing (all or nothing); the buffer keeps what it had.
- Every page comes back at `close` or when the process ends, whichever is first. N9 and T15 keep checking the totals.

### System calls
Arguments are checked first (−22, −14), then the descriptor (−9), then the disk (−19 without a mounted disk).

| Call | `a7` | Behaviour |
|---|---|---|
| `open(name, namelen, flags)` | 1018 | only for programs from the archive or the disk (like `sbrk`; the built-in processes get −22). flags 0 read, 1 write; anything else −22. Returns a descriptor 3–6. Errors: −22 name or flags, −14, −19, −24 four files already open, −2 read of a missing file, −5 damaged file or device failed, −12 no page in the limit, −30 write mode on a read-only disk |
| `close(fd)` | 1019 | read mode: frees the snapshot, 0. Write mode: saves the file (as `fs_write`), frees the buffer; 0 once durable. Errors: −9 not an open file descriptor, and in write mode −28, −5. After an error the descriptor is closed anyway, and the old file stays |
| `read(fd, buf, len)` | 63 | fd 0 unchanged (v1.2). fd 3–6 in read mode: copies up to `len` bytes from the current position, advances it; 0 at the end. `len` ≤ 128 KiB, else −22; −14; −9 for a write-mode descriptor or one not open |
| `write(fd, buf, len)` | 64 | fd 1 unchanged (v1.1). fd 3–6 in write mode: appends; the file stays at most 128 KiB (−27 beyond). −14; −9 for a read-mode descriptor or one not open; −12 |
| `lseek(fd, off, whence)` | 1020 | read mode only. whence 0 from the start, 1 from the current position, 2 from the end. The new position must be in 0 … size, else −22. Returns the new position; −9 for write mode or not open |

`fs_list`, `fs_read`, `fs_write`, `fs_delete`, `args` and `run_arg` (1012–1017) are unchanged. New error number: −24 EMFILE (the Linux number, same meaning). −9 EBADF already exists.

### Order of operations
Two processes can work with one file at the same time:
- a reader's snapshot is fixed at its `open`;
- two writers each replace the file whole at their own `close`, and the later `close` wins.

Each call still runs whole in the kernel with interrupts off.

## `protoos.run`
- `protoos.run=<name>` or `protoos.run=<name> <argument>` (the argument runs to the end of `bootargs`, at most 55 bytes) starts the program through the path of `run_arg`: archive first, then disk.
- Process A is the shell, B the worker, as in the v1.5 autorun scenarios. The harness observes the program's `exit` as before.
- Removed: `protoos.test=fs`, `fs-ro`, `persist-w`, `persist-r`, `crash`, `fs-bad`, `no-disk`.
- In the acceptance matrix T24, T25, T27, N14 and N15 use `protoos.run=t_fs` and so on. Their criteria do not change.
- The boot line is `AUTORUN <name> pid=<pid>`, as in v1.5. An unknown or refused program gives `AUTORUN <name> pid=-N` with its error, and the system runs on as the shell. A name over 15 characters, a space with nothing after it, or an argument over 55 bytes is an unknown `bootargs` (`KERNEL_PANIC unknown bootargs`, as for any other).
- The v1.0–v1.4 scenarios (`umode`, `preempt`, `elf`, `sbrk`, …) are unchanged: frozen v1.0 checks and built-in assembly drivers.

## Fuzzer
- `tests.img` adds `fuzz <seed> <calls>`. It makes random system calls 0…1100, including random 64-bit numbers, with arguments from a hostile pool:
  - kernel, device and other processes' addresses, guard pages;
  - 0, −1, huge lengths, page-boundary lengths;
  - valid and invalid pids, names and descriptors.
- Calls that may block forever by design are tamed:
  - `sleep` ≤ 10 ms;
  - `read(0)` with length 0;
  - `wait` only on ended processes or invalid pids.
- Every child it starts is killed at the end.
- The harness judges from outside (GDB, QEMU trace, UART), never from the fuzzer's own report.

## Additional acceptance checks (3 clean boots each)
| ID | Scenario | PASS criterion | Independent evidence |
|---|---|---|---|
| T28 | `protoos.run=t_fd`, `tests.img`, blank disk | files of 0, 1, 4095, 4096, 4097 bytes and 128 KiB written through `write` in pieces of 1, 7, 4096 and 65536 bytes and read back through `read` in other pieces with `lseek` 0/1/2; the image holds exactly the expected files (pfsck, SHA-256). A snapshot keeps the old content while another process replaces the file. A write-mode file is unchanged on disk until `close`. The fifth `open` −24. 128 KiB snapshots up to the 64-page limit, then −12. After exit: every page back | results read at `exit`, GDB allocator counters, pfsck |
| T29 | `protoos.run=t_fdcrash`: a child opens a file for writing, writes, and is (a) killed, (b) faulting, (c) ended by `exit` without `close`, (d) closed; plus 20 QEMU kills at random moments around `close` | (a)–(c): the old file, byte for byte, and every page back; (d): the new file. After every QEMU kill: pfsck mounts, the file is entirely old or entirely new; after `close` = 0 always new | results, GDB, pfsck, seed in the evidence |
| N16 | `protoos.run=t_fdbad` | descriptors −1, 0 for write, 1 for read, 2, 7, 1000, another process's descriptor, a closed one, double `close` → −9; flags 2 and −1 → −22; `lseek` before 0 and past the end → −22; kernel pointers for name and buffer → −14; `write` past 128 KiB → −27; nothing on the disk changes, no page leaked | results, GDB, pfsck, image SHA-256 |
| N17 | `protoos.run=fuzz` with seeds 1, 2, 3 × 20 000 calls, `programs.img`, a copy of `disk.img` | no S-mode exception other than the expected kinds (QEMU `-d int`), no stop of progress for 5 s, A and B alive, every page back after the fuzzer ends, pfsck accepts the disk, every unknown number returned −38 | QEMU trace, GDB, pfsck, UART |

v1.6 is accepted when T1–T29 and N1–N17 all PASS in 3 clean boots each.

- **Host tests:** the descriptor logic (positions, bounds, the page accounting) as pure code, plain and with ASan/UBSan, like PFS in v1.5.
- **Nightly:** the fuzzer with new seeds every night on the mini-PC; a failing seed is reported with the command that repeats it.
- **Mutants**, each must fail its check:

| Bug put in | Check |
|---|---|
| `close` writes the directory before the data | T29, host |
| a write buffer saved when its process is killed | T29 |
| no snapshot (`read` goes to the disk) | T28 |
| descriptor pages outside the 64-page limit | T28 |
| another process's descriptor accepted | N16 |
| `lseek` without a bounds check | N16 |
| a page lost at `close` | T28 |
| the harness ignoring an S-mode exception | N17 with a kernel that faults on call 1100 |

## Honest limits
- The fuzzer finds what random search reaches. It is not a proof.
- Snapshots make reading a large file cost its whole size in memory. There is no streaming read in v1.6.
- Write mode always replaces the whole file. Append and editing in place stay out of scope (v1.8 plans atomic editing).
- Unchanged from v1.5: QEMU only, no real disks.

## Owner answers (2026-10-13)
1. At most **4 open files** per process in v1.6; v1.7 decides again together with pipes.
2. The modes stay separate: `read` on a write-mode descriptor (and `write` on a read-mode one) gives −9.
