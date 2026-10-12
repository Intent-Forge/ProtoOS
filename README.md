# ProtoOS

[![acceptance](https://github.com/Intent-Forge/ProtoOS/actions/workflows/acceptance.yml/badge.svg)](https://github.com/Intent-Forge/ProtoOS/actions/workflows/acceptance.yml)

An experimental, deliberately minimal but architecturally genuine operating system for **RISC-V RV64**, running on **QEMU `virt`**.

OpenSBI (M-mode) hands over to the ProtoOS kernel (S-mode), which runs two static user processes (U-mode) with their own **Sv39** page tables, **timer-driven preemptive round-robin scheduling** and real `ecall` system calls. It is a teaching/demonstration kernel, not a general-purpose OS.

![ProtoOS running under QEMU: two user processes print via write() and sleep(), process B tries to overwrite A's memory, takes a page fault and is stopped while A keeps running; then timer-driven switching between the processes](docs/demo/protoos-demo.svg)

<sub>Real UART output of the released kernel captured under QEMU (demo mode with its real timing, then the scheduler test replayed at a readable pace). Regenerate with `python3 docs/demo/make_demo.py`.</sub>

## Features (v1.1)
- Monolithic kernel, ~1,900 lines of freestanding C11 and RV64 assembly; no libc. ISA: RV64IM + Zicsr + Zifencei, LP64, no floating point.
- **Virtual memory:** three Sv39 roots (kernel, A, B), 4 KiB pages. Kernel pages are mapped with U=0; each process's pages exist only in its own root. Guard pages below the kernel stack and the user stacks.
- **Processes:** exactly two (A = pid 1, B = pid 2), created at boot. States: READY, RUNNING, SLEEPING, EXITED, FAULTED.
- **Scheduler:** round robin, 10 ms quantum, preemption by the S-mode timer (SBI TIME). A process that never yields is still preempted.
- **System calls** (`a7` = number, `a0`–`a2` = arguments, `a0` = result): `write` (64), `exit` (93), `yield` (124), `getpid` (172), `sleep` (1001, ProtoOS-specific). `write` checks the whole user buffer against the caller's own page table before touching it (kernel pages, the other process's pages and guard pages are rejected with `-14`). Unknown numbers return `-38` and the process continues.
- **Fault isolation:** any other exception from U-mode (page fault, illegal instruction, …) marks only that process FAULTED; the other keeps running. With nothing runnable, the kernel parks safely (timer off, `wfi`).
- **One image for everything:** the same ELF runs the normal mode, the demo mode and every test scenario, selected by `bootargs`.

New in v1.1 (specified in [`docs/SPEC_V1_1.md`](docs/SPEC_V1_1.md)): `write` and `sleep`, the SLEEPING state with an in-kernel wait when every process sleeps, and the demo mode shown above. v1.0 is tagged [`v1.0`](https://github.com/Intent-Forge/ProtoOS/tree/v1.0).

Architecture, memory map and ABI: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) ([Russian](docs/ARCHITECTURE_V1_RU.md)). Original frozen specification: [`TECHNICAL_SPECIFICATION_RU.md`](TECHNICAL_SPECIFICATION_RU.md), [`DECISION_REGISTER_RU.md`](DECISION_REGISTER_RU.md).

## What has been verified — and what has not
v1.0 defines 12 mandatory checks in [`ACCEPTANCE_MATRIX.csv`](ACCEPTANCE_MATRIX.csv): T1–T7 (S-mode boot, two U-mode contexts, timer preemption, `getpid`/`yield`/`exit`, Sv39 isolation, fault isolation, autonomous normal run) and N1–N5 (cross-process write, user read of kernel memory, privileged instruction from U-mode, unknown syscall, no runnable process). v1.1 adds T8 (`write`), T9 (`sleep`, including both processes asleep) and N6 (rejected `write` buffers), see [`docs/SPEC_V1_1.md`](docs/SPEC_V1_1.md).

| Check | Result |
|---|---|
| **v1.1:** T1–T9, N1–N6, 3 clean boots each (60 boots), QEMU 10.2.1 + OpenSBI 1.8 + gdb 17.1 | **all PASS** — [`evidence/acceptance/20261012T053626Z/RESULTS.json`](evidence/acceptance/20261012T053626Z/RESULTS.json); a 200 ms `sleep` measured 200.9–201.1 ms |
| v1.0: T1–T7, N1–N5 ×3 (48 boots); same matrix on OpenSBI 1.4 | **all PASS** — [`evidence/acceptance/20261011T231210Z/`](evidence/acceptance/20261011T231210Z/RESULTS.json), [`evidence/crosscheck-opensbi-1.4/`](evidence/crosscheck-opensbi-1.4/) |
| CI on every push: stock Ubuntu 24.04 runner, QEMU 8.2.2, OpenSBI 1.7, gdb 15 — full matrix ×3 | **all PASS** — see the badge and [Actions](https://github.com/Intent-Forge/ProtoOS/actions/workflows/acceptance.yml) (evidence attached to each run) |
| Rebuild of the ELF on the CI runner and by a second independent reviewer | byte-identical to `release/protoos.elf` |
| Real hardware, other emulators | **not run** |

Evidence is collected from outside the kernel, never from its own messages: the QEMU interrupt trace (`-d int`), GDB (privilege level, CSRs, registers, physical memory, an independent Sv39 page-table walk) and QMP sampling for the normal mode. Every boot directory under `evidence/` holds the exact QEMU command line, raw logs, observations and a per-check verdict. The normal-mode check (T7) is a 3 × 30 s bounded observation (~3,075 timer switches per run), not a proof of unlimited stability.

ELF v1.1: [`release/protoos.elf`](release/protoos.elf), SHA-256 `129fce46eea8597645192aabe225682353f695ca420285e71efe69afdfab687e` (v1.0: `f1d5a5bc…`, tag `v1.0`).

## Build and run
Tested on Ubuntu 26.04 under WSL2; any recent x86-64 Ubuntu/Debian should work.

```sh
sudo apt install qemu-system-riscv opensbi gdb-multiarch binutils-riscv64-unknown-elf \
                 python3 python3-venv build-essential
sh build-scripts/setup-zig.sh            # pinned Zig 0.13.0 (hash-checked) into ~/.protoos-venv
sh build-scripts/build-wsl.sh            # -> build/protoos.elf + SHA-256
sh build-scripts/run-scenario.sh protoos.mode=demo 8         # demo: processes print, B attacks A
sh build-scripts/run-scenario.sh protoos.mode=normal 10      # normal mode for 10 s
sh build-scripts/run-scenario.sh protoos.test=isolation 3    # A writes into B -> page fault
python3 validation/run_acceptance.py     # full acceptance, 3 clean boots per check (~4 min)
sh build-scripts/host-tests.sh           # host tests: Sv39 table builder, DT parser
```
On older Ubuntu releases QEMU for RISC-V is in `qemu-system-misc`. Other scenarios: `umode`, `preempt`, `getpid`, `yield`, `exit`, `kernel-read`, `fault`, `csr`, `bad-syscall`, `idle`, `idle-fault`, `write`, `write-bad`, `sleep`, `sleep-all`.

## Not included (by design)
No shell, file system, networking, drivers beyond the UART, dynamic processes, SMP, POSIX or user-mode floating point.

## How it was made
ProtoOS was built with AI assistance under the direction of the project owner: the specification and earlier stages with OpenAI GPT models, the v1.0/v1.1 kernel, test harness and releases with Anthropic Claude. The owner set the goals and made the decisions; the frozen specification and the decision register record the AI roles as they were at the time.

## License
[GNU Affero General Public License v3.0](LICENSE).

---

## Кратко по-русски
ProtoOS — экспериментальная минимальная ОС для RISC-V RV64 под QEMU `virt`: ядро в S-mode, два процесса в U-mode с отдельными таблицами Sv39, вытеснение по таймеру, системные вызовы `write`/`sleep`/`getpid`/`yield`/`exit`, изоляция отказов, демо-режим (`protoos.mode=demo`). Все 15 обязательных проверок v1.1 (T1–T9, N1–N6) пройдены, по 3 чистых запуска каждая; доказательства получены снаружи (трассы QEMU, GDB) и лежат в `evidence/`. При каждом изменении GitHub Actions заново собирает ОС (ELF побайтно совпадает с выпущенным) и прогоняет всю приёмку на QEMU 8.2.2. Не проверялось: реальное железо. Проект создан с помощью ИИ (GPT, затем Claude) под руководством владельца. Лицензия — AGPL-3.0.
