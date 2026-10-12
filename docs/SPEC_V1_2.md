# ProtoOS v1.2 — specification addendum

Owner decision (2026-10-12): make ProtoOS interactive. The kernel gets console input through the UART receive interrupt, a blocking `read`, a time source and process information, and process A can run a small command shell. The v1.0 documents ([`TECHNICAL_SPECIFICATION_RU.md`](../TECHNICAL_SPECIFICATION_RU.md), [`DECISION_REGISTER_RU.md`](../DECISION_REGISTER_RU.md), [`ACCEPTANCE_MATRIX.csv`](../ACCEPTANCE_MATRIX.csv)) and the v1.1 addendum ([`SPEC_V1_1.md`](SPEC_V1_1.md)) stay unchanged, and **every v1.0 and v1.1 requirement and check (T1–T9, N1–N6) still applies to v1.2**.

## Additions
### Device interrupts (QEMU `virt`)
- The UART (16550, `0x10000000`) raises its receive interrupt through the PLIC (`0x0C000000`, source 10, hart 0 S-mode context). The kernel maps only the three PLIC pages it uses (priority, S enable, S threshold/claim) into every Sv39 root, kernel-only (U=0) like the UART.
- `sie.SEIE` is enabled together with `sie.STIE` when the processes start, in every scenario. The kernel itself still never sets `sstatus.SIE`: an external interrupt is taken only from U-mode, or noticed by polling `sip.SEIP` while the kernel waits.
- On an external interrupt the kernel claims the PLIC source, moves every received byte into a fixed 128-byte input buffer and completes the claim. When the buffer is full the new byte is dropped (counted); bytes already buffered are never lost or reordered. The interrupted process continues; readers are woken at the next scheduling point (at most one 10 ms quantum later).

### `read(fd, buf, len)` — `a7 = 63`
- `fd` must be 0 (the console); otherwise `-9`. `len` ≤ 256; otherwise `-22`. Every byte of `[buf, buf + len)` must lie in pages of the calling process's own root that are valid, user (U), readable (R) **and writable (W)**; otherwise `-14`. All checks happen **before** the call can block. `len = 0` returns 0 at once.
- With input buffered, `read` copies up to `len` bytes at once and returns their number (≥ 1). Otherwise the process enters the new state **BLOCKED** and is not scheduled until input arrives; then the kernel copies the bytes into its buffer (with `sstatus.SUM` set only for the copy, under the reader's own root) and `read` returns their number.
- If every live process is SLEEPING or BLOCKED, the kernel waits in S-mode (`wfi`, `sstatus.SIE` = 0) until the earliest wake-up or until input arrives for a blocked reader. `sched_idle` is entered only when no process is READY, RUNNING, SLEEPING or BLOCKED.

### `uptime()` — `a7 = 1002` (ProtoOS-specific)
Returns the microseconds of the `time` CSR elapsed since kernel entry.

### `procinfo(pid, buf)` — `a7 = 1003` (ProtoOS-specific)
`pid` 1 or 2 (otherwise `-22`); `buf` must be 32 bytes of U+R+W memory of the caller (otherwise `-14`). Writes four 64-bit words — state, preemptions, system calls, dispatches of that process — and returns 0. States: 1 READY, 2 RUNNING, 3 EXITED, 4 FAULTED, 5 SLEEPING, 6 BLOCKED.

### `write`: backspace
`write` now also passes byte `0x08` (backspace) to the UART, so the shell can erase characters. Everything else in `write` is unchanged.

### Shell — `bootargs = protoos.mode=shell`
Process A runs `proto>`, a line-oriented shell in U-mode: it echoes typed characters, supports backspace, treats CR, LF or CR LF as Enter, and ignores other control and non-ASCII bytes and characters beyond 63 per line. Commands: `help`, `ps` (state, preemptions and system calls of both processes via `procinfo`), `uptime`, `echo <text>`, `attack` (asks the kernel to `write` 8 bytes of B's memory; the kernel's page-table check refuses with `-14`). Process B runs the v1.0 worker in the background the whole time. While the shell waits for a key, A is BLOCKED and uses no CPU time.

## Additional acceptance checks (3 clean boots each)
In these scenarios the harness connects the guest UART to a UNIX socket and types into it like a terminal user; the UART output is still logged to `uart.log`.

| ID | Scenario (`bootargs`) | PASS criterion | Independent evidence |
|---|---|---|---|
| T10 | `protoos.test=read` | A's `read(0, buf, 16)` traps with `a7=63` on a buffer the PT walk shows as U+R+W; without input A is BLOCKED and **not dispatched for 30 timer ticks** while B runs; typed bytes arrive by S external interrupts taken from U-mode, A returns at `ecall+4` with the byte count, the bytes are in its buffer, `SUM=0`; works again for a second input | GDB trap frame, PT walk, process state and dispatch counts, kernel input counters, A's physical memory, QEMU trace (`async` cause 9) |
| T11 | `protoos.test=uptime` | two `uptime` values around `sleep(100)` match the kernel's own time stamps (within one quantum) and differ by ≥ 100 ms and ≤ 100 ms + 2 quanta | GDB registers, kernel `boot_time`, sleep/dispatch stamps |
| T12 | `protoos.mode=shell` | a scripted session (`help`, `ps`, `uptime`, `echo …`, `attack`, `ps`, `uptime`) produces the specified output with one prompt per command; at the prompt A is BLOCKED and not dispatched for 50 ticks; B keeps being preempted; `attack` is refused with −14 and nothing faults; the kernel received exactly the typed bytes | UART log, GDB process states, kernel counters, QEMU trace |
| N7 | `protoos.test=read-bad` | wrong `fd` → −9; kernel page, other process's page, A's own read-only text page, page-crossing into a guard page, NULL and overflowing address → −14; `len > 256` → −22; `len = 0` → 0; none of these blocks or consumes input; the following valid `read` blocks and returns the typed byte; A is not faulted | per-call results in A's physical memory, PT walk, process state, QEMU trace |
| N8 | `protoos.test=read-flood` | while A sleeps, 300 bytes are typed: the kernel takes all 300 from the UART, keeps exactly the first 128 in order, drops 172, does not panic; A's next read returns those 128 bytes, the following read blocks and then returns a byte typed after the flood; nothing faults | kernel input counters, A's physical memory, process states, QEMU trace |

v1.2 is accepted when T1–T12 and N1–N8 all PASS in 3 clean boots each.
