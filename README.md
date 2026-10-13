# ProtoOS

[![acceptance](https://github.com/Intent-Forge/ProtoOS/actions/workflows/acceptance.yml/badge.svg)](https://github.com/Intent-Forge/ProtoOS/actions/workflows/acceptance.yml)

An experimental, deliberately minimal but architecturally genuine operating system for **RISC-V RV64**, running on **QEMU `virt`**.

OpenSBI (M-mode) hands over to the ProtoOS kernel (S-mode), which runs user processes (U-mode) with their own **Sv39** page tables, **timer-driven preemptive round-robin scheduling**, real `ecall` system calls, **interrupt-driven keyboard input** and a **page allocator**. Since v1.4 it **loads programs written in C** from a program archive given at boot, checks every file before loading it, and lets you run them from its command shell (Ctrl-C stops a program); every page a program used comes back. It is a teaching/demonstration kernel, not a general-purpose OS.

![ProtoOS running under QEMU: two user processes print via write() and sleep(), B tries to overwrite A's memory and is stopped; a shell session: run counter and worker, ps, kill, run crash (stopped by a page fault), mem shows every page back; programs in C from the archive: ls, primes, clock stopped with Ctrl-C, the guessing game; then timer-driven switching between the processes](docs/demo/protoos-demo.svg)

<sub>Real UART output of the released kernel captured under QEMU (demo mode and two typed shell sessions — built-in processes, then the v1.4 programs from the archive — with their real timing, then the scheduler test replayed at a readable pace). Regenerate with `python3 docs/demo/make_demo.py`.</sub>

## Features (v1.4)
- Monolithic kernel, ~4,600 lines of freestanding C11 and RV64 assembly; no libc. ISA: RV64IM + Zicsr + Zifencei, LP64, no floating point.
- **Programs in C (v1.4):** static ELF files in a cpio archive that the boot loader places in RAM (QEMU `-initrd`). The kernel checks the whole archive at boot and every program file before it allocates anything (segment permissions, addresses, sizes, no W+X), then loads it into its own address space with exact page permissions, a 16 KiB stack behind a guard page and a heap (`sbrk`); at most 64 pages per process. A small library (`print`, `read_line`, `malloc`, system call wrappers) and three programs: `guess`, `primes`, `clock`.
- **Memory:** a physical page allocator over the RAM above the kernel (31 232 pages of 4 KiB on QEMU `virt`); every page is zeroed when handed out and returned when its process ends.
- **Virtual memory:** an Sv39 root per process, 4 KiB user pages, disjoint user address ranges. Kernel pages and the page pool are mapped with U=0; each process's pages exist as user pages only in its own root; built-in programs are shared read-only. Guard pages below the kernel stack and the user stacks.
- **Processes:** up to 8. A (pid 1) and B (pid 2) are created at boot; the others are started with `spawn` (built-in programs) or `run` (programs from the archive) and ended with `kill`, `exit`, a fault or Ctrl-C; `wait` blocks until a process has ended and returns its status. States: READY, RUNNING, SLEEPING, BLOCKED (waiting for input or for another process), EXITED, FAULTED.
- **Scheduler:** round robin, 10 ms quantum, preemption by the S-mode timer (SBI TIME). A process that never yields is still preempted; a process waiting for input or sleeping uses no CPU time.
- **Interrupts:** S-mode timer and the UART receive interrupt through the PLIC into a 128-byte kernel input buffer.
- **System calls** (`a7` = number, `a0`–`a2` = arguments, `a0` = result): `read` (63), `write` (64), `exit` (93), `yield` (124), `getpid` (172), and ProtoOS-specific `sleep` (1001), `uptime` (1002), `procinfo` (1003), `spawn` (1004), `kill` (1005), `meminfo` (1006), `slotinfo` (1007), `run` (1008), `wait` (1009), `sbrk` (1010), `progs` (1011). `read` and `write` check the whole user buffer against the caller's own page table before touching it (kernel pages, other processes' pages and guard pages are rejected with `-14`). Unknown numbers return `-38` and the process continues.
- **Shell:** `protoos.mode=shell` runs `proto>` in process A — `help`, `ps`, `uptime`, `echo <text>`, `attack` (asks the kernel to print B's memory; refused with `-14`), `run <worker|hello|counter|crash>`, `kill <pid>`, `mem`, and since v1.4 `ls` and `run <program>` for programs from the archive (the shell waits with the program in the foreground; Ctrl-C stops it; `run <program> &` starts it in the background).
- **Fault isolation:** any other exception from U-mode (page fault, illegal instruction, …) marks only that process FAULTED and frees its pages; the others keep running. With nothing runnable, the kernel parks safely (timer off, `wfi`).
- **One image for everything:** the same ELF runs the normal mode, the demo and shell modes and every test scenario, selected by `bootargs`.

New in v1.4 (specified in [`docs/SPEC_V1_4.md`](docs/SPEC_V1_4.md)): programs in C loaded from a program archive, `run`/`wait`/`sbrk`/`progs`, Ctrl-C, the shell commands `ls` and `run <program> [&]`. v1.3 ([`docs/SPEC_V1_3.md`](docs/SPEC_V1_3.md)) added the page allocator, 8 process slots, `spawn`/`kill`/`meminfo`/`slotinfo`, built-in programs and the shell commands for them; v1.2 ([`docs/SPEC_V1_2.md`](docs/SPEC_V1_2.md)) added keyboard input through the PLIC, `read` with the BLOCKED state, `uptime`, `procinfo` and the shell; v1.1 ([`docs/SPEC_V1_1.md`](docs/SPEC_V1_1.md)) `write`, `sleep` and the demo mode. Earlier versions are tagged [`v1.3.1`](https://github.com/Intent-Forge/ProtoOS/tree/v1.3.1), [`v1.2`](https://github.com/Intent-Forge/ProtoOS/tree/v1.2), [`v1.1`](https://github.com/Intent-Forge/ProtoOS/tree/v1.1) and [`v1.0`](https://github.com/Intent-Forge/ProtoOS/tree/v1.0).

Architecture, memory map and ABI: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) ([Russian](docs/ARCHITECTURE_V1_RU.md)). Original frozen specification: [`TECHNICAL_SPECIFICATION_RU.md`](TECHNICAL_SPECIFICATION_RU.md), [`DECISION_REGISTER_RU.md`](DECISION_REGISTER_RU.md).

## What has been verified — and what has not
v1.0 defines 12 mandatory checks in [`ACCEPTANCE_MATRIX.csv`](ACCEPTANCE_MATRIX.csv): T1–T7 (S-mode boot, two U-mode contexts, timer preemption, `getpid`/`yield`/`exit`, Sv39 isolation, fault isolation, autonomous normal run) and N1–N5 (cross-process write, user read of kernel memory, privileged instruction from U-mode, unknown syscall, no runnable process). v1.1 adds T8 (`write`), T9 (`sleep`, including both processes asleep) and N6 (rejected `write` buffers), see [`docs/SPEC_V1_1.md`](docs/SPEC_V1_1.md). v1.2 adds T10 (`read` blocks without busy waiting and is woken by the UART interrupt), T11 (`uptime` against kernel time stamps), T12 (a scripted shell session), N7 (rejected `read` buffers) and N8 (input flood: 300 bytes into a 128-byte buffer), see [`docs/SPEC_V1_2.md`](docs/SPEC_V1_2.md); for these the harness types into the guest UART through a socket. v1.3 adds T13 (pages arrive zeroed even when reused), T14 (a spawned process gets its own root and pages and is scheduled), T15 (kill/exit return every page; a killed process never runs again), T16 (with several processes only the attacker faults), T17 (a scripted shell session with `run`, `ps`, `kill`, `mem`) and N9 (rejected `spawn`/`kill`, the slot limit, 200 spawn/kill cycles without a leaked page), see [`docs/SPEC_V1_3.md`](docs/SPEC_V1_3.md). v1.4 adds T18 (a C program is loaded page by page exactly as its ELF file says: permissions and contents of every page compared with the file), T19 (heap growth up to the 64-page limit), T20 (`wait` for exit, fault, kill, blocking), T21 (Ctrl-C ends the foreground program, also while it waits for input), T22 (a scripted shell session with `ls`, `primes`, the guessing game won by binary search, `clock` stopped with Ctrl-C), N10 (30 malformed program files, each refused for exactly its defect), N11 (13 malformed archives) and N12 (8 programs that break the rules fault alone), see [`docs/SPEC_V1_4.md`](docs/SPEC_V1_4.md).

| Check | Result |
|---|---|
| **v1.4:** T1–T22, N1–N12, 3 clean boots each, QEMU 10.2.1 + OpenSBI 1.8 + gdb 17.1 | **all PASS** — [`evidence/acceptance/20261013T045933Z/RESULTS.json`](evidence/acceptance/20261013T045933Z/RESULTS.json); the checks of v1.4 were also run against 15 kernels with a bug put in on purpose (W+X accepted, writable code, file bytes in `.bss`, heap limit off by one, Ctrl-C left armed, a fault reported as success, archive not reserved, duplicate names accepted, waiters woken by input, and the six scenarios of the audit below): each one fails its check |
| v1.4 on a second machine (Intel N100, Ubuntu 24.04, QEMU 8.2.2 + OpenSBI 1.7 + gdb 15.1) | kernel and program archive rebuilt byte-identical; full matrix ×3 **all PASS** — [`evidence/crosscheck-n100/20261013T045928Z/`](evidence/crosscheck-n100/20261013T045928Z/RESULTS.json) |
| Host tests of the archive parser and the program file checker (plain and ASan/UBSan) | every defect, every truncation, 2 × 200 000 mutated inputs: accepted inputs always satisfy the guarantees |
| v1.3.1: T1–T17, N1–N9, 3 clean boots each (93 boots), QEMU 10.2.1 + OpenSBI 1.8 + gdb 17.1 | **all PASS** — [`evidence/acceptance/20261012T233512Z/RESULTS.json`](evidence/acceptance/20261012T233512Z/RESULTS.json) (v1.3: [`20261012T220448Z`](evidence/acceptance/20261012T220448Z/RESULTS.json)); a reused page that its previous owner filled with `0xa5` arrived zeroed; 206 spawn/kill cycles leaked no page |
| v1.3.1 on a second machine (Intel N100, Ubuntu 24.04, QEMU 8.2.2 + OpenSBI 1.7 + gdb 15.1) | ELF rebuilt byte-identical; full matrix ×3 **all PASS** — [`evidence/crosscheck-n100/20261012T233509Z/`](evidence/crosscheck-n100/20261012T233509Z/RESULTS.json) |
| Independent code audit of v1.4 before release (OpenAI Codex, read-only access) | 8 findings confirmed and fixed before publication: 4 in the code (shell kept input typed ahead for a program, an archive over 4 MiB was not reserved, a foreground-wait rule, two library edge cases) and 4 checks made stricter (T18 whole window and one-to-one pages, N10 no allocation before refusal, T21 Ctrl-C while the program runs) — see [`docs/SPEC_V1_4.md`](docs/SPEC_V1_4.md#independent-audit-before-release) |
| Independent code audit of v1.3 (OpenAI Codex, read-only access) | 2 findings (Round Robin position after a process ended; pid wrap-around), both confirmed and fixed in v1.3.1 — see [`docs/SPEC_V1_3.md`](docs/SPEC_V1_3.md#v131-corrections-independent-audit) |
| v1.2: T1–T12, N1–N8 ×3 (75 boots), also on the N100 | **all PASS** — [`evidence/acceptance/20261012T201554Z/RESULTS.json`](evidence/acceptance/20261012T201554Z/RESULTS.json), [`evidence/crosscheck-n100/20261012T201932Z/`](evidence/crosscheck-n100/20261012T201932Z/RESULTS.json) |
| v1.1: T1–T9, N1–N6 ×3 (60 boots) | **all PASS** — [`evidence/acceptance/20261012T053626Z/RESULTS.json`](evidence/acceptance/20261012T053626Z/RESULTS.json); a 200 ms `sleep` measured 200.9–201.1 ms |
| v1.0: T1–T7, N1–N5 ×3 (48 boots); same matrix on OpenSBI 1.4 | **all PASS** — [`evidence/acceptance/20261011T231210Z/`](evidence/acceptance/20261011T231210Z/RESULTS.json), [`evidence/crosscheck-opensbi-1.4/`](evidence/crosscheck-opensbi-1.4/) |
| CI on every push: stock Ubuntu 24.04 runner, QEMU 8.2.2, OpenSBI 1.7, gdb 15 — full matrix ×3 | **all PASS** — see the badge and [Actions](https://github.com/Intent-Forge/ProtoOS/actions/workflows/acceptance.yml) (evidence attached to each run) |
| Rebuild of the ELF (and since v1.4 the program archive) on the CI runner (every push) and, for v1.0, by a second independent reviewer | byte-identical to `release/protoos.elf` (and `release/programs.img`) |
| Real hardware, other emulators | **not run** |

Evidence is collected from outside the kernel, never from its own messages: the QEMU interrupt trace (`-d int`), GDB (privilege level, CSRs, registers, physical memory, an independent Sv39 page-table walk) and QMP sampling for the normal mode. Every boot directory under `evidence/` holds the exact QEMU command line, raw logs, observations and a per-check verdict. The normal-mode check (T7) is a 3 × 30 s bounded observation (~3,075 timer switches per run), not a proof of unlimited stability.

v1.4: kernel [`release/protoos.elf`](release/protoos.elf), SHA-256 `fe21d834e4dcdb8b83e7e108d617e53c5ff013abd8c9410c3055ee3a0697ad71`; program archive [`release/programs.img`](release/programs.img), SHA-256 `2a96d0abd0ea06343bbd1788be9993d55969da784c901b4d2bafcaee0a8271e6` (v1.3.1: `9688a42f…`, v1.3: `aca6a11b…`, v1.2: `df8eca42…`, v1.1: `129fce46…`, v1.0: `f1d5a5bc…` — see the tags).

## Build and run
Tested on Ubuntu 26.04 under WSL2; any recent x86-64 Ubuntu/Debian should work.

```sh
sudo apt install qemu-system-riscv opensbi gdb-multiarch binutils-riscv64-unknown-elf \
                 python3 python3-venv build-essential
sh build-scripts/setup-zig.sh            # pinned Zig 0.13.0 (hash-checked) into ~/.protoos-venv
sh build-scripts/build-wsl.sh            # -> build/protoos.elf, programs, build/programs.img + SHA-256
sh build-scripts/run-shell.sh            # shell with the programs: ls, run guess, run clock, ps, ... (quit: Ctrl-A, X)
sh build-scripts/run-scenario.sh protoos.mode=demo 8         # demo: processes print, B attacks A
sh build-scripts/run-scenario.sh protoos.mode=normal 10      # normal mode for 10 s
sh build-scripts/run-scenario.sh protoos.test=isolation 3    # A writes into B -> page fault
python3 validation/run_acceptance.py     # full acceptance, 3 clean boots per check (~5 min)
sh build-scripts/host-tests.sh           # host tests: Sv39 builder, DT parser, archive and ELF checkers
```
On older Ubuntu releases QEMU for RISC-V is in `qemu-system-misc`. Other scenarios: `umode`, `preempt`, `getpid`, `yield`, `exit`, `kernel-read`, `fault`, `csr`, `bad-syscall`, `idle`, `idle-fault`, `write`, `write-bad`, `sleep`, `sleep-all`, `read`, `read-bad`, `read-flood`, `uptime` (the `read` ones wait for typed input), `mem`, `spawn`, `kill`, `multi`, `spawn-bad`; with a program archive as the fourth argument (`build/tests.img`): `elf`, `sbrk`, `wait`, `ctrlc`, `user-bad`. Released programs only: `sh build-scripts/run-shell.sh release/protoos.elf release/programs.img`.

## Not included (by design)
No file system (programs come in a read-only archive loaded with the kernel; nothing is saved), dynamic linking, networking, drivers beyond the UART and PLIC, SMP, POSIX or user-mode floating point.

## How it was made
ProtoOS was built with AI assistance under the direction of the project owner: the specification and earlier stages with OpenAI GPT models, the v1.0–v1.4 kernel, programs, test harness and releases with Anthropic Claude; the v1.3 and v1.4 code was also audited independently by OpenAI Codex. The owner set the goals and made the decisions; the frozen specification and the decision register record the AI roles as they were at the time.

## License
[GNU Affero General Public License v3.0](LICENSE).

---

## Кратко по-русски
ProtoOS — экспериментальная минимальная ОС для RISC-V RV64 под QEMU `virt`: ядро в S-mode, до 8 процессов в U-mode, у каждого свои таблицы Sv39, распределитель страниц памяти (страницы обнуляются при выдаче и возвращаются при завершении процесса), запуск и завершение процессов во время работы, вытеснение по таймеру, ввод с клавиатуры по прерываниям (PLIC), изоляция отказов, демо-режим (`protoos.mode=demo`) и командная оболочка `proto>` с командами `run`, `kill`, `ps`, `mem`, `ls` и др. (`protoos.mode=shell`, запуск: `sh build-scripts/run-shell.sh`). С версии 1.4 ОС запускает **программы на C** из архива, который передаётся при загрузке: ядро проверяет каждый файл до загрузки, даёт программе своё адресное пространство с точными правами страниц, стек и кучу; программу на переднем плане останавливает Ctrl-C. В комплекте игра `guess`, `primes` и `clock`. Все 34 обязательные проверки v1.4 (T1–T22, N1–N12) пройдены, по 3 чистых запуска каждая; доказательства получены снаружи (трассы QEMU, GDB) и лежат в `evidence/`. При каждом изменении GitHub Actions заново собирает ОС и архив программ (оба побайтно совпадают с выпущенными) и прогоняет всю приёмку на QEMU 8.2.2. Не проверялось: реальное железо. Проект создан с помощью ИИ (GPT, затем Claude) под руководством владельца. Лицензия — AGPL-3.0.
