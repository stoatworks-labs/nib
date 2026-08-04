#!/usr/bin/env python3
"""Move every parameter and fail if any of them made no difference to the frame.

**This is the only thing in the repo that catches a dead control**, and it is
not a theoretical risk. A GLSL uniform whose name does not match the C++ is
silently ignored -- `glGetUniformLocation` returns -1 and `glUniform` on -1 is
a documented no-op -- so a slider can be stone dead while everything compiles,
links, loads and renders. The preset sheet will not catch it either: it only
ever exercises the settings the presets happen to use.

## Why there is a context table

Several parameters are *supposed* to do nothing in the default configuration,
and a naive sweep would report a stack of false failures:

- `Length` is the reach of the LIC, and the LIC pass is skipped entirely at
  `Flow` zero. Flow is its context.
- `Dim` dims the Dimmed Source paper and nothing else.
- The Paper swatch is read only when Paper is the flat-colour mode.
- `Stability` filters over *time*, so on a still card it provably does
  nothing. Its context adds per-frame noise and more frames, which is the
  situation the control exists for.
- `Coherence` changes the direction estimate, which on a clean synthetic card
  barely changes the picture -- the whole point of the tensor smoothing is
  that it earns its keep against noise. Its context is therefore noisy, the
  same as Stability's, and `nibtest --flow` is where it is actually measured
  rather than merely shown to be alive.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose]
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

# Applied to every render. Nothing needed: the defaults already draw on the
# card, which was a design goal of the defaults.
BASE = []

# What else has to be true for a parameter to be able to do anything. Entries
# are extra `--set` assignments, except entries starting with "--", which are
# passed through as raw harness arguments.
CONTEXT = {
    # The LIC pass does not run at all at Flow zero.
    "Length": ["Flow=0.8"],

    # Dim dims one paper mode.
    "Dim": ["Paper=2"],

    # The swatch is read by the flat-colour paper only.
    "Paper Colour": ["Paper=0"],
    "Paper_Green":  ["Paper=0"],
    "Paper_Blue":   ["Paper=0"],

    # Both of these exist to fight noise, and neither can demonstrate that on
    # a still, clean card.
    "Stability":  ["--noise", "0.12", "--frames", "40"],
    "Coherence":  ["--noise", "0.12", "--frames", "20"],
}

# The values every non-option parameter is swept across. The awkward numbers
# are load-bearing: a parameter the picture is periodic in can land 0, 0.5 and
# 1 on pixel-identical frames and report a working slider as dead. 0.137 and
# 0.611 are not rational multiples of anything swept here.
SWEEP_VALUES = [0.0, 0.137, 0.611, 1.0]

# Option parameters are swept across their elements instead. --list reports a
# parameter's kind but not its element count, so these track the enums in
# Controls.h and the table in Presets.h by hand.
OPTION_RANGE = {
    "Detect On": 5,
    "Passes": 3,
    "Paper": 4,
    "Preset": 1 + 8,
}

# Parameter kinds with no scalar worth sweeping.
SKIP_KINDS = {"buffer", "event"}


def read_png(path):
    """Decode a PNG to raw bytes. Enough of the format for our own writer's
    output -- 8-bit RGBA, one IDAT, filter 0 on every row."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    pos = 8
    idat = b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IDAT":
            idat += body
        pos += 12 + length

    return zlib.decompress(idat)


def render(harness, out, settings, raw, verbose):
    args = [str(harness), "--out", str(out), "--size", "480x270"]
    args += raw
    for setting in settings:
        args += ["--set", setting]

    if verbose:
        print("   ", " ".join(args))

    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"nibtest failed: {result.stderr.strip()}")

    return read_png(out)


def parameters(harness):
    """Name and kind of every parameter, in declaration order."""
    result = subprocess.run([str(harness), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"nibtest --list failed: {result.stderr.strip()}")

    found = []
    for line in result.stdout.splitlines()[1:]:
        # id, name (may contain spaces), kind, default
        parts = line.split()
        if len(parts) < 4:
            continue
        kind = parts[-2]
        name = " ".join(parts[1:-2])
        found.append((name, kind))

    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    harness = REPO / args.build / "nibtest"
    if not harness.exists():
        print(f"no nibtest at {harness} -- build first", file=sys.stderr)
        return 2

    dead = []
    checked = 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)

        for name, kind in parameters(harness):
            if kind in SKIP_KINDS:
                continue

            # Context entries that start "--" are raw harness arguments (a
            # value follows each, also in the list); the rest are --set
            # assignments.
            context = CONTEXT.get(name, [])
            extra = []
            sets = []
            walk = iter(context)
            for token in walk:
                if token.startswith("--"):
                    extra += [token, next(walk)]
                else:
                    sets.append(token)

            base = BASE + sets

            if name in OPTION_RANGE:
                values = [float(i) for i in range(OPTION_RANGE[name])]
            else:
                values = SWEEP_VALUES

            frames = []
            for value in values:
                out = tmp / "sweep.png"
                frames.append(
                    render(harness, out, base + [f"{name}={value}"], extra, args.verbose))

            checked += 1
            if all(f == frames[0] for f in frames[1:]):
                dead.append(f"{name} ({kind})")
                print(f"  DEAD {name}")
            elif args.verbose:
                print(f"  ok   {name}")

    print()
    if dead:
        print(f"sweep: {checked} parameters, {len(dead)} made no difference:")
        for entry in dead:
            print(f"  - {entry}")
        return 1

    print(f"sweep: {checked} parameters, all live")
    return 0


if __name__ == "__main__":
    sys.exit(main())
