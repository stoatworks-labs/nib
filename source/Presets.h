#pragma once

/**
    Factory presets: named drawing instruments an operator can reach in one
    gesture.

    The values live in the 0..1 parameter space the host sees, so one table
    drives every binding of it. Plain data only; the application machinery
    lives with the host glue in Nib.cpp.

    Element 0 of the host-facing dropdown is "Custom" and is not in this
    table: it means "the sliders are the truth".

    **Detect On and Mix are deliberately not covered.** Which channel carries
    the drawing is a property of the footage, not of the instrument, and an
    operator who has just worked out that their subject only separates in
    saturation should not lose that by trying a different nib. Mix is the
    same: it is how the effect is dialled into a composition, not part of
    what the effect is.
*/

namespace nib
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. Nib.cpp binds this order
/// to its ParamIds and static_asserts against kParamCount so the two lists
/// cannot drift apart silently.
enum Param
{
	kScale,
	kSharpness,
	kThreshold,
	kFalloff,
	kWeight,
	kFlow,
	kCoherence,
	kLength,
	kPasses,
	kStability,
	kInkR,
	kInkG,
	kInkB,
	kPaper,
	kPaperR,
	kPaperG,
	kPaperB,
	kDim,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Option values are element indices:
//   Passes  0 = one iteration, 1 = two, 2 = three
//   Paper   0 Paper Colour / 1 Source / 2 Dimmed Source / 3 Transparent
inline constexpr Preset kPresets[] = {
	// The default instrument, and the one to judge every other by: a fine
	// line, strongly flow-guided, two iterations, near-black ink on
	// near-white paper. Neither ink nor paper is a pure value -- pure black
	// on pure white is the one combination that never occurs on a real page,
	// and it reads as a threshold rather than as a drawing.
	{ "Pen and Ink",
	  { /*Scale*/ 0.35f, /*Sharp*/ 0.94f, /*Thresh*/ 0.5f, /*Falloff*/ 0.62f, /*Weight*/ 0.0f,
	    /*Flow*/ 0.85f, /*Coherence*/ 0.45f, /*Length*/ 0.4f, /*Passes*/ 1,
	    /*Stability*/ 0.35f,
	    /*Ink*/ 0.05f, 0.04f, 0.06f,
	    /*Paper*/ 0, /*PaperCol*/ 0.96f, 0.95f, 0.92f, /*Dim*/ 0.25f } },

	// Everything the same except the flow, which is off. Kept as a preset
	// and not merely as a slider position because it is the honest A/B:
	// this is what the plugin would be if it were plain isotropic XDoG, and
	// having it one click away is what makes the claim checkable rather
	// than merely asserted.
	{ "Unguided (XDoG)",
	  { /*Scale*/ 0.35f, /*Sharp*/ 0.94f, /*Thresh*/ 0.5f, /*Falloff*/ 0.62f, /*Weight*/ 0.0f,
	    /*Flow*/ 0.0f, /*Coherence*/ 0.45f, /*Length*/ 0.4f, /*Passes*/ 0,
	    /*Stability*/ 0.35f,
	    /*Ink*/ 0.05f, 0.04f, 0.06f,
	    /*Paper*/ 0, /*PaperCol*/ 0.96f, 0.95f, 0.92f, /*Dim*/ 0.25f } },

	// A soft graphite edge: low falloff so the line is a wash rather than a
	// stroke, wide scale so it finds shapes instead of pores, and a warm
	// grey on cartridge paper.
	// Falloff is the whole preset, and it has a floor: below about 0.4 the
	// tanh ramp is wider than the DoG response itself, every pixel lands
	// mid-ramp, and the drawing washes out to a barely-there stain rather
	// than reading as soft. Soft graphite is a wide *scale* with a still-
	// definite edge, not a soft threshold.
	{ "Charcoal",
	  { /*Scale*/ 0.55f, /*Sharp*/ 0.9f, /*Thresh*/ 0.52f, /*Falloff*/ 0.45f, /*Weight*/ 0.4f,
	    /*Flow*/ 0.7f, /*Coherence*/ 0.6f, /*Length*/ 0.55f, /*Passes*/ 1,
	    /*Stability*/ 0.45f,
	    /*Ink*/ 0.13f, 0.11f, 0.10f,
	    /*Paper*/ 0, /*PaperCol*/ 0.88f, 0.85f, 0.78f, /*Dim*/ 0.25f } },

	// Three iterations and a long reach: the setting that closes gaps in a
	// faint or broken edge, at roughly three times the cost of one pass.
	// This is what the iteration is *for*, so it exists as a preset to be
	// compared against Pen and Ink rather than argued about.
	{ "Technical Pen",
	  { /*Scale*/ 0.28f, /*Sharp*/ 0.97f, /*Thresh*/ 0.5f, /*Falloff*/ 0.8f, /*Weight*/ 0.12f,
	    /*Flow*/ 1.0f, /*Coherence*/ 0.5f, /*Length*/ 0.62f, /*Passes*/ 2,
	    /*Stability*/ 0.4f,
	    /*Ink*/ 0.02f, 0.02f, 0.03f,
	    /*Paper*/ 0, /*PaperCol*/ 1.0f, 0.99f, 0.97f, /*Dim*/ 0.25f } },

	// White ink on nothing: the drawing as an alpha layer for the
	// composition to place, rather than as a finished sheet.
	{ "Alpha Ink",
	  { /*Scale*/ 0.35f, /*Sharp*/ 0.94f, /*Thresh*/ 0.5f, /*Falloff*/ 0.7f, /*Weight*/ 0.1f,
	    /*Flow*/ 0.85f, /*Coherence*/ 0.45f, /*Length*/ 0.4f, /*Passes*/ 1,
	    /*Stability*/ 0.35f,
	    /*Ink*/ 1.0f, 1.0f, 1.0f,
	    /*Paper*/ 3, /*PaperCol*/ 0.96f, 0.95f, 0.92f, /*Dim*/ 0.25f } },

	// The drawing laid back over the footage it was taken from, held down
	// far enough that the ink still reads. The one preset that keeps the
	// clip's own colour in the picture.
	{ "Overlay",
	  { /*Scale*/ 0.4f, /*Sharp*/ 0.92f, /*Thresh*/ 0.5f, /*Falloff*/ 0.68f, /*Weight*/ 0.25f,
	    /*Flow*/ 0.9f, /*Coherence*/ 0.5f, /*Length*/ 0.45f, /*Passes*/ 1,
	    /*Stability*/ 0.4f,
	    /*Ink*/ 1.0f, 0.97f, 0.9f,
	    /*Paper*/ 2, /*PaperCol*/ 0.96f, 0.95f, 0.92f, /*Dim*/ 0.35f } },

	// Blueprint: white line on a drawing-office blue. The flow is turned up
	// and the scale down, because a blueprint's lines are all one weight.
	{ "Cyanotype",
	  { /*Scale*/ 0.3f, /*Sharp*/ 0.96f, /*Thresh*/ 0.5f, /*Falloff*/ 0.85f, /*Weight*/ 0.2f,
	    /*Flow*/ 1.0f, /*Coherence*/ 0.55f, /*Length*/ 0.5f, /*Passes*/ 1,
	    /*Stability*/ 0.4f,
	    /*Ink*/ 0.93f, 0.96f, 1.0f,
	    /*Paper*/ 0, /*PaperCol*/ 0.06f, 0.20f, 0.42f, /*Dim*/ 0.25f } },

	// Wide scale, heavy weight, hard falloff: the shapes only, drawn thick.
	// Where Technical Pen finds everything, this finds the three lines a
	// poster would have kept.
	//
	// Scale and Weight fight each other here and both have to be held back.
	// Scale widens the band-pass so neighbouring strokes are found as one,
	// and Weight then dilates that already-merged answer -- so the two
	// compound, and the values that each look reasonable alone turn a hatched
	// field into a solid block. Judge this one on the diagonal bars, which
	// are the closest-spaced thing on the card.
	{ "Woodcut",
	  { /*Scale*/ 0.6f, /*Sharp*/ 0.99f, /*Thresh*/ 0.46f, /*Falloff*/ 0.95f, /*Weight*/ 0.32f,
	    /*Flow*/ 0.95f, /*Coherence*/ 0.75f, /*Length*/ 0.7f, /*Passes*/ 1,
	    /*Stability*/ 0.5f,
	    /*Ink*/ 0.04f, 0.03f, 0.03f,
	    /*Paper*/ 0, /*PaperCol*/ 0.93f, 0.90f, 0.82f, /*Dim*/ 0.25f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace nib
