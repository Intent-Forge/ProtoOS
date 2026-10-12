#!/bin/bash
# Install the pinned Zig 0.13.0 (the checkpoint 0008 compiler) into ~/.protoos-venv.
# Pinned wheel hash of the official ziglang 0.13.0 manylinux x86_64 wheel.
set -euo pipefail
WHEEL_SHA=3ce0c9f16547e5d61b32e0d226926e9a2552ef4b91fccf7ab5ea1a623a77824b
VENV="$HOME/.protoos-venv"
DL=$(mktemp -d)
python3 -m venv "$VENV"
"$VENV/bin/python" -m pip download -q --no-deps --only-binary=:all: \
    --platform manylinux_2_12_x86_64 'ziglang==0.13.0' -d "$DL"
W=$(ls "$DL"/ziglang-0.13.0-*.whl)
echo "wheel: $(basename "$W")"
echo "$WHEEL_SHA  $W" | sha256sum -c -
"$VENV/bin/python" -m pip install -q --no-deps "$W"
ZIG=$("$VENV/bin/python" -c 'import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),"zig"))')
ln -sf "$ZIG" "$VENV/bin/zig"
echo "zig: $("$VENV/bin/zig" version) at $VENV/bin/zig"
