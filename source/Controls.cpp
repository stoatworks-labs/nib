#include "Controls.h"

#include <cmath>

namespace nib
{
namespace
{
/// value^0 = low, value^1 = high, with every octave the same width of travel.
float geometric( float value, float low, float high )
{
	return low * std::pow( high / low, value );
}

float linear( float value, float low, float high )
{
	return low + ( high - low ) * value;
}
} // namespace

float ScaleFromParam( float value )
{
	return geometric( value, 0.3f, 8.0f );
}

float SharpnessFromParam( float value )
{
	return linear( value, 0.5f, 1.0f );
}

float ThresholdFromParam( float value )
{
	return linear( value, -0.1f, 0.1f );
}

float FalloffFromParam( float value )
{
	return geometric( value, 0.5f, 60.0f );
}

float WeightFromParam( float value )
{
	return linear( value, 0.0f, 3.0f );
}

float FlowFromParam( float value )
{
	return value;
}

float CoherenceFromParam( float value )
{
	return geometric( value, 0.5f, 12.0f );
}

float LengthFromParam( float value )
{
	return geometric( value, 1.0f, 32.0f );
}

//---------------------------------------------------------------------------
// The temporal filter.
//
// Both halves are per-frame IIR coefficients: new = mix( old, measured, k ).
// Attack is held near 1 across most of the control's travel because a line
// that arrives late is a line drawn on the wrong frame -- the drawing would
// visibly trail the footage, which reads as the plugin being broken rather
// than as smoothing. Release is what the control actually buys: a line that
// dies slowly stops the per-frame flicker that any thresholded detector has
// on real footage, where thousands of pixels sit within noise of epsilon and
// cross it back and forth.
//---------------------------------------------------------------------------
float AttackFromParam( float value )
{
	return linear( value, 1.0f, 0.55f );
}

float ReleaseFromParam( float value )
{
	return geometric( 1.0f - value, 0.04f, 1.0f );
}

} // namespace nib
