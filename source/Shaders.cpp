#include "Shaders.h"

namespace nib
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. The usual FFGL vertex shader
	//folds MaxUV in here; that happens once in the copy pass instead, and
	//every pass after it works on a texture we allocated, where the picture
	//really does fill the texture.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: copy.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;      //the part of the input texture that is really picture
uniform vec2 HalfTexel;  //half an input texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge. GL_LINEAR at the picture boundary takes
	//half its weight from the texture's undrawn padding, and that shows up as
	//a false edge running down the side of the frame -- which this plugin
	//would then dutifully ink in.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );

	fragColor = texture( InputTexture, picture * MaxUV );
}
)";

//---------------------------------------------------------------------------
// Pass 2: tone. One scalar, which everything downstream is computed on.
//---------------------------------------------------------------------------
const char* const kToneShader = R"(#version 410 core

uniform sampler2D CopyTexture;
uniform float SourceMode;  //0 luma, 1 R, 2 G, 3 B, 4 saturation

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 c = texture( CopyTexture, uv );

	//Un-premultiply before looking at colour. The copy is premultiplied, so a
	//half-transparent red pixel arrives as a dark red one, and a channel or
	//saturation reading taken off it would be measuring the alpha instead.
	vec3 rgb = c.a > 0.0031 ? c.rgb / c.a : c.rgb;

	int mode = int( SourceMode + 0.5 );

	float value;
	if( mode == 1 )
		value = rgb.r;
	else if( mode == 2 )
		value = rgb.g;
	else if( mode == 3 )
		value = rgb.b;
	else if( mode == 4 )
	{
		float high = max( rgb.r, max( rgb.g, rgb.b ) );
		float low  = min( rgb.r, min( rgb.g, rgb.b ) );
		value = high > 0.0 ? ( high - low ) / high : 0.0;
	}
	else
		value = dot( rgb, vec3( 0.2126, 0.7152, 0.0722 ) );

	fragColor = vec4( value, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 3: the structure tensor.
//
// The gradient is Sobel, and the tensor is its outer product with itself:
//
//     J = [ gx*gx  gx*gy ]
//         [ gx*gy  gy*gy ]
//
// Why a tensor and not simply the gradient: the thing that has to be averaged
// over a neighbourhood is an *orientation*, and orientations live on a half
// circle -- a line running at 179 degrees and one at 1 degree are very nearly
// parallel, but their vectors nearly cancel. The tensor is quadratic in the
// gradient, so it takes the same value for g and -g, and averaging it
// averages orientation without the wrap. That is the whole reason this pass
// exists rather than blurring gx and gy directly.
//---------------------------------------------------------------------------
const char* const kTensorShader = R"(#version 410 core

uniform sampler2D ToneTexture;
uniform vec2 TexelSize;

in vec2 uv;
out vec4 fragColor;

float tone( vec2 p )
{
	return texture( ToneTexture, p ).r;
}

void main()
{
	vec2 t = TexelSize;

	float tl = tone( uv + vec2( -t.x,  t.y ) );
	float tc = tone( uv + vec2(  0.0,  t.y ) );
	float tr = tone( uv + vec2(  t.x,  t.y ) );
	float ml = tone( uv + vec2( -t.x,  0.0 ) );
	float mr = tone( uv + vec2(  t.x,  0.0 ) );
	float bl = tone( uv + vec2( -t.x, -t.y ) );
	float bc = tone( uv + vec2(  0.0, -t.y ) );
	float br = tone( uv + vec2(  t.x, -t.y ) );

	float gx = ( tr + 2.0 * mr + br ) - ( tl + 2.0 * ml + bl );
	float gy = ( tl + 2.0 * tc + tr ) - ( bl + 2.0 * bc + br );

	//Sobel's kernel sums to 8 over the ring; dividing here keeps the tensor's
	//eigenvalues in the same units as the tone, which is what lets the
	//anisotropy measure below be compared against a plain constant.
	gx *= 0.125;
	gy *= 0.125;

	fragColor = vec4( gx * gx, gy * gy, gx * gy, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 4: separable Gaussian on the tensor. Run twice, H then V.
//---------------------------------------------------------------------------
const char* const kTensorBlurShader = R"(#version 410 core

uniform sampler2D TensorTexture;
uniform vec2 Direction;   //one texel along the axis being blurred
uniform float Sigma;
uniform int Taps;         //half-width in taps, already clamped host-side

in vec2 uv;
out vec4 fragColor;

void main()
{
	float twoSigmaSq = 2.0 * Sigma * Sigma;

	vec3 sum = texture( TensorTexture, uv ).xyz;
	float norm = 1.0;

	for( int i = 1; i <= 32; ++i )
	{
		if( i > Taps )
			break;

		float d = float( i );
		float w = exp( -d * d / twoSigmaSq );

		sum += w * texture( TensorTexture, uv + Direction * d ).xyz;
		sum += w * texture( TensorTexture, uv - Direction * d ).xyz;
		norm += 2.0 * w;
	}

	fragColor = vec4( sum / norm, 1.0 );
}
)";

//---------------------------------------------------------------------------
// The flow, recovered from the smoothed tensor. Shared by the two
// convolution passes, so it is one string pasted into both -- a second copy
// of an eigen decomposition is a second thing to get wrong.
//
// For a symmetric 2x2 [[E,F],[F,G]] the larger eigenvalue is
//
//     l1 = 0.5 * ( E + G + sqrt( (E-G)^2 + 4F^2 ) )
//
// and the *minor* eigenvector -- the direction in which the tone changes
// least, i.e. along the edge -- is perpendicular to the major one. Written
// directly as (l1 - E, -F), which needs no branch and degrades gracefully:
// in a flat region the tensor is zero, the vector is zero, and the caller
// gets the fallback rather than a normalize() of nothing.
//---------------------------------------------------------------------------
static const char* const kFlowLibrary = R"(
uniform sampler2D FlowTexture;

//The local edge direction, unit length, plus how much to believe it.
//
//`anisotropy` is (l1 - l2) / (l1 + l2): 1 where the tensor says one direction
//dominates completely, 0 where it has no opinion at all. It is what stops the
//flow-guided passes from confidently smearing along a direction picked out of
//sensor noise in a flat sky.
vec3 flowAt( vec2 p )
{
	vec3 j = texture( FlowTexture, p ).xyz;
	float E = j.x, G = j.y, F = j.z;

	float d = E - G;
	float disc = sqrt( max( d * d + 4.0 * F * F, 0.0 ) );

	float l1 = 0.5 * ( E + G + disc );
	float l2 = 0.5 * ( E + G - disc );

	vec2 t = vec2( l1 - E, -F );
	float len = length( t );

	//The fallback is deliberate and not arbitrary: with no measurable
	//structure there is no edge to follow, so pointing everything the same
	//way makes the LIC a plain 1D blur rather than a swirl of noise.
	vec2 dir = len > 1e-9 ? t / len : vec2( 0.0, 1.0 );

	float sum = l1 + l2;
	float anisotropy = sum > 1e-9 ? ( l1 - l2 ) / sum : 0.0;

	return vec3( dir, anisotropy );
}
)";

//---------------------------------------------------------------------------
// Pass 5: the difference of Gaussians, sampled ACROSS the flow.
//
// A plain XDoG convolves two isotropic Gaussians over the whole
// neighbourhood. Sampling only along the gradient direction instead is what
// makes this flow-guided: perpendicular to an edge is the only direction in
// which a band-pass filter is measuring the edge at all, and every tap taken
// along the edge is a tap spent blurring the line the filter is trying to
// find.
//---------------------------------------------------------------------------
static const char* const kDoGMain = R"(
uniform sampler2D ToneTexture;
uniform vec2 TexelSize;
uniform float Sigma;
uniform float Tau;
uniform int Taps;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec3 f = flowAt( uv );

	//Across the flow: the normal to the edge direction.
	vec2 n = vec2( f.y, -f.x );

	float sigma2 = Sigma * 1.6;
	float twoS1 = 2.0 * Sigma * Sigma;
	float twoS2 = 2.0 * sigma2 * sigma2;

	float c = texture( ToneTexture, uv ).r;
	float sum1 = c, sum2 = c;
	float norm1 = 1.0, norm2 = 1.0;

	for( int i = 1; i <= 32; ++i )
	{
		if( i > Taps )
			break;

		float d = float( i );
		//Not named `step`: that is a GLSL built-in, and shadowing one gives a
		//"syntax error" pointing into a file that does not exist, because
		//these shaders are assembled from strings.
		vec2 walk = n * TexelSize * d;

		float a = texture( ToneTexture, uv + walk ).r;
		float b = texture( ToneTexture, uv - walk ).r;
		float pair = a + b;

		float w1 = exp( -d * d / twoS1 );
		float w2 = exp( -d * d / twoS2 );

		sum1 += w1 * pair;
		sum2 += w2 * pair;
		norm1 += 2.0 * w1;
		norm2 += 2.0 * w2;
	}

	fragColor = vec4( sum1 / norm1 - Tau * ( sum2 / norm2 ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 6: line integral convolution ALONG the flow.
//
// This is the pass that turns a per-pixel response into a drawn line. It
// walks the flow field a step at a time, re-reading the direction as it goes,
// so it follows a curve round rather than shooting off the tangent it started
// on -- and averages the DoG response over that walk. A pixel whose
// neighbours *along the same stroke* agree survives; one that fired on its
// own does not.
//
// The sign flip is the trap. An eigenvector is only defined up to a sign, so
// consecutive samples of the flow field can come back pointing 180 degrees
// apart for no reason but the arithmetic -- and a walk that believes them
// oscillates on the spot instead of travelling. Every step is therefore
// aligned against the step before it.
//---------------------------------------------------------------------------
static const char* const kLicMain = R"(
uniform sampler2D DoGTexture;
uniform vec2 TexelSize;
uniform float Sigma;
uniform float Strength;
uniform int Taps;

in vec2 uv;
out vec4 fragColor;

void main()
{
	float twoSigmaSq = 2.0 * Sigma * Sigma;

	float raw = texture( DoGTexture, uv ).r;

	float sum = raw;
	float norm = 1.0;

	//Both ways along the stroke, as two walks from the same seed.
	for( int side = 0; side < 2; ++side )
	{
		vec2 p = uv;
		vec2 dir = flowAt( uv ).xy * ( side == 0 ? 1.0 : -1.0 );

		for( int i = 1; i <= 32; ++i )
		{
			if( i > Taps )
				break;

			vec3 f = flowAt( p );
			vec2 t = f.xy;

			//Up to a sign. Align it with where we were already going, or the
			//walk stalls.
			if( dot( t, dir ) < 0.0 )
				t = -t;

			//Where the tensor has no opinion, keep the previous heading
			//rather than turning to face noise.
			dir = mix( dir, t, clamp( f.z * 4.0, 0.0, 1.0 ) );
			dir = normalize( dir );

			p += dir * TexelSize;

			float d = float( i );
			float w = exp( -d * d / twoSigmaSq );

			sum += w * texture( DoGTexture, p ).r;
			norm += w;
		}
	}

	//A morph rather than a switch, so Flow is continuous across its travel and
	//an operator can sit at a third of the way and get a line that is guided
	//but still admits the detector had its own opinion.
	fragColor = vec4( mix( raw, sum / norm, Strength ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 7: reinjection, between iterations.
//
// Kang's iterated FDoG: darken the tone wherever this iteration is confident
// there is ink, then run the whole thing again. The next DoG therefore sees
// a picture in which the strokes it already found are stronger than they
// were, and commits to them -- which is what closes the gaps a single pass
// leaves in a faint or broken edge.
//---------------------------------------------------------------------------
const char* const kReinjectShader = R"(#version 410 core

uniform sampler2D ToneTexture;
uniform sampler2D LicTexture;
uniform float Threshold;
uniform float Falloff;

in vec2 uv;
out vec4 fragColor;

void main()
{
	float tone = texture( ToneTexture, uv ).r;
	float u = texture( LicTexture, uv ).r;

	//The same soft step the threshold pass applies, used here as a weight
	//rather than as a picture.
	float ink = u >= Threshold ? 1.0 : 1.0 + tanh( Falloff * ( u - Threshold ) );

	fragColor = vec4( min( tone, ink ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 8: the XDoG threshold, and the weight.
//
// Winnemoeller's soft step. A hard comparison against epsilon gives a binary
// mask with staircased diagonals; the tanh puts a controllable ramp on it, so
// Falloff is the difference between a wash and a pen line without any
// anti-aliasing machinery of its own.
//---------------------------------------------------------------------------
const char* const kThresholdShader = R"(#version 410 core

uniform sampler2D LicTexture;
uniform vec2 TexelSize;
uniform float Threshold;
uniform float Falloff;
uniform float Weight;   //dilation radius in pixels
uniform int WeightTaps;

in vec2 uv;
out vec4 fragColor;

float inkAt( vec2 p )
{
	float u = texture( LicTexture, p ).r;
	float t = u >= Threshold ? 1.0 : 1.0 + tanh( Falloff * ( u - Threshold ) );
	return clamp( 1.0 - t, 0.0, 1.0 );
}

void main()
{
	float ink = inkAt( uv );

	//Dilation, not a blur: a heavier nib lays down more ink over the same
	//stroke, it does not make the stroke fainter and wider. Taking the max
	//over a disc is what keeps a weighted line as solid as an unweighted one.
	if( WeightTaps > 0 )
	{
		for( int y = -3; y <= 3; ++y )
		{
			for( int x = -3; x <= 3; ++x )
			{
				if( x == 0 && y == 0 )
					continue;

				vec2 o = vec2( float( x ), float( y ) );
				float r = length( o );
				if( r > Weight )
					continue;

				ink = max( ink, inkAt( uv + o * TexelSize ) );
			}
		}
	}

	fragColor = vec4( ink, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 9: the temporal filter.
//---------------------------------------------------------------------------
const char* const kStabiliseShader = R"(#version 410 core

uniform sampler2D InkTexture;
uniform sampler2D HistoryTexture;
uniform float Attack;
uniform float Release;
uniform float Reset;    //1 on the first frame, or after anything upstream changed

in vec2 uv;
out vec4 fragColor;

void main()
{
	float measured = texture( InkTexture, uv ).r;
	float previous = texture( HistoryTexture, uv ).r;

	if( Reset > 0.5 )
	{
		fragColor = vec4( measured, 0.0, 0.0, 1.0 );
		return;
	}

	//Asymmetric: ink appears at the attack rate and fades at the release
	//rate. Symmetric smoothing would make the drawing lag the footage in
	//both directions, which reads as latency rather than as steadiness.
	float k = measured > previous ? Attack : Release;

	fragColor = vec4( mix( previous, measured, k ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 10: ink, paper, mix.
//---------------------------------------------------------------------------
const char* const kCompositeShader = R"(#version 410 core

uniform sampler2D InkTexture;
uniform sampler2D CopyTexture;

uniform vec3 Ink;
uniform vec3 PaperColour;
uniform float PaperMode;  //0 colour, 1 source, 2 dimmed source, 3 transparent
uniform float Dim;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

void main()
{
	float ink = texture( InkTexture, uv ).r;
	vec4 source = texture( CopyTexture, uv );

	int mode = int( PaperMode + 0.5 );

	vec4 paper;
	if( mode == 1 )
		paper = source;
	else if( mode == 2 )
		paper = vec4( source.rgb * Dim, source.a );
	else if( mode == 3 )
		paper = vec4( 0.0 );
	else
		paper = vec4( PaperColour, 1.0 );

	//Premultiplied throughout, so the ink is laid over the paper the same way
	//in every mode -- including the transparent one, where it is laid over
	//nothing and leaves the alpha for the layer below to use.
	vec4 laid = vec4( Ink, 1.0 ) * ink;
	vec4 result = laid + paper * ( 1.0 - ink );

	fragColor = mix( source, result, MixAmount );
}
)";

//---------------------------------------------------------------------------
// Assembly. The two flow-guided passes are the flow library plus their own
// main, so the eigen decomposition has exactly one home.
//---------------------------------------------------------------------------
std::string DoGShaderSource()
{
	return std::string( "#version 410 core\n" ) + kFlowLibrary + kDoGMain;
}

std::string LicShaderSource()
{
	return std::string( "#version 410 core\n" ) + kFlowLibrary + kLicMain;
}

std::string FlowProbeShaderSource()
{
	//No convolution and no tone: just the flow, written out. Folding any of
	//the filtering in would mean a disagreement could not be attributed to
	//the direction estimate, which is the only thing being measured.
	static const char* const probeMain = R"(
in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = vec4( flowAt( uv ), 1.0 );
}
)";

	return std::string( "#version 410 core\n" ) + kFlowLibrary + probeMain;
}

} // namespace nib
