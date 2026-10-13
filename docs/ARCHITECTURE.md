# ProtoOS v1.5 — architecture

Normative basis: the frozen specification v1.0 ([`TECHNICAL_SPECIFICATION_RU.md`](../TECHNICAL_SPECIFICATION_RU.md)) and decisions 01–19 ([`DECISION_REGISTER_RU.md`](../DECISION_REGISTER_RU.md)). v1.1 extensions (`write`, `sleep`, demo mode) are specified in [`SPEC_V1_1.md`](SPEC_V1_1.md), v1.2 extensions (console input through the PLIC, `read`, `uptime`, `procinfo`, the shell) in [`SPEC_V1_2.md`](SPEC_V1_2.md), v1.3 extensions (page allocator, 8 process slots, `spawn`/`kill`/`meminfo`/`slotinfo`, built-in programs) and the v1.3.1 corrections in [`SPEC_V1_3.md`](SPEC_V1_3.md), v1.4 extensions (programs in C loaded from a program archive, `run`/`wait`/`sbrk`/`progs`, Ctrl-C) in [`SPEC_V1_4.md`](SPEC_V1_4.md), v1.5 extensions (virtio-blk disk, the PFS file system, `fs_*`, `args`, `run_arg`, programs from the disk) in [`SPEC_V1_5.md`](SPEC_V1_5.md). This document records the technical choices the specifications leave to the implementer. Russian version: [`ARCHITECTURE_V1_RU.md`](ARCHITECTURE_V1_RU.md).

## Platform
QEMU `virt`, `-cpu rv64 -smp 1 -m 128M`; OpenSBI `fw_dynamic.bin` runs in M-mode and enters the kernel in S-mode at `0x80200000` (`a0` = hart id, `a1` = device tree). Kernel and user ISA: RV64IM + Zicsr + Zifencei, LP64, no floating point. In U-mode `sstatus.FS = VS = Off`, so any FP/vector instruction raises an illegal-instruction exception and the process is marked FAULTED (no FP state is kept).

## Boot sequence
1. `_start` (`boot.S`): interrupts off, `sscratch = 0`, clear `.bss` and the user pages, install `stvec`.
2. `kernel_main` (`kernel.c`): check hart 0 and `satp = 0`, parse the device tree (UART address, timebase, `bootargs`, since v1.4 the program archive `linux,initrd-start/-end` from `/chosen`, since v1.5 the `/soc/virtio_mmio@*` transports), select the scenario.
3. Set up the page pool below the device tree (`mem.c`); v1.4: if the boot loader supplied a program archive inside the pool, reserve its pages and check it completely (`archive.c`); build the kernel, template, A and B Sv39 roots (`vm.c`), switch to the kernel root, verify `satp` was accepted.
4. v1.5: find and start the disk (`virtio_blk.c`) and mount its file system (`pfs.c`, `disk.c`); without a virtio block device nothing happens and nothing is printed.
5. Create processes A and B (v1.5 test scenarios: and one archive program, "autorun"), enable the UART receive interrupt in the UART and the PLIC (`input.c`), arm the timer, set `sie.STIE` and `sie.SEIE`, enter the first process with `sret` (`proc.c`, `trap.S`).

## Physical memory (static)
| Range | Contents | Mapping |
|---|---|---|
| `0x80000000`–`0x801FFFFF` | OpenSBI | not accessible from S/U (PMP) |
| `__kernel_start` … `__text_end` | kernel code | every root: R X, U=0 |
| … `__rodata_end` | read-only data (incl. `kernel_secret`, used by test N2) | R, U=0 |
| `kernel_stack_guard` (4 KiB) | page below the kernel stack | **not mapped** |
| … `__kernel_end` | data/bss: kernel stack (16 KiB), emergency stack, page tables, `procs` | R W, U=0 |
| `__user_a_text`, `__user_b_text` | one code page each for A and B | owner's root only, U=1 |
| `__user_prog_text` (3 pages, v1.3) | shared built-in programs and the shell | every process root at `0x4000000`, R X U |
| `user_{a,b}_{data,stack}` | data and stack pages of A and B | owner's root only, U=1 |
| `0x80400000` – 2 MiB below the device tree (v1.3) | page pool: tables, data and stack pages of run-time processes | every root: 2 MiB leaves R W, U=0; plus the owner's 4 KiB user mappings |
| inside the pool, where the boot loader put it (QEMU: `0x84200000`) (v1.4) | program archive (`-initrd`) | reserved in the allocator for good; kernel-only through the pool map |
| `0x10000000` | UART 16550 | R W, U=0 |
| `0x0C000000`, `0x0C002000`, `0x0C201000` | PLIC: priorities, S-mode enables, S-mode threshold/claim (v1.2) | R W, U=0 |
| `0x10001000`–`0x10008FFF` (from the DT) | virtio-mmio transports (v1.5) | R W, U=0, in the UART's level-0 table |
| in `.bss` (v1.5) | disk: virtqueue page, 4 KiB bounce buffer, request header and status byte; the mounted directory; a 128 KiB file buffer | R W, U=0 (part of the kernel image) |

The static user pages lie after `__kernel_end` and are **not** part of the kernel's identity map: neither the kernel nor the other process can see them. The whole image must stay below `0x80400000` (checked at link time).

## Page allocator (v1.3)
`mem.c` manages the pool `[0x80400000, end)`, where `end` is the 2 MiB boundary below the device tree (31 232 pages on QEMU `virt` with 128 MiB), with a bitmap; `mem_alloc` takes the lowest free page and **zeroes it**, `mem_free` panics on a page that is not allocated. Lowest-first makes the reuse of freed pages deterministic, which test T13 exploits to show that a reused page arrives zeroed.

## Programs from the archive (v1.4)
The archive is a cpio "newc" file. `archive_parse` (`archive.c`) checks all of it once at boot — magic, every header field, names (1–15 characters `a–z 0–9 _`, unique), regular files only, data inside the archive, at most 32 files, the trailer and nothing but zeros after it — and indexes the files; any defect rejects the whole archive. An archive inside the pool is reserved before it is checked, whatever its size, so even a rejected one is never handed out as free memory. `elf_check` (`elf.c`) checks a program file completely before anything is allocated: ELF64 RISC-V `ET_EXEC`, little endian, `e_flags` = 0, 1–4 `PT_LOAD` segments in order, page-aligned, non-overlapping, inside `[0x40000000, 0x40100000)`, readable and never both writable and executable, file bytes inside the file, no dynamic linking or TLS, the entry inside executable file bytes, at most 57 pages. Both are pure functions over a byte array, read bytewise (no alignment assumptions), and tested on the host with every defect, every truncation and mutation fuzzing.

`run` then builds the process (`proc.c`, `vm.c`): a root copied from the template, a private level-1 table for the 1 GiB at `0x40000000` and a private level-0 table for its 2 MiB window, one zeroed pool page per segment page (file bytes copied in, the rest stays zero: `.bss`), mapped with the segment's permissions, and 4 stack pages; then `fence.i`, so instruction fetch sees the code that was written with ordinary stores. If memory runs out, every page taken so far is returned.

| Program VA | Contents | Permissions |
|---|---|---|
| `0x40000000` … | ELF segments | code R X U, constants R U, data R W U |
| after the image | heap (`sbrk`, page by page, zeroed) | R W U |
| `0x401fb000` | guard page, never mapped | — |
| `0x401fc000`–`0x401fffff` | stack, `sp` = `0x40200000` | R W U |

A run-time process owns at most 64 pages; `sbrk` allocates every page it needs before mapping any (all or nothing) and refuses growth into the guard page or beyond the limit.

## Disk and files (v1.5)
Three layers. `virtio_blk.c` drives one virtio-mmio version 2 block device: full virtio 1.x initialisation, one split virtqueue of 8 in a page of the kernel image, one request at a time (header, 4 KiB bounce buffer, status byte — descriptors never point at user memory), polled with a 2 s bound; any timeout or device error marks the device failed until reboot. `pfs.c` is the file system, pure code over a block interface (host-tested with a simulated faulty disk): a superblock, two directory copies A and B (seq + CRC-32, 63 entries) and contiguous file extents; a change writes the data into blocks free in the mounted copy, flushes, writes the new directory into the other copy with seq + 1 and flushes again, and mount takes the valid copy with the higher seq — so after a stop at any moment every file is entirely old or entirely new. A failed write or flush stops the file system (EIO until reboot). Every read checks the CRC of the whole file. `disk.c` connects the two, prints the boot lines and owns the 128 KiB file buffer through which `fs_read`, `fs_write` and programs started from the disk pass. A disk without flush support (or with `readonly=on`) is mounted read-only. Each `fs_*` call runs whole in the kernel with interrupts off.

`run` and `run_arg` share one path (`start_program`): the archive first, then the disk; a disk program is read whole and its CRC checked, then `elf_check` and the loader treat it exactly like an archive file. The argument line (≤ 55 bytes) is kept in the process slot for `args`.

## Virtual memory
Sv39 roots (`vm.c`): kernel, a template (kernel + program pages, never installed), A, B, and one per run-time process; 4 KiB user leaves; A/D bits preset in software (no reliance on Svadu). ASID 0 everywhere: every `satp` change is bracketed by `sfence.vma zero, zero`. Every root also maps the page pool with 2 MiB leaves, U=0, so the kernel can build and clear pages of any process whatever root is installed.

| Process VA | A (slot 0) | B (slot 1) | slot *s* = 2…7 (run time) | Permissions |
|---|---|---|---|---|
| text | `0x400000` | `0x800000` | not mapped | R X U |
| data | `0x401000` | `0x801000` | `0x400000·(s+1) + 0x1000` | R W U |
| guard | `0x402000` | `0x802000` | `+ 0x2000` | not mapped |
| stack | `0x403000` (sp = `0x404000`) | `0x803000` (sp = `0x804000`) | `+ 0x3000` (sp = `+ 0x4000`) | R W U |
| programs | `0x4000000`–`0x4002FFF` | same | same | R X U (shared, read-only) |

Disjoint VA ranges make a cross-process access name an address that is simply unmapped in the attacker's root. Archive programs (v1.4) all use the same window at `0x40000000` instead, each in its own root, so another program's pages are simply not in it. A run-time root is assembled by `vm_create` from 5 pool pages: the root and the low level-1 table start as copies of the template (sharing the kernel's high level-1 table and the UART, PLIC and program level-0 tables), then one private level-0 table maps the data and stack page. `vm_destroy` frees exactly these 5 pages.

## Traps
One vector, `trap_entry` (`trap.S`). `sscratch` protocol: while a process runs in U-mode it holds the address of that process's `struct trap_frame`; while the kernel runs it is 0. The kernel never sets `sstatus.SIE`, so a trap with `sscratch == 0` is a synchronous S-mode exception: it switches to the emergency stack and reports `KERNEL_PANIC` with `scause`/`sepc`/`stval` — including a kernel stack overflow into the guard page. From U-mode the entry saves x1–x31 plus `sepc`, `sstatus`, `scause`, `stval` (288 bytes) and continues on an empty kernel stack; `user_return` restores a frame and executes `sret`.

Interrupts from U-mode: the S timer (cause 5) preempts the process; the S external interrupt (cause 9, v1.2) is the UART receive interrupt routed by the PLIC — the kernel claims it, moves all received bytes into a 128-byte ring buffer (dropping new bytes when it is full), completes the claim and returns to the interrupted process. Because the kernel never sets `sstatus.SIE`, an interrupt that becomes pending while the kernel runs is taken right after the next `sret`, or noticed by polling `sip.SEIP` while the kernel waits.

## Scheduler
Round robin over the 8 process slots (v1.3; A and B in slots 0 and 1) with a 10 ms quantum (`timebase / 100`; 100 000 ticks on QEMU `virt`), driven by the S-mode timer through SBI TIME (`sbi_set_timer`). Choosing the next process and switching context are done by the kernel. The search for the next process starts at the slot after the last dispatched one, even if that process has just ended (v1.3.1: the position is kept in `sched.last_slot`). A run-time process that exits, faults or is killed gives all its pages back at once (`reclaim`); if its root is installed, the kernel switches to its own root first. Pids increase from 3 and are never reused: when they are exhausted, `spawn` returns −11 (v1.3.1).

States: READY, RUNNING, SLEEPING, BLOCKED (v1.2: waiting for input; v1.4: or for another process to end), EXITED, FAULTED. At every scheduling point the kernel first moves SLEEPING processes whose wake-up time has passed, and BLOCKED readers if input is buffered, to READY. A process BLOCKED in `wait` (v1.4) becomes READY, with the status in `a0`, the moment the awaited process ends (exit, fault, kill or Ctrl-C). A reader is dispatched only with input waiting: `sched_dispatch` installs its root and then copies the bytes into its buffer and sets its `a0`. If no process is READY but some are SLEEPING or BLOCKED, `sched_wait` arms the timer for the earliest wake-up (or disarms it) and waits in `wfi` (with `sstatus.SIE` still 0, so no trap is taken), servicing a pending external interrupt by polling and returning early when input arrives for a blocked reader; then scheduling resumes with a fresh quantum. Only when no process is READY, RUNNING, SLEEPING or BLOCKED does `sched_idle` turn the timer off, clear `sie`, install the kernel root and park in a `wfi` loop in `halt()`; no process context is ever loaded again.

## Console foreground and Ctrl-C (v1.4)
A `wait` with the foreground flag makes the awaited process the console foreground (`sched.fg_pid`) and arms Ctrl-C in the input path (`input.intr_armed`). While armed, the receive interrupt does not buffer the byte 0x03 but sets `input.intr_pending`; the kernel then ends the foreground process like `kill` (exit code 130) — at once if the interrupt came from user mode, or from the wait loop — whether it is running, sleeping or blocked in `read`. Ending the foreground process or its waiter disarms Ctrl-C; without a foreground wait 0x03 is ordinary input.

## System calls
`ecall` from U-mode; number in `a7`, arguments in `a0`–`a4` (five since v1.5), result in `a0`; execution resumes at `sepc + 4`. **Numbering rule:** a call uses the RISC-V Linux number only where its meaning is the Linux one; every other call is ProtoOS-specific, numbered from 1001 up; a number, once released, never changes its meaning (v1.5 adds `run_arg` as 1017 instead of changing `run`, 1008).

| Call | `a7` | Behaviour |
|---|---|---|
| `read(fd, buf, len)` | 63 | v1.2: `fd` must be 0, `len` ≤ 256; every byte of the buffer must be in a valid U+R+W page of the caller **before** the call may block; returns the bytes buffered (≥ 1) at once, or the process is BLOCKED until input arrives; `len = 0` returns 0. Errors: −9, −14, −22 |
| `write(fd, buf, len)` | 64 | v1.1: `fd` must be 1, `len` ≤ 256; the kernel walks the caller's own page table and requires every byte of the buffer to be in a valid U+R page **before** reading it with `sstatus.SUM` set; then copies it to the UART (non-printable bytes except `\n`, `\t` and, since v1.2, backspace become `?`). Errors: −9 bad fd, −14 bad address, −22 bad length |
| `sleep(ms)` | 1001 | v1.1: `ms` ≤ 60 000 (else −22); the process is SLEEPING for at least `ms` milliseconds; `0` acts like `yield`; returns 0 |
| `uptime()` | 1002 | v1.2: microseconds of `time` since kernel entry |
| `procinfo(pid, buf)` | 1003 | v1.2: writes state, preemptions, system calls and dispatches of `pid` (any pid with a slot since v1.3, else −22) as four words into 32 bytes of the caller's U+R+W memory (else −14); returns 0 |
| `spawn(program)` | 1004 | v1.3: starts built-in program 1–5 in the lowest free slot; returns its pid. Errors: −22 unknown program, −11 no free slot (or pids exhausted, v1.3.1), −12 out of memory |
| `kill(pid)` | 1005 | v1.3: ends a live run-time process (exit code 137), its pages return; 0. Errors: −3 no such live process, −1 the caller or a boot process |
| `meminfo()` | 1006 | v1.3: free pages in `a0`, pool pages in `a1` (v1.4: without the archive's pages) |
| `slotinfo(slot, buf)` | 1007 | v1.3: pid, state, program, preemptions, system calls, pages of slot 0–7 as six words into 48 bytes of U+R+W memory; 0. Errors: −22, −14. v1.4: program 16 + n = archive file n |
| `run(name, len)` | 1008 | v1.4: starts archive program `name`; returns its pid. Errors: −22, −14, −2 not found, −8 not a valid program, −11, −12 |
| `wait(pid, flags)` | 1009 | v1.4: status of `pid` once it has ended (exit code & 0xff, 137 killed, 130 Ctrl-C, 139 faulted), BLOCKED until then; flag 1 = foreground. Errors: −22, −3, −1, −16 |
| `sbrk(bytes)` | 1010 | v1.4: grows an archive program's heap; returns the old end. Errors: −22, −12 |
| `progs(index, buf)` | 1011 | v1.4: name (16 bytes) of archive file `index`; returns its size. Errors: −2, −14 |
| `fs_list(index, buf)` | 1012 | v1.5: name (16 bytes) of the `index`-th file on the disk; returns its size. Errors: −14, −19 no disk, −2 |
| `fs_read(name, namelen, off, buf, len)` | 1013 | v1.5: bytes of a file after a whole-file CRC check; returns the count. Errors: −22, −14, −19, −2, −5 |
| `fs_write(name, namelen, buf, len)` | 1014 | v1.5: create or replace a file whole; 0 once durable. Errors: −22, −27, −14, −19, −30, −28, −5 |
| `fs_delete(name, namelen)` | 1015 | v1.5: remove a file. Errors: −22, −19, −30, −2, −5 |
| `args(buf, len)` | 1016 | v1.5: the caller's argument line; returns its length. Errors: −14 |
| `run_arg(name, namelen, arg, arglen)` | 1017 | v1.5: `run` with an argument line (≤ 55 bytes), the archive first, then the disk. Errors: as `run`, and −5 |
| `exit(code)` | 93 | process becomes EXITED and never runs again; a run-time process's pages return at once |
| `yield()` | 124 | process becomes READY, the scheduler switches; returns 0 |
| `getpid()` | 172 | returns the pid: 1 for A, 2 for B, 3 and up for run-time processes (built-in or archive programs) |
| anything else | — | returns `-38` (ENOSYS); the process keeps running |

Any other synchronous exception from U-mode (page or access fault, illegal instruction, misaligned access, breakpoint) marks only that process FAULTED (a run-time process also gives its pages back; v1.4: processes waiting for it wake with 139); the others keep running.

## One image, many scenarios
The scenario is chosen only by `bootargs` (device tree `/chosen`); the ELF is identical for all of them.

| `bootargs` | A | B | Purpose |
|---|---|---|---|
| `protoos.mode=normal` | worker | worker | normal mode, T7 |
| `protoos.mode=demo` | prints every 400 ms | prints every 700 ms, then writes into A's memory | v1.1 demonstration |
| `protoos.mode=shell` | `proto>` shell: `help`, `ps`, `uptime`, `echo`, `attack`, since v1.3 `run`, `kill`, `mem`, since v1.4 `ls`, `run <program> [&]`, since v1.5 `run <program> <argument>` | worker | interactive mode, T12, T17; with `programs.img` T22; with a disk T23, T26, N13 |
| `protoos.test=fs` / `fs-ro` / `persist-w` / `persist-r` / `crash` / `fs-bad` / `no-disk` (with `tests.img` and a disk) | the shell, idle; the kernel also starts `t_fs` / `t_fsro` / `t_pw` / `t_pr` / `t_crash` / `t_fsbad` / `t_nodisk` at boot | worker | T24 / T24 / T25 / T25 / T27 / N14 / N15 (v1.5) |
| `protoos.test=elf` / `sbrk` / `wait` / `ctrlc` / `user-bad` (with `tests.img`) | load programs / heap / wait / Ctrl-C / 8 rule-breaking programs | worker | T18 / T19 / T20 / T21 / N12 |
| `protoos.test=elf-bad` (`badelf.img`) / `archive-bad` (a `badarc/` image) | 30 malformed program files / a malformed archive | worker | N10 / N11 |
| `protoos.test=mem` / `spawn` / `kill` / `multi` / `spawn-bad` | `memcheck` twice / one worker / kill and respawn / three workers and a crash / rejected calls and 200 cycles | worker | T13 / T14 / T15 / T16 / N9 |
| `protoos.test=read` / `read-bad` / `read-flood` | valid / rejected / flooded `read` | worker | T10 / N7 / N8 |
| `protoos.test=uptime` | `uptime` around `sleep(100)` | worker | T11 |
| `protoos.test=write` / `write-bad` | valid / invalid `write` calls | worker | T8 / N6 |
| `protoos.test=sleep` / `sleep-all` | sleeps 200 ms / 100 ms | worker / sleeps 150 ms | T9 |
| `protoos.test=boot` | — | — | T1 (S-mode, Sv39 active) |
| `protoos.test=boot-trap` | — | — | fatal S-mode trap (diagnostics) |
| `protoos.test=umode` / `preempt` | worker | worker | T2 / T3 |
| `protoos.test=getpid` / `yield` / `exit` | … | … | T4 |
| `protoos.test=isolation` | writes into B's page | worker | T5, N1 |
| `protoos.test=kernel-read` | reads `kernel_secret` | worker | T5, N2 |
| `protoos.test=fault` | writes into its own code | worker | T6 |
| `protoos.test=csr` | `csrw satp` | worker | N3 |
| `protoos.test=bad-syscall` | `a7 = 999` | worker | N4 |
| `protoos.test=idle` / `idle-fault` | exit / fault | exit | N5 |
| `protoos.demo=console` | — | — | small polling UART console (runs before the MMU is enabled) |

`worker` is an endless loop without system calls that checks its own context on every iteration (`s0 + s1 == 0`, `s2`–`s10` hold a fixed pattern) and fills a 2 KiB canary area, so a lost or foreign register context is detected by the process itself.

## Source map
| File | Role |
|---|---|
| `src/boot.S` | entry point, stacks, `.bss` clearing |
| `src/trap.S` | trap vector, context save/restore, fatal S-mode path |
| `src/kernel.c` | UART, boot flow, scenario table, panic |
| `src/fdt.c` | bounded device-tree parser (UART, timebase, `bootargs`, archive, v1.5 virtio-mmio transports) |
| `src/virtio_blk.c`, `src/pfs.c`, `src/disk.c` | v1.5: virtio-blk driver, PFS file system (pure, host-tested), their glue and boot lines |
| `src/vm.c`, `src/sv39_builder.c`, `src/sv39.h` | Sv39 page tables |
| `src/proc.c` | processes, scheduler, system calls, fault handling, idle |
| `src/input.c` | v1.2: PLIC setup, UART receive interrupt, input ring buffer |
| `src/mem.c` | v1.3: physical page allocator (bitmap, zero on allocation); v1.4: reservation of the archive |
| `src/archive.c`, `src/elf.c` | v1.4: program archive parser, program file checker (pure, host-tested) |
| `src/user.S` | user programs A and B; the shared program pages (shell, `worker`, `hello`, `counter`, `crash`, `memcheck`) |
| `src/linker.ld` | memory layout and size checks |
| `user/` | v1.4: `crt0.S`, library, `user.ld`; programs `guess`, `primes`, `clock`, v1.5 `files`, `cat`, `note`, `rm`, `hello` (on the disk); test programs |
| `build-scripts/mkimages.py` | v1.4: program archives, malformed test files and archives, `images.json`; v1.5: `disk.img` and the test disks |
| `build-scripts/mkpfs.py`, `build-scripts/pfsck.py` | v1.5: PFS image builder; independent checker written from the specification |

## Build and verify
```sh
sh build-scripts/build-wsl.sh            # Zig 0.13.0 -> build/protoos.elf, programs, archives, disks
sh build-scripts/host-tests.sh           # Sv39 builder, device tree, archive/ELF checkers, PFS fault model
sh build-scripts/run-scenario.sh protoos.mode=normal 5
sh build-scripts/run-shell.sh            # shell with programs.img and a copy of disk.img; quit: Ctrl-A, X
python3 validation/run_acceptance.py     # T1-T27, N1-N15 x3 -> evidence/acceptance/<run>/
```
