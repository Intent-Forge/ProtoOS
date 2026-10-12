# ProtoOS v1.3 — specification addendum

Owner decision (2026-10-12): processes on demand. The kernel gets a physical page allocator, a process table of 8 slots, built-in programs that can be started and stopped at run time, and the shell gets commands for them. The v1.0 documents ([`TECHNICAL_SPECIFICATION_RU.md`](../TECHNICAL_SPECIFICATION_RU.md), [`DECISION_REGISTER_RU.md`](../DECISION_REGISTER_RU.md), [`ACCEPTANCE_MATRIX.csv`](../ACCEPTANCE_MATRIX.csv)) and the addenda [`SPEC_V1_1.md`](SPEC_V1_1.md) and [`SPEC_V1_2.md`](SPEC_V1_2.md) stay unchanged, and **every v1.0–v1.2 requirement and check (T1–T12, N1–N8) still applies to v1.3**. Processes A (pid 1) and B (pid 2) are still created at boot with their static pages and layout.

## Additions
### Page allocator
- The pool is the RAM from `0x80400000` (above the 2 MiB reserved for the kernel image) up to the 2 MiB boundary below the device tree — 31 232 pages of 4 KiB on QEMU `virt` with 128 MiB. A bitmap records which pages are in use; allocation takes the lowest free page.
- **Every page is zeroed when it is handed out**, so nothing written by a previous owner can be read by the next. Freeing a page that is not allocated is a kernel bug and panics.
- The kernel reaches the pool through 2 MiB identity mappings (level-1 leaves) present in every Sv39 root with **U=0**: user mode can reach a pool page only through its own 4 KiB user mapping.

### Process table and shared program pages
- 8 slots: A and B in slots 0 and 1, slots 2–7 for processes created at run time. Slot *s* uses user VAs from `0x400000 × (s + 1)` with the v1.0 layout (data at +0x1000, stack at +0x3000 with sp = +0x4000, guard between) — so all processes keep disjoint user ranges.
- Built-in programs live in 3 shared pages mapped **R X U at `0x4000000` in every process root** (never writable). They are position independent and find their data page in `a0` and their pid in `a1` at entry.
- A run-time process owns 5 pages: its root table, one level-1 and one level-0 table, its data page and its stack page. The kernel and program tables are shared from a template and never freed.
- When a run-time process exits, faults or is killed, all 5 pages are returned at once (before any other process runs). If it is the process whose root is installed, the kernel switches to its own root first.
- Pids increase from 3 and are never reused. An ended process keeps its slot (and appears in `ps` as EXITED or FAULTED) until a new process takes the slot.

### System calls (ProtoOS-specific numbers)
| Call | `a7` | Behaviour |
|---|---|---|
| `spawn(program)` | 1004 | starts program 1 `worker`, 2 `hello`, 3 `counter`, 4 `crash` or 5 `memcheck` in the lowest free slot; returns its pid. Errors: −22 unknown program, −11 no free slot, −12 out of memory |
| `kill(pid)` | 1005 | ends a live run-time process (exit code 137) and returns its pages; returns 0. Errors: −3 no such live process, −1 the caller itself or a boot process (A, B) |
| `meminfo()` | 1006 | returns free pages in `a0` and pool pages in `a1` |
| `slotinfo(slot, buf)` | 1007 | slot 0–7 (else −22); writes pid, state, program, preemptions, system calls, pages owned as six words into 48 bytes of the caller's U+R+W memory (else −14); state 0 = unused slot |

`procinfo` (v1.2) now accepts any pid that has a slot (−22 otherwise).

### Programs
`worker` counts forever without system calls; `hello` prints one line and exits; `counter` prints three lines one second apart and exits; `crash` announces itself and stores into B's data page (unmapped in its root) — a store page fault ends only it; `memcheck` (for tests) checks that its data and stack pages arrived all zero, records the result, then fills both with `0xa5`.

### Shell
New commands: `run <worker|hello|counter|crash>`, `kill <pid>` (prints how many pages came back), `mem` (free pages). `ps` lists every used slot; lines for A and B keep the v1.2 format, lines for run-time processes end with the program name. `help` prints a second line for the new commands. Output of run-time processes appears on the same console as the shell, as on any terminal.

## Additional acceptance checks (3 clean boots each)
| ID | Scenario (`bootargs`) | PASS criterion | Independent evidence |
|---|---|---|---|
| T13 | `protoos.test=mem` | `memcheck` runs twice in the same slot: each instance owns 5 distinct page-aligned pool pages outside the kernel image, marked in the bitmap; free count drops by 5; the second instance receives the very physical data and stack pages the first one filled with `0xa5` and still finds them zero; after both kills free = baseline and allocations = frees | GDB: allocator state and bitmap, process pages, physical page contents; QEMU trace |
| T14 | `protoos.test=spawn` | a spawned worker gets pid 3 and 5 pages, its own Sv39 root, runs in U-mode in the program pages with its stack and data page, is preempted by the timer and keeps counting; PT walk: its data/stack U+R+W on its own pages, program page R X U not W, A/B data unmapped, kernel and UART not user-accessible, its pages reachable only through the U=0 2 MiB map, not mapped in A's or B's root | GDB CPU state and registers, independent Sv39 walks, physical counters, QEMU trace |
| T15 | `protoos.test=kill` | a killed worker is EXITED (137), all 5 pages are back, it is not dispatched again; a new spawn reuses the slot and the same pages; `hello` exits by itself and its pages come back; free = baseline, allocations = frees | GDB process table, bitmap, allocator counters; UART |
| T16 | `protoos.test=multi` | three workers and `crash` (in slot D, between workers): after its fault the scheduler continues with the next READY slot (v1.3.1); the only user exception is crash's store page fault at its store with `stval` = B's data VA; B's data unchanged; crash FAULTED and reclaimed; workers and B keep running | GDB fault frame and states, physical counters, QEMU trace |
| T17 | `protoos.mode=shell` | scripted session `mem`, `run worker`, `run hello`, `ps`, `kill 3`, `run crash`, `ps`, `mem` gives the specified output; the crash is the only fault; all pages free at the end; the shell is BLOCKED at its prompt | UART log, GDB process table and allocator, QEMU trace |
| N9 | `protoos.test=spawn-bad` | spawn of program 0 or 99 → −22; kill of a missing pid → −3, of itself or B → −1; with all 6 run-time slots taken spawn → −11; kill of an already killed pid → −3; then 200 spawn/kill cycles all succeed and no page leaks (free = baseline, allocations = frees, 206 spawned = killed = reclaimed) | A's results in physical memory, allocator and scheduler counters |

v1.3 is accepted when T1–T17 and N1–N9 all PASS in 3 clean boots each.

## v1.3.1 corrections (independent audit)
An independent code audit of v1.3 (OpenAI Codex, read-only) reported two defects; both were confirmed and fixed:
1. **Round Robin position lost after a run-time process ended.** `reclaim()` cleared `current`, so the next scan started at slot 0 instead of after the ended process, favouring low slots. The scheduler now keeps the last dispatched slot (`sched.last_slot`) independently of `current`. T16 now starts the crash between workers (slot D) and additionally requires that the next dispatch after its fault is the next READY slot after it; a build with the old code fails this check.
2. **Pid counter wrap-around.** After 2⁶⁴ − 1 spawns the counter would wrap and reuse pids 0–2. `spawn` now returns −11 when the pid space is exhausted, so pids are never reused. (Not reachable in practice; not exercised by a test, since the harness never writes guest memory.)
