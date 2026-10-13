# ProtoOS v1.5 — specification addendum

Owner decision (2026-10-13, "Утверждаю", plan revision 3): **disk and files**. A virtio-blk disk in QEMU with a small flat file system (PFS), programs to list, show, write and remove files, `run <program> <argument>`, and programs started from the disk. Crash safety within a stated fault model: every file is entirely old or entirely new. The v1.0 documents and the addenda [`SPEC_V1_1.md`](SPEC_V1_1.md) … [`SPEC_V1_4.md`](SPEC_V1_4.md) stay unchanged, and **every v1.0–v1.4 requirement and check (T1–T22, N1–N12) still applies to v1.5**. Booted without a disk, v1.5 behaves like v1.4 (the only differences are noted below).

Owner decisions on the plan: **A** — the argument comes with a new call `run_arg` (1017); `run` (1008) keeps its v1.4 meaning and inside the kernel is `run_arg` with no argument (one code path); the shell and the C library use only 1017. **B** — `disk.img` is a release file in `release/SHA256SUMS`, byte-reproducible like the kernel and the program archive. **C** — a disk without `VIRTIO_BLK_F_FLUSH` is mounted read-only even if it is write-through.

## The disk
- QEMU: `-drive if=none,file=disk.img,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 -global virtio-mmio.force-legacy=false` (`build-scripts/run-shell.sh` does this with a copy of `build/disk.img`).
- The kernel finds the virtio-mmio transports in the device tree (`/soc/virtio_mmio@*`, compatible `virtio,mmio`, a 16-byte `reg` of at most one page, page aligned, below 4 GiB; at most 8 are kept, in DT order). Only transports inside the UART's 2 MiB are used and mapped (QEMU virt: `0x10001000`–`0x10008000`); their pages are mapped kernel-only (U=0) in every root, sharing the UART's level-0 table. No address is assumed: QEMU 8.2.2 puts the first `-device` at `0x10008000`.
- The first transport with magic `virt`, device ID 2 is the disk. Version 1 (legacy) is refused with a boot line; only virtio-mmio version 2 (virtio 1.x) is used.

### Driver (`src/virtio_blk.c`)
- Init as virtio 1.2 §3.1.1: reset (bounded wait for status 0) → ACKNOWLEDGE → DRIVER → features → FEATURES_OK, read back → queue 0 (size 8, or 4 if QueueNumMax < 8; a split queue's size is a power of two; QueueNumMax < 4 fails) → DRIVER_OK. Negotiated: `VIRTIO_F_VERSION_1` (required), and `VIRTIO_BLK_F_RO` / `VIRTIO_BLK_F_FLUSH` when offered; nothing else. Capacity is read until the configuration generation is stable.
- One split virtqueue, one request at a time, polled; every wait is bounded (2 s on the `time` CSR). Fences around the ring updates and the notify.
- Descriptors point only at kernel memory in the identity-mapped kernel image: the 16-byte request header, a 4 KiB bounce buffer, and the status byte. Never at user memory.
- 4 KiB blocks (8 sectors of 512 bytes): read, write, flush. A timeout, a non-zero request status, or NEEDS_RESET/FAILED in the device status marks the device failed until reboot (its buffers, descriptors and request header are never written again); every later request returns EIO.
- No interrupts: the scheduler pauses while a request runs. Every file system call runs whole in the kernel with interrupts off, so no other process runs during it.

### Boot lines
| Situation | Line |
|---|---|
| no virtio block device | nothing (as v1.4) |
| a legacy (version 1) block device | `DISK refused: legacy virtio-mmio block device at 0x…` |
| init failed | `DISK failed: virtio-blk at 0x… reason=N` |
| device ready | `DISK virtio-blk at 0x…: <sectors> sectors, flush` or `no flush`, then `, read-write` or `, read-only` |
| file system mounted | `DISK <blocks> blocks, <files> files` |
| file system refused | `DISK rejected code=-N` (table below); the system runs without a disk |

## PFS, the ProtoOS file system (`src/pfs.c`)
Pure code over a block interface (read, write, flush, capacity, read-only), host-tested with a simulated faulty disk. All integers little-endian, 4 KiB blocks.

| Block | Contents |
|---|---|
| 0 | superblock: `PFS1`, version 1 (u32), block count (u32), CRC-32 (u32) of the block with this field zero; the other 4080 bytes zero |
| 1 | directory copy A |
| 2 | directory copy B |
| 3 … | file data: every file is one contiguous extent of whole blocks, zero-padded |

Directory copy: `PFSDIR01`, seq (u64), CRC-32 (u32, of the block with this field zero), 44 zero bytes, then 63 entries of 64 bytes. An entry: name (16 bytes: 1–15 characters `a–z 0–9 _ .`, NUL padded), size (u32, ≤ 128 KiB), first block (u32), block count (u32 = ⌈size / 4096⌉; 0 for an empty file, whose first block is 0), CRC-32 of the data (u32), 32 zero bytes. An unused entry is 64 zero bytes. CRC-32 is IEEE 802.3 (zlib's). Disk sizes 1–64 MiB (256–16384 blocks); 63 files; files up to 128 KiB.

### Mount
The superblock is checked, then both copies; the copy used is the valid one with the higher seq (equal seq: A). A copy is valid if its magic, CRC and reserved bytes are right and every entry is valid: name, reserved bytes, size and block count, extent inside blocks 3 … count−1; then, pair by pair in slot order, no two equal names and no two overlapping extents. `mkpfs.py` writes both copies, A with seq 1 and B with seq 0. Refusal codes (copy A's code when neither copy is valid):

| Code | Defect | Code | Defect |
|---|---|---|---|
| 1 | superblock unreadable | 11 | directory magic |
| 2 | superblock magic | 12 | directory CRC |
| 3 | version | 13 | nonzero reserved bytes (directory header or entry) |
| 4 | superblock CRC | 14 | bad name, or bytes after its NUL |
| 5 | block count outside 256–16384 | 15 | two equal names |
| 6 | block count larger than the device | 16 | extent outside the data blocks (or an empty file with a first block) |
| 7 | nonzero reserved superblock bytes | 17 | size over 128 KiB or block count ≠ ⌈size/4096⌉ |
| 10 | directory unreadable | 18 | overlapping extents; 19: an unused entry that is not all zero |

### Changing a file
`fs_write` replaces a file whole, `fs_delete` removes it: (1) the data goes to blocks that are free in the mounted directory (first fit; the old version of the file keeps its blocks); (2) flush; (3) the new directory (seq + 1) goes into the copy that is **not** the mounted one; (4) flush; only then is the operation reported done and the new copy becomes the mounted one. A failed write or flush returns EIO and stops the file system (EIO for every call until reboot). A mounted directory whose seq is already 2^64−1 takes no more changes (−28): seq + 1 would wrap to 0 and lose to the old copy at the next mount. There is no defragmentation: a file needs one free extent, otherwise ENOSPC.

### Reading
Every `fs_read` reads the whole file and checks its CRC before it copies a byte (at most 32 block reads): a damaged file gives EIO, even for a read of a part that is not damaged, and the file system goes on. Reading a file in many small pieces therefore costs O(n²); `cat`, `note` and the library read in one call. There is no cache.

### Fault model
- Writes not yet confirmed by a flush may be lost, reordered, or torn per 512-byte sector (each sector old, new or garbage).
- After a successful flush, every write completed before it is durable.
- Out of scope: a disk that lies about flush, decay of durable data (detected by the CRC → EIO, not repaired), hardware failure.

Within this model every file is entirely old or entirely new after a stop at any moment: a torn or lost directory write fails its CRC or keeps the old copy, and every valid copy points only at data that was durable before it was written.

## System calls (ProtoOS-specific numbers)
Names are pointer + length. Arguments are checked first (−22, −14), then the disk (−19 without a mounted disk).

| Call | `a7` | Behaviour |
|---|---|---|
| `fs_list(index, buf)` | 1012 | name of the `index`-th file in directory order (16 bytes, NUL padded) into U+R+W `buf`; returns its size. −2 past the end, −14, −19 |
| `fs_read(name, namelen, off, buf, len)` | 1013 | bytes `[off, off+len)` of the file into U+R+W `buf` after the whole-file CRC check; returns the count (0 at or after the end). −22 bad name or `len` > 128 KiB, −14, −19, −2, −5 |
| `fs_write(name, namelen, buf, len)` | 1014 | create or replace the file with U+R `buf[0..len)`; returns 0 once the new directory is durable. −22, −27 `len` > 128 KiB, −14, −19, −30 read-only, −28 no free extent or 63 files, −5 |
| `fs_delete(name, namelen)` | 1015 | same path; −22, −19, −30, −28 (seq at its maximum), −2, −5 |
| `args(buf, len)` | 1016 | the caller's argument line (no NUL, at most `len` bytes) into U+R+W `buf`; returns its full length (0 if started by 1008). −14 |
| `run_arg(name, namelen, arg, arglen)` | 1017 | as `run`, with an argument line of at most 55 bytes (the shell line is 63). −22 `arglen` > 55, −14 bad `arg` |

`run` and `run_arg` look in the program archive first, then on the disk: a program from the disk is read whole into a 128 KiB static kernel buffer (CRC checked; −5 if damaged), then `elf_check` and the loader run exactly as for the archive; the 64-page limit is unchanged. In `slotinfo` the program of a process started from disk entry *n* is 256 + *n*. New error numbers: −5 EIO, −19 ENODEV, −27 EFBIG, −28 ENOSPC, −30 EROFS. The numbering rule is in [`ARCHITECTURE.md`](ARCHITECTURE.md): Linux numbers only where the meaning matches, otherwise ≥ 1000, and numbers never change.

## Programs and shell
- `release/programs.img` adds `files` (each file and its size), `cat <file>`, `note <file>` (type lines; an empty line saves the file whole) and `rm <file>`. `release/disk.img` (1 MiB) holds `hello`, which prints its argument: `run hello world` starts it from the disk.
- The C library adds `sys_run_arg`, `get_arg`, `fs_list`, `fs_read`, `fs_write`, `fs_delete`, `print_error` and a 5-argument `proto_call5`; `sys_run` now uses 1017 with an empty argument.
- Shell (`src/user.S`, the only user.S changes): `run <program> <argument>` splits the line at the first space (a trailing ` &` is removed first) and always calls 1017; `ps` shows a program from the disk as `(disk)` (its name is not in the archive); `help` names the disk programs (its first line is unchanged).
- Build: `build/disk.img`, `build/disks/` (test disks, 22 malformed images in `bad/`, `fallback.img`), and the `disks` section of `images.json` with the code each malformed image must get. `build-scripts/mkpfs.py` makes images; `build-scripts/pfsck.py` is an independent checker written from this specification.

## Harness changes
- Since commit faab424 (before v1.5): the harness repeats a SIGINT that GDB dropped, and measures waits on the monotonic clock.
- T22 takes the number of programs in `programs.img` from `images.json` (3 in v1.4, 7 in v1.5); the check is otherwise unchanged.
- The harness copies the disk image for every boot (T25 and T27: one copy per repetition, shared by its boots), runs `pfsck.py` on it after the boot and keeps its SHA-256 and pfsck's verdict in the evidence (not the image).

## Additional acceptance checks (3 clean boots each)
| ID | Scenario (`bootargs`, archive, disk) | PASS criterion | Independent evidence |
|---|---|---|---|
| T23 | `protoos.mode=shell`, `programs.img`, `disk.img` | the 8 DT transports found; the device in use is one of them: magic, version 2, ID 2, status DRIVER_OK (read physically); capacity = image size; negotiated exactly VERSION_1 + FLUSH; queue 8; mount read exactly 3 blocks and wrote none; every request completed; descriptors point only at the kernel's header, bounce buffer and status byte; the kernel's directory = pfsck's; every transport page mapped with U=0 in the kernel, A and B roots; boot lines; image unchanged | GDB (driver, virtqueue, MMIO registers, directory), page-table walks, pfsck, UART |
| T24 | `protoos.test=fs` (`t_fs`), `tests.img`, empty 1 MiB disk; `protoos.test=fs-ro` (`t_fsro`) on `ro.img` with no flush, and with `readonly=on` | sizes 0, 1, 4095, 4096, 4097, 128 KiB written and read back; 128 KiB + 1 → −27; partial reads and a read past the end; list; replace; delete; disk full → −28 after exactly 6 files of 128 KiB; the 64th file → −28; 135 directory updates (seq 136); the image after the boot holds exactly the expected files and bytes. Read-only disks: reads work, every change −30, no write or flush reaches the device, the image is byte-identical | the driver's results read through its page table at its `exit`, GDB, pfsck and SHA-256 of every file on the image |
| T25 | `persist-w` then `persist-r` on the same disk copy | boot 1 writes, replaces, deletes; boot 2 sees exactly the result (seq 6) and changes nothing; pfsck after each boot | results, GDB, pfsck, image SHA-256 |
| T26 | `protoos.mode=shell`, `programs.img`, `disk.img` | `run hello world` starts the program from disk entry 0 with argument `world`; `files`, `note memo` (two lines), `cat memo`, `note tmp`, `rm tmp`, `files`, `cat nosuch`: specified output; 3 directory updates; the image holds exactly `hello` (unchanged) and `memo` | UART, GDB process table (program 256, argument), pfsck |
| T27 | `protoos.test=crash` (`t_crash`), one disk copy for 20 boots | 20 boots, each killed (SIGKILL) at a seeded random moment while `t_crash` replaces two files in a loop: after every kill pfsck mounts the image, every file is intact and is one whole generation; a write reported saved is on the disk; the next boot continues from it; progress. A smoke test of the whole system: a killed QEMU keeps the writes already in the host page cache, so it cannot show lost unconfirmed writes — the host tests prove the fault model | pfsck and the files' content after every kill, UART, seed in the evidence |
| N13 | `protoos.mode=shell` on each of 22 malformed images, and `fallback.img` | each refused with exactly its code (kernel state, boot line, and pfsck agree); the device itself was fine; the system runs on; the image unchanged. `fallback.img` (copy A damaged): copy B mounted | GDB disk state, UART, pfsck |
| N14 | `protoos.test=fs-bad` (`t_fsbad`), `crcbad.img` | bad lengths and names −22; kernel pointers for name, data, output, list buffer, argument and `args` −14; 128 KiB + 1 → −27; argument of 56 bytes −22; a 100-byte read at offset 0 of a file damaged in its last block −5, and at its end −5; good files still read; a broken ELF on the disk −8 (refused by `elf_check`), a damaged program −5; no page leaked; writing still works | results, GDB, pfsck (the same two files damaged) |
| N15 | `protoos.test=no-disk` (`t_nodisk`) without a disk, and with a legacy virtio-mmio device | every fs call −19; `run` from the archive works (42); no driver state and no `DISK` line without a disk; the legacy device refused with exactly its line, its image unchanged | results, GDB, UART |

v1.5 is accepted when T1–T27 and N1–N15 all PASS in 3 clean boots each.

**Host tests** (`tests/test_pfs.c`, plain and with ASan/UBSan) are the main, deterministic proof of the fault model: a scripted run of creates, replaces and deletes over several files is stopped at every device call; the unflushed writes are then lost, kept, kept in reverse order, kept with the last write stopped after each of its 8 sectors, or kept/lost/torn at random per sector (old, new or garbage); a failed write or flush is injected at every call. After every stop the image is mounted again and must hold exactly the directory before or after the interrupted operation, every file with the content of that state, and must accept new writes. Also: API behaviour, 4000 fuzzed superblocks and directories (no crash), and `tests/test_pfsck_agree.py`: pfsck and the kernel's mount agree on every fuzzed image and on every image the build makes. `tests/test_fdt.py` adds the virtio-mmio DT cases.

**Mutants**: 10 kernels with one plausible bug each (directory written before the data, data flush skipped, the mounted copy overwritten, no CRC check, no overlap check, a block-range off-by-one, the `fs_read` buffer not checked, the seq choice reversed, a disk without flush writable, a legacy device accepted) must each fail the check named for them.

## Honest limits
- QEMU kills simulate the system stopping, not a disk tearing a sector or losing its cache; the host tests cover the fault model. Real hardware (SD, NVMe) is not claimed.
- Mutants show the checks catch typical bugs; they do not prove the model complete.
- Out of scope: folders, permissions, users, timestamps, file descriptors (v1.6), pipes, append or edit in place, caching, disk interrupts, several disks, journaling beyond the two copies, defragmentation.
- Without a disk, v1.5 differs from v1.4 only in its page tables: the virtio-mmio transport pages of the device tree are mapped kernel-only.
