# ProtoOS v1.1 — architecture

Normative basis: the frozen specification v1.0 ([`TECHNICAL_SPECIFICATION_RU.md`](../TECHNICAL_SPECIFICATION_RU.md)) and decisions 01–19 ([`DECISION_REGISTER_RU.md`](../DECISION_REGISTER_RU.md)). v1.1 extensions (`write`, `sleep`, demo mode) are specified in [`SPEC_V1_1.md`](SPEC_V1_1.md). This document records the technical choices the specifications leave to the implementer. Russian version: [`ARCHITECTURE_V1_RU.md`](ARCHITECTURE_V1_RU.md).

## Platform
QEMU `virt`, `-cpu rv64 -smp 1 -m 128M`; OpenSBI `fw_dynamic.bin` runs in M-mode and enters the kernel in S-mode at `0x80200000` (`a0` = hart id, `a1` = device tree). Kernel and user ISA: RV64IM + Zicsr + Zifencei, LP64, no floating point. In U-mode `sstatus.FS = VS = Off`, so any FP/vector instruction raises an illegal-instruction exception and the process is marked FAULTED (no FP state is kept).

## Boot sequence
1. `_start` (`boot.S`): interrupts off, `sscratch = 0`, clear `.bss` and the user pages, install `stvec`.
2. `kernel_main` (`kernel.c`): check hart 0 and `satp = 0`, parse the device tree (UART address, timebase, `bootargs` from `/chosen`), select the scenario.
3. Build the three Sv39 roots (`vm.c`), switch to the kernel root, verify `satp` was accepted.
4. Create processes A and B, arm the timer, enter the first process with `sret` (`proc.c`, `trap.S`).

## Physical memory (static)
| Range | Contents | Mapping |
|---|---|---|
| `0x80000000`–`0x801FFFFF` | OpenSBI | not accessible from S/U (PMP) |
| `__kernel_start` … `__text_end` | kernel code | every root: R X, U=0 |
| … `__rodata_end` | read-only data (incl. `kernel_secret`, used by test N2) | R, U=0 |
| `kernel_stack_guard` (4 KiB) | page below the kernel stack | **not mapped** |
| … `__kernel_end` | data/bss: kernel stack (16 KiB), emergency stack, page tables, `procs` | R W, U=0 |
| `__user_a_text`, `__user_b_text` | one code page each for A and B | owner's root only, U=1 |
| `user_{a,b}_{data,stack}` | data and stack pages of A and B | owner's root only, U=1 |
| `0x10000000` | UART 16550 | R W, U=0 |

User pages lie after `__kernel_end` and are **not** part of the kernel's identity map: neither the kernel nor the other process can see them. The whole image must stay below `0x80400000` (checked at link time).

## Virtual memory
Three Sv39 roots (`vm.c`): kernel, A, B; 4 KiB leaves only; A/D bits preset in software (no reliance on Svadu). ASID 0 everywhere: every `satp` change is bracketed by `sfence.vma zero, zero`.

| Process VA | A | B | Permissions |
|---|---|---|---|
| text | `0x400000` | `0x800000` | R X U |
| data | `0x401000` | `0x801000` | R W U |
| guard | `0x402000` | `0x802000` | not mapped |
| stack | `0x403000` (sp = `0x404000`) | `0x803000` (sp = `0x804000`) | R W U |

Disjoint VA ranges make a cross-process access name an address that is simply unmapped in the attacker's root.

## Traps
One vector, `trap_entry` (`trap.S`). `sscratch` protocol: while a process runs in U-mode it holds the address of that process's `struct trap_frame`; while the kernel runs it is 0. The kernel never sets `sstatus.SIE`, so a trap with `sscratch == 0` is a synchronous S-mode exception: it switches to the emergency stack and reports `KERNEL_PANIC` with `scause`/`sepc`/`stval` — including a kernel stack overflow into the guard page. From U-mode the entry saves x1–x31 plus `sepc`, `sstatus`, `scause`, `stval` (288 bytes) and continues on an empty kernel stack; `user_return` restores a frame and executes `sret`.

## Scheduler
Round robin with a 10 ms quantum (`timebase / 100`; 100 000 ticks on QEMU `virt`), driven by the S-mode timer through SBI TIME (`sbi_set_timer`). Choosing the next process and switching context are done by the kernel. States: READY, RUNNING, SLEEPING, EXITED, FAULTED. At every scheduling point the kernel first moves SLEEPING processes whose wake-up time has passed to READY. If no process is READY but some are SLEEPING, `sched_wait` arms the timer for the earliest wake-up and waits in `wfi` (with `sstatus.SIE` still 0, so no trap is taken), then scheduling resumes with a fresh quantum. Only when no process is READY, RUNNING or SLEEPING does `sched_idle` turn the timer off, clear `sie`, install the kernel root and park in a `wfi` loop in `halt()`; no process context is ever loaded again.

## System calls
`ecall` from U-mode; number in `a7`, arguments in `a0`–`a2`, result in `a0`; execution resumes at `sepc + 4`. Numbers follow the RISC-V Linux table, except the ProtoOS-specific `sleep`.

| Call | `a7` | Behaviour |
|---|---|---|
| `write(fd, buf, len)` | 64 | v1.1: `fd` must be 1, `len` ≤ 256; the kernel walks the caller's own page table and requires every byte of the buffer to be in a valid U+R page **before** reading it with `sstatus.SUM` set; then copies it to the UART (non-printable bytes become `?`). Errors: −9 bad fd, −14 bad address, −22 bad length |
| `sleep(ms)` | 1001 | v1.1: `ms` ≤ 60 000 (else −22); the process is SLEEPING for at least `ms` milliseconds; `0` acts like `yield`; returns 0 |
| `exit(code)` | 93 | process becomes EXITED and never runs again |
| `yield()` | 124 | process becomes READY, the scheduler switches; returns 0 |
| `getpid()` | 172 | returns 1 for A, 2 for B |
| anything else | — | returns `-38` (ENOSYS); the process keeps running |

Any other synchronous exception from U-mode (page or access fault, illegal instruction, misaligned access, breakpoint) marks only that process FAULTED; the other one keeps running.

## One image, many scenarios
The scenario is chosen only by `bootargs` (device tree `/chosen`); the ELF is identical for all of them.

| `bootargs` | A | B | Purpose |
|---|---|---|---|
| `protoos.mode=normal` | worker | worker | normal mode, T7 |
| `protoos.mode=demo` | prints every 400 ms | prints every 700 ms, then writes into A's memory | v1.1 demonstration |
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
| `src/fdt.c` | bounded device-tree parser (UART, timebase, `bootargs`) |
| `src/vm.c`, `src/sv39_builder.c`, `src/sv39.h` | Sv39 page tables |
| `src/proc.c` | processes, scheduler, system calls, fault handling, idle |
| `src/user.S` | user programs A and B |
| `src/linker.ld` | memory layout and size checks |

## Build and verify
```sh
sh build-scripts/build-wsl.sh            # Zig 0.13.0 -> build/protoos.elf
sh build-scripts/host-tests.sh           # Sv39 builder + device-tree parser (host)
sh build-scripts/run-scenario.sh protoos.mode=normal 5
python3 validation/run_acceptance.py     # T1-T7, N1-N5 x3 -> evidence/acceptance/<run>/
```
