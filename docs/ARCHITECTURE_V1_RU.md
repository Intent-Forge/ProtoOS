# ProtoOS v1.0 — архитектура и реализация

Нормативная база: Frozen Specification v1.0 (`TECHNICAL_SPECIFICATION_RU.md`) и решения 01–19. Ниже — технические уточнения, которые спецификация оставляет исполнителю (§0, «Технические уточнения без нового решения»). English version: [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Платформа
QEMU `virt`, `-cpu rv64 -smp 1 -m 128M`, OpenSBI `fw_dynamic.bin` (M) → ядро в S-mode по `0x80200000`. ISA ядра и пользователя: RV64IM + Zicsr + Zifencei, LP64, без FP. В U-mode `sstatus.FS=VS=Off`: любая FP/vector-инструкция → illegal instruction → FAULTED (FP-состояние не хранится, §3).

## Физическая память (статическая, §4)
| Диапазон | Содержимое | Отображение |
|---|---|---|
| `__kernel_start` … `__text_end` | код ядра | все корни: R X, U=0 |
| … `__rodata_end` | rodata (в т.ч. `kernel_secret` для N2) | R, U=0 |
| … `__kernel_end` | data/bss: таблицы страниц, `procs`, стеки | R W, U=0 |
| `kernel_stack_guard` (4 KiB) | страница под стеком ядра | **не отображается** |
| `__user_a_text`, `__user_b_text` | по одной странице кода A и B | только в корне владельца, U=1 |
| `user_{a,b}_{data,stack}` | данные и стек A и B | только в корне владельца, U=1 |
| `0x10000000` | UART 16550 | R W, U=0 |

Пользовательские страницы лежат за `__kernel_end` и **не входят** в тождественное отображение ядра: ни ядро, ни другой процесс их не видят.

## Виртуальная память
Три корня Sv39 (`vm.c`): ядро, A, B; 4 KiB листы, A/D выставлены программно (не полагаемся на Svadu). ASID=0, при каждой смене `satp` — `sfence.vma zero, zero` до и после записи.

| VA процесса | A | B | Права |
|---|---|---|---|
| text | `0x400000` | `0x800000` | R X U |
| data | `0x401000` | `0x801000` | R W U |
| guard | `0x402000` | `0x802000` | не отображена |
| stack | `0x403000` (sp=`0x404000`) | `0x803000` (sp=`0x804000`) | R W U |

## Ловушки
Один вектор `trap_entry` (`trap.S`). Протокол `sscratch`: в U-mode — адрес `struct trap_frame` текущего процесса, в ядре — 0. Ядро никогда не включает `sstatus.SIE`, поэтому ловушка при `sscratch==0` — синхронное исключение S-mode → аварийный стек → `KERNEL_PANIC` (§6). Из U-mode сохраняются x1–x31, `sepc/sstatus/scause/stval`; возврат — `user_return` (`sret`).

## Планировщик (§3)
Round Robin, квант 10 мс (`timebase/100`), таймер через SBI TIME (`sbi_set_timer`). Состояния: READY, RUNNING, EXITED, FAULTED. Если READY нет — `sched_idle`: таймер выключен, `sie=0`, корень ядра, `wfi`-цикл в `halt()`; контекст процесса больше не загружается.

## Системные вызовы (§5)
`ecall` из U-mode, номер в `a7`, аргумент/результат в `a0`, возврат на `sepc+4`.

| Вызов | a7 | Семантика |
|---|---|---|
| `exit(code)` | 93 | процесс → EXITED, не возвращается |
| `yield()` | 124 | → READY, переключение; возвращает 0 |
| `getpid()` | 172 | A=1, B=2 |
| неизвестный | любой другой | `a0 = -38` (ENOSYS), процесс продолжает работу |

Любое другое синхронное исключение из U-mode (page/access fault, illegal instruction, misaligned, breakpoint) → процесс FAULTED, остальные продолжают работу (§6).

## Режимы одного образа (§7, решение 14)
Сценарий выбирается только `bootargs` (DT `/chosen`), ELF один и тот же.

| bootargs | A | B | Назначение |
|---|---|---|---|
| `protoos.mode=normal` | worker | worker | Normal, T7 |
| `protoos.test=boot` | — | — | T1 (S-mode, Sv39 активен) |
| `protoos.test=boot-trap` | — | — | фатальная ловушка S-mode (диагностика) |
| `protoos.test=umode` / `preempt` | worker | worker | T2 / T3 |
| `protoos.test=getpid` / `yield` / `exit` | … | … | T4 |
| `protoos.test=isolation` | запись в страницу B | worker | T5, N1 |
| `protoos.test=kernel-read` | чтение `kernel_secret` | worker | T5, N2 |
| `protoos.test=fault` | запись в свой код | worker | T6 |
| `protoos.test=csr` | `csrw satp` | worker | N3 |
| `protoos.test=bad-syscall` | `a7=999` | worker | N4 |
| `protoos.test=idle` / `idle-fault` | exit / fault | exit | N5 |
| `protoos.demo=console` | — | — | демо-консоль D1 (до включения MMU) |

`worker` — бесконечный цикл без системных вызовов, который сам проверяет инварианты регистров (`s0+s1==0`, `s2..s10` = шаблон) и canary-область данных.

## Сборка и проверка
```
sh build-scripts/build-wsl.sh            # Zig 0.13.0 → build/protoos.elf
sh build-scripts/host-tests.sh           # C1b builder + FDT (host)
sh build-scripts/run-scenario.sh protoos.mode=normal 5
python3 validation/run_acceptance.py     # T1–T7, N1–N5 × 3 → evidence/acceptance/<run>/
```
