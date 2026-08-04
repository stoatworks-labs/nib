#pragma once

/**
    The passes, as GLSL source.

        1. **copy**       picture size. Resolves MaxUV once, so no later pass
                          has to think about it.
        2. **tone**       picture size, R16F. The one scalar channel the whole
                          chain is computed on, per the Source control.
        3. **tensor**     picture size. The structure tensor (gxx, gyy, gxy).
        4. **tensorBlur** picture size, run twice (H then V). Smooths the
                          *tensor*, which is the only correct way to average
                          orientations -- see AGENTS.md.
        5. **dog**        picture size, R16F. Difference of Gaussians sampled
                          **across** the flow.
        6. **lic**        picture size, R16F. Line integral convolution
                          **along** the flow.
        7. **reinject**   picture size, R16F. Darkens the tone where ink was
                          found, so the next iteration sharpens what the last
                          one believed. Skipped after the final iteration.
        8. **threshold**  picture size. The XDoG soft step, then the weight
                          dilation.
        9. **stabilise**  picture size, ping-ponged against itself. Asymmetric
                          IIR over time.
       10. **composite**  output size. Ink, paper, mix.

    Passes 5 to 7 are a loop: the Passes control runs them one, two or three
    times, and only the last iteration skips the reinjection.

    **All of this is GPU-only.** There is no C++ mirror of the tensor
    decomposition or the convolutions -- a mirrored per-pixel filter is a
    second implementation bought to restate the shader, and it would be tested
    against itself (orrery's precedent, followed across the fleet). What the
    harness proves instead is that the flow field is a *measurably* better
    predictor of edge direction than the raw gradient (`nibtest --flow`), that
    every control is alive (`tools/sweep.py`), and that the presets are alive
    and distinct (`nibtest --presets`).
*/

#include <string>

namespace nib
{

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kToneShader;
extern const char* const kTensorShader;
extern const char* const kTensorBlurShader;
extern const char* const kReinjectShader;
extern const char* const kThresholdShader;
extern const char* const kStabiliseShader;
extern const char* const kCompositeShader;

/// The two flow-guided passes: the shared eigen decomposition plus their own
/// main. Assembled rather than written out twice, so there is exactly one
/// place where "which way does the drawing run here" is answered.
std::string DoGShaderSource();
std::string LicShaderSource();

/// The flow field written straight out as (dir.x, dir.y, anisotropy), built
/// from the same `flowAt` text the two convolution passes run. Exists only
/// for `nibtest --flow`, and assembled from the shared string rather than
/// reimplemented so that what the measurement checks is the code that ships.
std::string FlowProbeShaderSource();

/// The largest number of taps either directional convolution will ever make
/// to one side of a pixel. Both loops are bounded by a uniform that is
/// clamped to this, because an unbounded loop driven straight off a control
/// is how a slider becomes a hang on someone else's GPU.
constexpr int kMaxTaps = 32;

/// How many times the DoG/LIC pair may run. Kang's iterated FDoG converges
/// visibly by three; past that it is cost with nothing to show.
constexpr int kMaxIterations = 3;

} // namespace nib
