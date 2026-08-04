#pragma once

/**
    The host's parameters, and what they mean in physical units.

    Every numeric parameter the host sees is a plain 0..1 float, because
    `SetParamInfo` clamps an `FF_TYPE_STANDARD` default into 0..1 *before*
    `SetParamRange` could widen it -- so a control that stands for a radius in
    pixels cannot declare a radius as its default. The conversions all live in
    Controls.cpp, one function per control, and the shader is handed the
    physical value.

    Option parameters are the exception: they hold the element value itself.
*/

namespace nib
{
/**
    Parameter ids.

    **Append only.** Two separate things depend on this order:
    `SetParamGroup` collapses runs of consecutive same-group ids, so inserting
    an id mid-enum silently splits a group in two; and every saved composition
    stores parameters by index, so a renumber rewrites what an operator's old
    project means.
*/
enum ParamId : unsigned int
{
	// What counts as tone. The DoG runs on one scalar channel, and which
	// channel is a creative decision as much as a technical one -- a red
	// line on a blue field has enormous chroma contrast and almost no
	// luminance contrast.
	PT_SOURCE = 0,

	// The line itself: the difference-of-Gaussians and its threshold.
	PT_SCALE,
	PT_SHARPNESS,
	PT_THRESHOLD,
	PT_FALLOFF,
	PT_WEIGHT,

	// The flow. This is the group that makes this plugin what it is; see
	// AGENTS.md before changing any of it.
	PT_FLOW,
	PT_COHERENCE,
	PT_LENGTH,
	PT_PASSES,

	// Time.
	PT_STABILITY,

	// Ink and paper.
	PT_INK_R,
	PT_INK_G,
	PT_INK_B,
	PT_PAPER,
	PT_PAPER_R,
	PT_PAPER_G,
	PT_PAPER_B,
	PT_DIM,

	// Output.
	PT_MIX,

	// Preset.
	PT_PRESET,

	PT_COUNT
};

/// Which scalar the whole chain is computed on.
enum class Source
{
	Luma = 0,   ///< Rec.709 luminance. The default, and right most of the time.
	Red,
	Green,
	Blue,
	Saturation, ///< For material whose subject is separated by colour, not tone.

	Count
};

/// What sits behind the ink.
enum class Paper
{
	Colour = 0,   ///< The Paper swatch, flat. Line art on a sheet.
	Source,       ///< The clip itself: ink drawn over its own footage.
	DimmedSource, ///< The clip held back by Dim, so the ink stays legible.
	Transparent,  ///< Ink premultiplied over nothing, for the layer below.

	Count
};

//---------------------------------------------------------------------------
// The mappings. Each says its range and its shape, because "geometric" and
// "linear" are the difference between a control that is usable across its
// whole travel and one that does everything in the last tenth.
//---------------------------------------------------------------------------

/// The inner Gaussian's sigma in pixels, 0.3 to 8.0, geometrically. This is
/// the scale the drawing is seen at: small values find every pore, large ones
/// find only the shapes. The outer Gaussian is this times kSigmaRatio.
float ScaleFromParam( float value );

/// tau, 0.5 to 1.0, linear. How much of the outer Gaussian is subtracted.
/// At 1.0 the two Gaussians have equal weight and flat regions cancel to
/// exactly zero, which is the sharpest and also the most fragile setting;
/// below about 0.8 the difference stops being a band-pass and starts being a
/// blurred copy of the picture.
float SharpnessFromParam( float value );

/// epsilon, -0.1 to 0.1, linear, with 0.5 landing exactly on zero. The DoG
/// response is centred on zero in flat regions, so zero is the meaningful
/// middle of this control and not merely its midpoint.
float ThresholdFromParam( float value );

/// phi, 0.5 to 60, geometrically. The softness of the ink's edge: low is a
/// wash, high is a hard-edged pen line.
float FalloffFromParam( float value );

/// Line weight in pixels, 0 to 3, linear. A dilation of the ink after
/// thresholding, so a thin line can be made to carry without changing what
/// the detector found.
float WeightFromParam( float value );

/// The line integral convolution's strength, 0 to 1, linear. At 0 the LIC
/// pass is skipped entirely and the plugin is plain isotropic XDoG; at 1 the
/// DoG response is fully replaced by its average along the flow. This is the
/// one control that turns the plugin's whole idea off, which is exactly why
/// it is a control: it is also the A/B.
float FlowFromParam( float value );

/// The structure tensor's smoothing sigma in pixels, 0.5 to 12,
/// geometrically. How far apart two edges have to be before they are allowed
/// to disagree about which way the drawing runs.
float CoherenceFromParam( float value );

/// The LIC's half-length in pixels, 1 to 32, geometrically. How far along the
/// flow a stroke is allowed to reach to find the rest of itself.
float LengthFromParam( float value );

/// The two halves of the temporal filter, from one Stability control.
/// Asymmetric on purpose, and in the direction that matters for line art: a
/// line that appears must appear at once or the drawing lags the footage,
/// and a line that disappears must fade or every frame flickers.
float AttackFromParam( float value );
float ReleaseFromParam( float value );

/// The ratio between the two Gaussians. 1.6 is the classical value -- it is
/// the one that makes a difference of Gaussians the best approximation to a
/// Laplacian of Gaussian, which is what a DoG is standing in for.
constexpr float kSigmaRatio = 1.6f;

} // namespace nib
