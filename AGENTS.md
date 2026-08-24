# nib — orientation for another LLM (or a newcomer)

**What it is:** flow-guided line drawing as an FFGL 2.1 effect plugin for
Resolume Arena/Avenue. It turns a clip into ink strokes — continuous,
closed, variable-weight lines rather than a lit edge mask. C++17 + GLSL 4.10,
CMake, universal macOS `.bundle` and a Windows `.dll`. Public, MIT,
`github.com/stoatworks-labs/nib`.

`CLAUDE.md` is the command reference — build, install, verify. This file is
the *why*: read it before touching the tensor pass, the two convolutions, or
the iteration.

Family notes: the copy pass, `PassBuffer`, `Diag`, the preset machinery and
the harness shape are lifted from **outrun** and **tinsel**. Where those
repos document a trap, it applies here too.

---

## The one idea

**The smoothing follows the drawing.**

Everything else in the fleet that finds edges — tinsel, outrun's Engine A,
orrery — asks one question per pixel: *how fast is tone changing here?* That
is a magnitude. It has no memory of direction, so it cannot tell a stroke
from a speck, and the answer at one pixel is unrelated to the answer at its
neighbour along the same line.

nib asks a different question first: *which way does the drawing run here?*
Once there is an answer to that, every filter downstream can be steered by
it — and the two that matter are steered in **opposite** directions:

- the **difference of Gaussians runs across the flow**, because
  perpendicular to an edge is the only direction in which a band-pass is
  measuring the edge at all. A tap taken along the edge is a tap spent
  blurring the line the filter is trying to find.
- the **line integral convolution runs along the flow**, because that is
  what turns a per-pixel response into a stroke. A pixel whose neighbours
  *along the same line* agree survives; one that fired on its own does not.

That is the whole plugin, and it is why `Flow` is a control rather than a
constant: at zero the LIC pass does not run and what remains is plain
isotropic XDoG. The A/B is one click away on purpose (preset "Unguided
(XDoG)"), because a claim an operator can check is worth more than one they
have to believe.

### Why a tensor and not a gradient direction

The thing that has to be averaged over a neighbourhood is an **orientation**,
and orientations live on a half circle: a line at 179° and one at 1° are very
nearly parallel, but their gradient *vectors* nearly cancel. Averaging
gradients therefore destroys exactly the agreement it was meant to find.

The structure tensor `J = ggᵀ` is quadratic in the gradient, so it takes the
same value for `g` and `−g`. Averaging it averages orientation without the
wrap. That is the only reason the tensor pass exists rather than a blur of
`gx` and `gy` — and it is measured, not asserted: `nibtest --flow` renders
concentric rings whose tangent is known in closed form, adds noise, and
reports the mean angular error at four Coherence settings. On an M4 Max it
reads **13.8° at the least-smoothed setting, 1.1° at the best** — a 92%
reduction. The check fails the build below 33%.

### What is deliberately NOT mirrored

There is no C++ mirror of the tensor decomposition or either convolution. A
mirrored per-pixel filter is a second implementation bought to restate the
shader, and it would be tested against itself (orrery's precedent, followed
across the fleet). `--flow` probes the **shipping** `flowAt` — the probe
shader is assembled from the same `kFlowLibrary` string the two convolution
passes are — so what the measurement checks is the code that runs.

---

## The traps

Ordered by how much time they will cost you.

**An eigenvector has no sign.** `flowAt` returns a direction that may come
back pointing 180° the other way at the very next pixel, for no reason but
the arithmetic. The LIC walk therefore aligns every step against the step
before it (`if( dot( t, dir ) < 0.0 ) t = -t;`). Without that the walk
oscillates on the spot instead of travelling, and the symptom is not an
obvious error — it is a LIC that looks like a slightly weak blur, because
every pixel averaged a handful of its immediate neighbours and went nowhere.

**Every intermediate buffer is floating point, and has to be.** The DoG
response is a *difference of two nearly equal blurs*: with Sharpness near 1
the interesting part of the signal is a few thousandths of the tone. An
8-bit buffer quantises that to nothing. The symptom is not a subtle loss of
quality — Sharpness stops working entirely above about 0.9, which is the half
of its range worth having.

**Scale and Weight compound.** Scale widens the band-pass so neighbouring
strokes are found as one; Weight then dilates that already-merged answer. The
values that each look reasonable alone turn a hatched field into a solid
block. Judge any change to either on the test card's diagonal bars, which are
the closest-spaced thing on it.

**Falloff has a floor.** Below about 0.4 the tanh ramp is wider than the DoG
response itself, so every pixel lands mid-ramp and the drawing washes out to
a stain. "Soft" is a wide Scale with a still-definite edge, not a soft
threshold.

**Where the tensor has no opinion, the LIC must not turn.** In a flat region
the anisotropy is ~0 and `flowAt`'s direction is whatever the noise says. The
walk blends toward the measured tangent in proportion to anisotropy and keeps
its previous heading otherwise; remove that and a clean sky fills with
swirls.

**`step` is a GLSL built-in**, and so are `flat`, `active`, `filter`,
`input`, `output`, `sample`, `common`, `layout`. These shaders are assembled
from strings, so the "syntax error, line N" a shadowed built-in produces
points into a file that does not exist.

**`ScopedFBOBinding` does not restore the viewport** (SDK b1afaf9). The host
viewport is captured at the top of `ProcessOpenGL` and restored before the
composite; without it the composite inherits the last pass's viewport, which
in most viewers reads as "blown out with a small picture in the corner"
rather than as a viewport bug.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** So every buffer is allocated in `ensureBuffers()` before anything
binds a texture. `FFGLFBO::Initialise` sizes its colour texture under a
scoped binding, and allocating mid-chain silently unbinds the input texture.
The symptom is the dangerous part: correct on every frame except the one that
allocates.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* `SetParamRange` could widen it,
so every host parameter is 0..1 and every conversion lives in `Controls.cpp`.
Option parameters are the exception: they hold the element value.

**`SetParamGroup` collapses runs of consecutive same-group ids.** The id
order in `Controls.h` is therefore load-bearing: reorder it, or insert a
parameter mid-enum, and a group silently splits in two (and every saved
composition renumbers). Append only.

**The plugin registers itself from a file-scope constructor.** `nib_core` is
an OBJECT library and the `CFFGLPluginInfo` lives in `PluginEntry.cpp`,
listed only in the MODULE target. A STATIC core drops the registration TU and
ships a bundle that loads, exports `plugMain`, and contains no plugins.
Verify with `nm -gU … | grep _plugMain` *and* a host load.

**A preset that changes Scale, Detect On or Passes must drop the history.**
`applyPreset` writes `params[]` directly rather than going through
`SetFloatParameter`, so it repeats that invalidation itself. Without it a
preset is the one way to change what an edge *is* while leaving the previous
scale's drawing decaying underneath it.

---

## Shape of the code

    source/Nib.{h,cpp}       the plugin class: parameters, buffers, the chain.
    source/Shaders.{h,cpp}   all GLSL. The flow library is one string, pasted
                             into both convolution passes and into the probe.
    source/Controls.{h,cpp}  0..1 host parameters to physical units; the enums.
    source/Presets.h         8 factory instruments, plain data, host-agnostic.
    source/PassBuffer.*      FFGLFBO with tinsel's leak fix, three samplings.
    source/PluginEntry.cpp   the registration. See traps.
    source/Diag.*            a log file, for the shader that will not compile.
    tools/nibtest/           the offline harness (drives the real classes).
    tools/sweep.py           no control is silently dead.
    tools/verify.sh          all of it.

Pass chain: copy → tone → tensor → tensorBlur×2 → **{ dog → lic → reinject }
× Passes** → threshold → stabilise → composite. The reinjection is skipped
after the final iteration.

## What has not been done

- **Never loaded into Resolume.** Everything here is the offline harness.
- **No OFX port**, and no `--pipe`/`--script`, so the fleet's project-video
  pipeline cannot film it yet.
- **Not built on Windows or Linux.** The CMake is the fleet's and should
  work; nothing has proven it.
- Measured on an M4 Max only: 0.66 ms/frame at 720p, 1.51 at 1080p, 6.60 at
  4K. The 4K figure is 40% of a 60 fps frame and is the one to watch if the
  chain grows.

## Notes

`docs/NOTES.md` carries this repo's working notes — current status, decisions
already made, and the traps that have actually bitten. Read it before changing
anything non-obvious. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
