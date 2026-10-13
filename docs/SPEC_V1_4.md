# ProtoOS v1.4 — specification addendum

Owner decision (2026-10-12): real programs. Programs are written in C, built separately from the kernel and handed to it at boot in a program archive; the kernel checks and loads them, gives them a heap, lets a process wait for another one, and Ctrl-C ends a program that runs in the foreground. The v1.0 documents ([`TECHNICAL_SPECIFICATION_RU.md`](../TECHNICAL_SPECIFICATION_RU.md), [`DECISION_REGISTER_RU.md`](../DECISION_REGISTER_RU.md), [`ACCEPTANCE_MATRIX.csv`](../ACCEPTANCE_MATRIX.csv)) and the addenda [`SPEC_V1_1.md`](SPEC_V1_1.md), [`SPEC_V1_2.md`](SPEC_V1_2.md) and [`SPEC_V1_3.md`](SPEC_V1_3.md) stay unchanged, and **every v1.0–v1.3.1 requirement and check (T1–T17, N1–N9) still applies to v1.4**. Booted without a program archive, v1.4 behaves exactly like v1.3.1.

## Additions
### Program archive
- The boot loader places the archive in RAM and names it in the device tree, `/chosen` `linux,initrd-start` and `linux,initrd-end` (one or two cells; QEMU: `-initrd <file>`). It must lie inside the page pool, otherwise it is ignored; inside the pool its pages are reserved whatever its size, so an archive over the size limit is reserved and then rejected.
- Format: cpio "newc" (magic `070701`, no CRC), as written by `build-scripts/mkimages.py` or `cpio -H newc`. Regular files only; names of 1–15 characters `a–z 0–9 _`, starting with a letter, all different; at most 32 files and 4 MiB; every header field 8 hex digits, check field 0; a `TRAILER!!!` entry, after it only zero bytes.
- The kernel checks the whole archive once at boot. Any defect rejects it as a whole: the boot log says `ARCHIVE … rejected code=-N` and the system runs without programs.
- Its pages are reserved in the page allocator for good: never handed out, not counted by `meminfo`, mapped kernel-only (U=0) like the rest of the pool.

### Program files
- Static ELF64 RISC-V executables (`ET_EXEC`), little endian, System V ABI, `e_flags` = 0 (soft-float, no compressed instructions), 1–16 program headers.
- 1–4 `PT_LOAD` segments, in address order, page-aligned and not overlapping, inside `[0x40000000, 0x40100000)`; each readable, with at most one of write or execute; file size ≤ memory size, file bytes inside the file. `PT_DYNAMIC`, `PT_INTERP`, `PT_SHLIB`, `PT_TLS` are refused; other types are ignored. The entry point is 4-byte aligned inside the file bytes of an executable segment. The segments occupy at most 57 pages.
- All of this is checked before anything is allocated (`src/elf.c`). A file that fails is not started.

### Address space of a program
| Process VA | Contents | Permissions |
|---|---|---|
| `0x40000000` … | the ELF segments: file bytes, then zeros | code R X U, constants R U, data R W U |
| after the last segment | the heap, grown by `sbrk` (initially empty) | R W U |
| `0x401fb000` | guard page, never mapped | — |
| `0x401fc000`–`0x401fffff` | stack, 4 pages, `sp` = `0x40200000` | R W U |
| `0x4000000` | shared program pages (v1.3) | R X U |

- Every program has this window in its own Sv39 root, behind a private level-1 and level-0 table; the root shares the kernel and program tables with the template (v1.3). It starts at `e_entry` with every other register 0.
- A run-time process owns at most 64 pages: 3 tables, its segment pages, 4 stack pages and its heap. All of them return at once when it ends, as in v1.3.
- The kernel executes `fence.i` after writing a program's code and before it can run.

### System calls (ProtoOS-specific numbers)
| Call | `a7` | Behaviour |
|---|---|---|
| `run(name, len)` | 1008 | starts the archive program `name` in the lowest free slot; returns its pid. Errors: −22 length not 1–15, −14 name not readable by the caller, −2 no such program, −8 not a valid program file, −11 no free slot (or pids exhausted), −12 out of memory (nothing kept) |
| `wait(pid, flags)` | 1009 | the status of process `pid` once it has ended: its exit code (low 8 bits; 137 killed, 130 Ctrl-C) or 139 if it faulted; BLOCKED while it is alive, returns at once if it has already ended. `flags` 1 = foreground (Ctrl-C ends `pid`). Errors: −22 unknown flag, −3 no process with this pid, −1 the caller itself or a foreground wait for a boot process (alive or ended), −16 another foreground wait is in progress |
| `sbrk(bytes)` | 1010 | grows the heap of an archive program by `bytes` ≥ 0 with zeroed R W U pages; returns the previous end of the heap. Errors: −22 negative or not an archive program, −12 beyond the guard page or the 64-page limit (nothing changes) |
| `progs(index, buf)` | 1011 | name of archive file `index` (16 bytes, NUL padded) into the caller's U+R+W memory; returns the file size. Errors: −2 no such index, −14 bad buffer |

`meminfo` (v1.3) excludes the archive's pages from the total. In `slotinfo` the program of an archive process is 16 + its index in the archive. `getpid` returns the pid of any process.

### Ctrl-C and the console foreground
- While a foreground `wait` is in progress, the byte 0x03 typed on the console is not input: the kernel ends the awaited process (exit code 130, marked killed) whether it is running, sleeping or blocked in `read`, and the waiting process wakes with 130.
- Without a foreground wait, 0x03 is an ordinary input byte.

### Programs
- `user/`: `crt0.S` (calls `main`, its return value is the exit code), a small library (`print`, `read_line`, `malloc` on `sbrk`, system call wrappers) and `user.ld` (three segments with exact permissions).
- Released in `release/programs.img`: `guess` (guess a number from 1 to 100), `primes` (sieve of Eratosthenes on the heap), `clock` (time since boot every second until Ctrl-C).
- Test programs (`tests.img`) and deliberately malformed files and archives (`badelf.img`, `badarc/`) are built by `build-scripts/build-wsl.sh`; `build/images.json` records the defect each malformed file carries and the reason code the kernel must give.

### Shell
The shell reads its input one byte at a time, so bytes typed after a command line stay in the kernel buffer for the program that command starts. New: `ls` (programs in the archive and their sizes); `run <program>` for an archive program waits with it in the foreground (Ctrl-C prints `^C`; a non-zero status is reported as `pid N ended with status S`); `run <program> &` starts it in the background; `ps` shows archive programs by name; `help` prints a third line. Built-in programs (`worker`, `hello`, `counter`, `crash`) start in the background as in v1.3.

## Additional acceptance checks (3 clean boots each)
| ID | Scenario (`bootargs`, archive) | PASS criterion | Independent evidence |
|---|---|---|---|
| T18 | `protoos.test=elf`, `tests.img` | `t_elf` gets pid 3 and exactly 3 + its segment pages + 4 pages, its own root; all 512 entries of its window: exactly the segment pages, each with exactly the permissions of its segment and holding exactly the file bytes (then zeros), and the 4 stack pages R W U, each of its non-table pages mapped exactly once and no page table mapped; nothing else (guard, heap); no part of the window's 1 GiB in A's, B's or the kernel root; kernel, UART, archive, A, B not reachable; the window is not in A's, B's or the kernel root; archive pages reserved; `t_elf`'s own checks pass (exit 0); `t_exit42` exits 42; all pages return; `progs`/`sbrk` results | GDB at a checkpoint in the program: CPU state, independent Sv39 walks, SHA-256 of physical pages against the ELF file read by the harness; QEMU trace |
| T19 | `protoos.test=sbrk`, `tests.img` | heap starts at the page after the image; pages arrive zeroed and mapped R W U; refusals (−22, −12) change nothing; growth stops at exactly 64 pages owned; all 64 return | GDB at two checkpoints: page-table walks, physical heap contents, allocator |
| T20 | `protoos.test=wait`, `tests.img` | `wait` returns 42 (exit), 139 (fault), 137 (killed), the status of an already ended process, −3/−1/−22/−1 for the error cases; while waiting for `t_sleepy` the caller is BLOCKED and not dispatched, then returns 7; only `b_text` faults | GDB process state and dispatch watch, results in physical memory, QEMU trace |
| T21 | `protoos.test=ctrlc`, `tests.img` | typed Ctrl-C ends the foreground program while it is executing (taken on the interrupt from it, current process and root are its own) (130) and later a foreground program blocked in `read` (130), is not delivered as input, leaves a background program alone; a second foreground wait is refused (−16); without a foreground wait 0x03 arrives as input; all pages return | harness types into the UART; GDB process table, input state, scheduler fields |
| T22 | `protoos.mode=shell`, `programs.img` | scripted session: `ls`, `run primes`, `run guess` with the first guess typed ahead together with the command (it must reach the game) and won by binary search, `run clock` + Ctrl-C, `run clock &`, `ps`, `kill`, `run nosuch`, `mem`: specified output, the shell in the foreground wait, every page back, the shell BLOCKED at its prompt | UART log, GDB process table, allocator, QEMU trace |
| N10 | `protoos.test=elf-bad`, `badelf.img` | each of 30 malformed program files is refused with −8 for exactly the defect it carries (kernel reason = `images.json`); the allocator's counters do not move across the 30 refusals; bad names −2/−22/−22/−14/−14; the valid file still runs | results in physical memory, allocator and scheduler counters, boot log |
| N11 | `protoos.test=archive-bad`, each of 13 archives in `badarc/` (one over 4 MiB) | the kernel rejects the archive for exactly its defect; no programs (−2); its pages stay reserved and kernel-only; the system keeps running | boot log, GDB allocator and archive state, page-table walk |
| N12 | `protoos.test=user-bad`, `tests.img` | 8 programs that write into their code or constants, read the kernel, jump into data, overflow the stack, write past the heap, use floating point or write `satp`: each faults alone with the expected cause and address, `wait` reports 139, all pages return, A and B unaffected | QEMU trace (cause, epc, tval), GDB, results |

v1.4 is accepted when T1–T22 and N1–N12 all PASS in 3 clean boots each. The archive and ELF checkers are also tested on the host (`tests/test_archive_elf.c`: every defect, every truncation, mutation fuzzing whose accepted inputs must satisfy the guarantees above, plain and with ASan/UBSan).

Not exercised by a guest test: running out of pool memory while loading a program or growing a heap (31 232 pages cannot be exhausted by 6 slots of at most 64 pages; the code paths return every page taken so far), and a foreground `wait` for a boot process that has already ended.

## Independent audit before release
The v1.4 code was audited read-only by OpenAI Codex before publication. Findings and what was done:

| # | Finding | Verdict | Change |
|---|---|---|---|
| 1 | `malloc` rounding wraps for sizes near `SIZE_MAX` and returns memory it never got | confirmed (library) | sizes that would wrap are refused (0); `t_elf` checks it |
| 2 | `read_line(buf, 0)` writes `buf[0]` | confirmed (library) | returns −1 for a zero-size buffer; `t_elf` checks it |
| 4 | the shell read up to 16 bytes at a time, so input typed together with `run <program>` was kept by the shell, not given to the program | confirmed | the shell reads one byte at a time; `read_line` ignores an LF right after a CR the shell consumed; T22 types the first guess ahead |
| 5 | a foreground `wait` for an ended boot process returned its status instead of −1 | confirmed | the boot-process rule is checked first |
| 6 | an archive inside the pool but over 4 MiB was reported as outside the pool and not reserved | confirmed | containment and size are separate: reserved, then rejected with −1; N11 adds such an archive |
| 7 | T18 looked at only part of the window | confirmed (test) | T18 reads all 512 entries, and the root entry of the window in A, B and the kernel root |
| 8 | T18 would accept a stack page mapped onto a page table | confirmed (test) | T18 requires a one-to-one mapping of the non-table pages |
| 9 | N10 could not see an allocation that is freed again before the refusal | confirmed (test) | N10 compares the allocator's counters before and after the 30 refusals |
| 10 | T21 did not ensure Ctrl-C arrived while the foreground program was executing | confirmed (test) | T21 stops in the program, types Ctrl-C there and observes the kernel at `console_intr()` |

Findings 3 and 11 reported no defect (isolation, page accounting, other areas). Each strengthened check was shown to fail on a kernel with the corresponding bug put back in.
