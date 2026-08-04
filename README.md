# nib

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The filtering is
> GPU-only and has no C++ mirror, so the central claim is not asserted but
> measured: `nibtest --flow` renders concentric rings whose tangent direction
> is known in closed form, adds noise, and reports how far the plugin's own
> flow field is from the truth in degrees — probing the same shader code that
> ships. A control sweep fails if any parameter turns out to do nothing (see
> [Building and testing](#building-and-testing)).

Line drawing for Resolume Arena/Avenue, as an FFGL effect. It turns a clip
into ink — continuous, closed strokes on paper, not a glowing outline.

Most edge effects ask one question per pixel: *how fast is tone changing
here?* That gives a magnitude, which has no memory of direction, so it cannot
tell a stroke from a speck and its answer at one pixel has nothing to do with
its answer at the neighbour along the same line.

nib asks which way the drawing runs first, and then steers two filters by
that answer in opposite directions — a difference of Gaussians **across** the
flow, which is the only direction in which a band-pass is measuring an edge
at all, and a line integral convolution **along** it, which is what joins a
per-pixel response into a stroke. A pixel whose neighbours along the same
line agree survives; one that fired on its own does not.

Turn `Flow` down to zero and the second filter is skipped: what is left is
plain isotropic XDoG, which is what the effect would be without the idea in
it. That comparison ships as a preset, because a claim you can check in one
click is worth more than one you have to believe.

## Controls

- **Detect On** — which channel carries the drawing. A red line on a blue
  field has enormous chroma contrast and almost no luminance contrast, so
  Saturation is not an exotic setting.
- **Line** — Scale (how fine a drawing), Sharpness, Threshold, Falloff (wash
  to pen line), Weight (a heavier nib, dilated rather than blurred).
- **Flow** — Flow (off is plain XDoG), Coherence (how far apart two edges may
  be before they are allowed to disagree about direction), Length (how far
  along a stroke to reach for the rest of it), Passes (1–3; each iteration
  darkens what the last one believed and runs again, which is what closes
  gaps in a faint edge).
- **Time** — Stability. Asymmetric on purpose: ink appears at once and fades
  slowly, because a line that arrives late is drawn on the wrong frame.
- **Ink** — ink and paper colour, or the clip itself as paper, dimmed or not,
  or nothing at all for an alpha layer.

Eight factory instruments: Pen and Ink, Unguided (XDoG), Charcoal, Technical
Pen, Alpha Ink, Overlay, Cyanotype, Woodcut.

## Status

**v0.1.0, and honestly early.** It has never been loaded into Resolume —
everything here is verified through the offline harness, which drives the
real plugin class headlessly. There is no OpenFX port and no frame-piping
mode yet. It has only been built and measured on macOS (Apple Silicon):
0.66 ms/frame at 720p, 1.51 at 1080p, 6.60 at 4K.

## Building and testing

C++17 + GLSL 4.10, CMake, FFGL 2.1 (SDK vendored as a submodule). macOS
builds are universal (arm64 + x86_64); Windows needs GLEW via vcpkg.

    git clone --recursive https://github.com/stoatworks-labs/nib
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    cmake --install build          # into Resolume's Extra Effects

The offline harness renders the real plugin class headlessly:

    ./build/nibtest --out /tmp/frame.png       # the test card, drawn
    ./build/nibtest --flow                     # the measurement, in degrees
    ./build/nibtest --presets /tmp/p.png       # every preset, checked distinct
    ./build/nibtest --bench                    # 720p through 4K
    python3 tools/sweep.py                     # no control is silently dead
    tools/verify.sh                            # all of it

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT.

The method is from the published computer-graphics literature: Winnemöller's
extended difference-of-Gaussians (XDoG), and Kang, Lee and Chui's
flow-based DoG with an edge tangent flow. Both are described in papers, not
copied from anyone's source; the implementation here is this repo's own.
