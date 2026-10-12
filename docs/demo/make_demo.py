#!/usr/bin/env python3
"""Record real ProtoOS UART output under QEMU and render an animated SVG.

Usage (from the repository root, after build-scripts/build-wsl.sh):
    python3 docs/demo/make_demo.py            # -> docs/demo/protoos-demo.svg

Every line shown is captured from the guest's UART in a real QEMU run; only
the playback pace is changed (one line per PACE seconds) so it can be read.
"""
import html
import os
import pathlib
import select
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
ELF = ROOT / 'build/protoos.elf'
OUT = ROOT / 'docs/demo/protoos-demo.svg'
FW = os.environ.get('OPENSBI', '/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin')

# (bootargs, seconds to run, max captured lines, comment shown above the command)
SCENES = [
    ('protoos.test=umode', 2, 15,
     'two user processes, switched by the timer interrupt every 10 ms'),
    ('protoos.test=isolation', 2, 12,
     "process A writes into B's memory: page fault, only A is stopped"),
    ('protoos.mode=normal', 4, 9,
     'normal mode: A and B share the CPU, ~100 switches per second'),
]
PACE, COMMAND_PAUSE, SCENE_HOLD = 0.22, 0.9, 2.6
COLS, ROWS = 112, 19
FONT, LINE_H, CHAR_W, PAD, TITLE_H = 13, 18, 7.82, 14, 28


def record(args, seconds, max_lines):
    cmd = ['qemu-system-riscv64', '-machine', 'virt', '-accel', 'tcg', '-cpu', 'rv64',
           '-smp', '1', '-m', '128M', '-bios', FW, '-kernel', str(ELF), '-display', 'none',
           '-monitor', 'none', '-serial', 'stdio', '-nic', 'none', '-append', args,
           '-no-reboot']
    vm = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    lines, buf, started, t0 = [], b'', False, time.monotonic()
    try:
        while time.monotonic() - t0 < seconds and len(lines) < max_lines:
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
                started = started or text.startswith('ProtoOS boot foundation')
                if started and text:
                    lines.append(text[:COLS])
    finally:
        vm.kill()
        vm.wait()
    return lines


def style_of(line):
    for prefix, cls in (('$ ', 'cmd'), ('# ', 'note'), ('FAULT', 'fault'), ('SCHED', 'sched'),
                        ('HEARTBEAT', 'beat'), ('MMU', 'mmu'), ('PROC', 'proc'),
                        ('EXIT', 'exit'), ('IDLE', 'exit')):
        if line.startswith(prefix):
            return cls
    return 'out'


def frames_for(scenes):
    """Yield (visible lines, duration) for every animation frame."""
    for args, seconds, max_lines, note in scenes:
        secs = 3 if 'normal' in args else 2
        screen = [f'# {note}', f'$ sh build-scripts/run-scenario.sh {args} {secs}']
        yield list(screen), COMMAND_PAUSE
        output = record(args, seconds, max_lines)
        if not output:
            raise SystemExit(f'no UART output captured for {args}')
        for i, line in enumerate(output):
            screen.append(line)
            yield list(screen[-ROWS:]), SCENE_HOLD if i == len(output) - 1 else PACE


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
        '.mmu{fill:#d2a8ff}.proc{fill:#e3b341}.exit{fill:#ffa657}.title{fill:#8b949e}',
        f'.tape{{animation:roll {total:.2f}s steps(1,end) infinite}}',
        f'@keyframes roll{{{"".join(keys)}}}',
        '</style>',
        f'<rect width="{width}" height="{height}" rx="8" fill="#0d1117" stroke="#30363d"/>',
        f'<rect width="{width}" height="{TITLE_H}" rx="8" fill="#161b22"/>',
        f'<rect y="{TITLE_H - 8}" width="{width}" height="8" fill="#161b22"/>',
        '<circle cx="18" cy="14" r="5" fill="#ff5f57"/><circle cx="34" cy="14" r="5" fill="#febc2e"/>'
        '<circle cx="50" cy="14" r="5" fill="#28c840"/>',
        f'<text class="title" x="{width / 2:.0f}" y="18" text-anchor="middle">'
        'ProtoOS v1.0 on QEMU virt (RISC-V RV64) - real UART output, readable pace</text>',
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
    frames = list(frames_for(SCENES))
    OUT.write_text(render(frames))
    print(f'{OUT.relative_to(ROOT)}: {len(frames)} frames, '
          f'{sum(d for _, d in frames):.1f} s loop, {OUT.stat().st_size} bytes')


if __name__ == '__main__':
    main()
