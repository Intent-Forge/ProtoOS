#!/usr/bin/env python3
"""Record real ProtoOS UART output under QEMU and render an animated SVG.

Usage (from the repository root, after build-scripts/build-wsl.sh):
    python3 docs/demo/make_demo.py            # -> docs/demo/protoos-demo.svg

Every line shown is captured from the guest's UART in a real QEMU run. The
demo-mode and shell scenes keep the real timing between lines (clamped); the
other scenes are replayed at one line per PACE seconds so they can be read.
In the shell scene the script types the commands into the guest UART.
"""
import html
import os
import pathlib
import select
import shutil
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
ELF = ROOT / 'build/protoos.elf'
PROGRAMS = ROOT / 'build/programs.img'
DISK = ROOT / 'build/disk.img'          # copied: the demo never writes the built image
OUT = ROOT / 'docs/demo/protoos-demo.svg'
FW = os.environ.get('OPENSBI', '/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin')

# Keystrokes for the shell scene: (seconds after the banner, bytes typed).
SHELL_TYPING = [(1.0, b'help\r'), (2.3, b'run counter\r'), (3.4, b'run worker\r'),
                (4.7, b'ps\r'), (6.1, b'kill 4\r'), (7.3, b'run crash\r'), (8.9, b'mem\r')]
# v1.4: programs in C from the program archive; \x03 is Ctrl-C. GUESS types
# the next guess of a binary search over the game's answers seen so far
# (skipped once the number is found).
GUESS = b'GUESS'
PROGRAM_TYPING = [(1.0, b'ls\r'), (2.2, b'run primes\r'), (3.6, b'run clock\r'),
                  (6.8, b'\x03'), (7.8, b'run guess\r')] + \
                 [(9.0 + 1.1 * k, GUESS) for k in range(7)] + [(17.0, b'mem\r')]
# v1.5: files on the virtio-blk disk, then a second boot on the same disk.
DISK_TYPING = [(1.0, b'run files\r'), (2.2, b'run hello world\r'), (3.4, b'run note todo\r'),
               (4.6, b'water the plants\r'), (5.8, b'plan v1.6\r'), (7.0, b'\r'),
               (8.2, b'run cat todo\r'), (9.4, b'run files\r')]
REBOOT_TYPING = [(1.0, b'run cat todo\r'), (2.2, b'ps\r')]
# (bootargs, seconds to run, max captured lines, comment, real timing?, typing, archive,
#  disk: None, 'fresh' (a new copy of disk.img) or 'same' (the previous scene's copy))
SCENES = [
    ('protoos.mode=demo', 9, 22,
     'demo mode: A and B print via write() and sleep(); then B attacks A', True, None, None, None),
    ('protoos.mode=shell', 11, 32,
     'shell mode: A is a command shell; it starts and stops processes on demand',
     True, SHELL_TYPING, None, None),
    ('protoos.mode=shell', 19, 40,
     'v1.4: programs written in C, loaded from the program archive; Ctrl-C stops them',
     True, PROGRAM_TYPING, PROGRAMS, None),
    ('protoos.mode=shell', 12, 34,
     'v1.5: files on the disk (virtio-blk + PFS); hello is a program on the disk, not in the archive',
     True, DISK_TYPING, PROGRAMS, 'fresh'),
    ('protoos.mode=shell', 4, 18,
     'v1.5: a new boot on the same disk: the file written before is still there',
     True, REBOOT_TYPING, PROGRAMS, 'same'),
    ('protoos.test=umode', 2, 15,
     'two user processes, switched by the timer interrupt every 10 ms', False, None, None, None),
]
PACE, COMMAND_PAUSE, SCENE_HOLD = 0.22, 0.9, 2.6
COLS, ROWS = 112, 19
FONT, LINE_H, CHAR_W, PAD, TITLE_H = 13, 18, 7.82, 14, 28


def record(args, seconds, max_lines, typing=None, initrd=None, disk=None):
    cmd = ['qemu-system-riscv64', '-machine', 'virt', '-accel', 'tcg', '-cpu', 'rv64',
           '-smp', '1', '-m', '128M', '-bios', FW, '-kernel', str(ELF)]
    cmd += ['-initrd', str(initrd)] if initrd else []
    cmd += ['-drive', f'if=none,file={disk},format=raw,id=hd0', '-device',
            'virtio-blk-device,drive=hd0', '-global', 'virtio-mmio.force-legacy=false'] if disk else []
    cmd += ['-display', 'none', '-monitor', 'none', '-serial', 'stdio', '-nic', 'none',
            '-append', args, '-no-reboot']
    vm = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL)
    lines, buf, started, t0 = [], b'', False, time.monotonic()
    t_start = t0
    pending = list(typing or [])
    lo, hi, guess, found, mark = 1, 100, None, False, 0
    try:
        while time.monotonic() - t0 < seconds and len(lines) < max_lines:
            if started and pending and time.monotonic() - t_start >= pending[0][0]:
                data = pending.pop(0)[1]
                if data == GUESS:
                    for _, text in lines[mark:]:      # the answer to the last guess
                        if guess is not None and 'Too small.' in text:
                            lo = guess + 1
                        if guess is not None and 'Too big.' in text:
                            hi = guess - 1
                        found = found or 'Correct!' in text
                    guess, mark = (None if found else (lo + hi) // 2), len(lines)
                    data = b'' if found else f'{guess}\r'.encode()
                vm.stdin.write(data)
                vm.stdin.flush()
            ready, _, _ = select.select([vm.stdout], [], [], 0.05)
            if not ready:
                continue
            chunk = os.read(vm.stdout.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            while b'\n' in buf and len(lines) < max_lines:
                raw, buf = buf.split(b'\n', 1)
                text = raw.decode('ascii', 'replace').rstrip('\r')
                if not started and text.startswith('ProtoOS boot foundation'):
                    started, t_start = True, time.monotonic()
                if started and text:
                    lines.append((time.monotonic() - t_start, text[:COLS]))
    finally:
        vm.kill()
        vm.wait()
    return lines


def style_of(line):
    for prefix, cls in (('$ ', 'cmd'), ('# ', 'note'), ('FAULT', 'fault'), ('SCHED', 'sched'),
                        ('[A]', 'usera'), ('[B] pid 2: now', 'attack'), ('[B]', 'userb'),
                        ('HEARTBEAT', 'beat'), ('MMU', 'mmu'), ('PROC', 'proc'),
                        ('EXIT', 'exit'), ('IDLE', 'exit'), ('proto> ', 'usera'),
                        ('attack:', 'attack'), ('kernel answered', 'fault'),
                        ('pid ', 'proc'), ('started pid', 'proc'), ('killed pid', 'exit'),
                        ('free pages', 'mmu'), ('crash:', 'attack'), ('hello from', 'userb'),
                        ('^C', 'attack'), ('ARCHIVE', 'mmu'), ('Primes', 'userb'),
                        ('There are', 'userb'), ('uptime ', 'userb'), ('clock:', 'userb'),
                        ('I picked', 'userb'), ('Your guess', 'userb'), ('Too ', 'userb'),
                        ('DISK', 'mmu'), ('Hello from the disk', 'userb'), ('note:', 'userb'),
                        ('saved ', 'beat')):
        if line.startswith(prefix):
            return cls
    return 'out'


def frames_for(scenes, workdir):
    """Yield (visible lines, duration) for every animation frame."""
    disk = pathlib.Path(workdir) / 'disk.img'
    for args, seconds, max_lines, note, real, typing, initrd, use_disk in scenes:
        run = ('sh build-scripts/run-shell.sh' if typing else
               f'sh build-scripts/run-scenario.sh {args} {seconds}')
        screen = [f'# {note}', f'$ {run}']
        yield list(screen), COMMAND_PAUSE
        if use_disk == 'fresh':
            shutil.copyfile(DISK, disk)
        output = record(args, seconds, max_lines, typing, initrd, disk if use_disk else None)
        if not output:
            raise SystemExit(f'no UART output captured for {args}')
        for i, (t, line) in enumerate(output):
            screen.append(line)
            if i == len(output) - 1:
                dur = SCENE_HOLD
            elif real:
                dur = min(max(output[i + 1][0] - t, 0.12), 0.9)
            else:
                dur = PACE
            yield list(screen[-ROWS:]), dur


def render(frames):
    width = round(2 * PAD + COLS * CHAR_W)
    body_h = ROWS * LINE_H + 2 * PAD
    height = TITLE_H + body_h
    total = sum(d for _, d in frames)
    keys, t = [], 0.0
    for i, (_, dur) in enumerate(frames):
        keys.append(f'{100 * t / total:.4f}%{{transform:translateY({-i * body_h}px)}}')
        t += dur
    keys.append(f'100%{{transform:translateY({-(len(frames) - 1) * body_h}px)}}')
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-label="ProtoOS running under QEMU">',
        '<style>',
        f'text{{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,"Liberation Mono",monospace;'
        f'font-size:{FONT}px;white-space:pre;fill:#c9d1d9}}',
        '.cmd{fill:#ffffff;font-weight:bold}.note{fill:#8b949e;font-style:italic}'
        '.fault{fill:#ff7b72;font-weight:bold}.sched{fill:#79c0ff}.beat{fill:#7ee787}'
        '.mmu{fill:#d2a8ff}.proc{fill:#e3b341}.exit{fill:#ffa657}.title{fill:#8b949e}'
        '.usera{fill:#7ee787}.userb{fill:#79c0ff}.attack{fill:#ffa657;font-weight:bold}',
        f'.tape{{animation:roll {total:.2f}s steps(1,end) infinite}}',
        f'@keyframes roll{{{"".join(keys)}}}',
        '</style>',
        f'<rect width="{width}" height="{height}" rx="8" fill="#0d1117" stroke="#30363d"/>',
        f'<rect width="{width}" height="{TITLE_H}" rx="8" fill="#161b22"/>',
        f'<rect y="{TITLE_H - 8}" width="{width}" height="8" fill="#161b22"/>',
        '<circle cx="18" cy="14" r="5" fill="#ff5f57"/><circle cx="34" cy="14" r="5" fill="#febc2e"/>'
        '<circle cx="50" cy="14" r="5" fill="#28c840"/>',
        f'<text class="title" x="{width / 2:.0f}" y="18" text-anchor="middle">'
        'ProtoOS v1.5 on QEMU virt (RISC-V RV64) - real UART output</text>',
        f'<svg x="0" y="{TITLE_H}" width="{width}" height="{body_h}"><g class="tape">',
    ]
    for i, (screen, _) in enumerate(frames):
        out.append(f'<g transform="translate(0,{i * body_h})">')
        for row, line in enumerate(screen):
            y = PAD + (row + 1) * LINE_H - 4
            out.append(f'<text class="{style_of(line)}" x="{PAD}" y="{y}">{html.escape(line)}</text>')
        out.append('</g>')
    out.append('</g></svg></svg>')
    return '\n'.join(out) + '\n'


def main():
    if not ELF.exists():
        raise SystemExit('build/protoos.elf missing: run build-scripts/build-wsl.sh first')
    with tempfile.TemporaryDirectory() as workdir:
        frames = list(frames_for(SCENES, workdir))
    OUT.write_text(render(frames))
    print(f'{OUT.relative_to(ROOT)}: {len(frames)} frames, '
          f'{sum(d for _, d in frames):.1f} s loop, {OUT.stat().st_size} bytes')


if __name__ == '__main__':
    main()
