#include "Nib.h"

#include "Diag.h"
#include "Presets.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;

namespace nib
{
namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be
/// the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kSourceNames[] = { "Luma", "Red", "Green", "Blue", "Saturation" };
const char* const kPaperNames[]  = { "Paper Colour", "Source", "Dimmed Source", "Transparent" };
const char* const kPassesNames[] = { "1", "2", "3" };

/// The parameters a preset covers, in the order Presets.h lists them.
constexpr unsigned int kPresetParamIDs[] = {
	PT_SCALE, PT_SHARPNESS, PT_THRESHOLD, PT_FALLOFF, PT_WEIGHT,
	PT_FLOW, PT_COHERENCE, PT_LENGTH, PT_PASSES,
	PT_STABILITY,
	PT_INK_R, PT_INK_G, PT_INK_B,
	PT_PAPER, PT_PAPER_R, PT_PAPER_G, PT_PAPER_B, PT_DIM,
};

static_assert( sizeof( kPresetParamIDs ) / sizeof( kPresetParamIDs[ 0 ] ) == presets::kParamCount,
               "the preset table and the id list it is bound to have drifted apart" );

/// Taps needed to cover a Gaussian out to where it stops contributing. Three
/// sigma is the usual answer and is not arbitrary: past it the weight is
/// under 1.2%, which is below the quantisation of the buffers being summed.
int tapsFor( float sigma )
{
	return std::clamp( static_cast< int >( std::ceil( sigma * 3.0f ) ), 1, kMaxTaps );
}
} // namespace

//---------------------------------------------------------------------------
// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree. They diverge the day somebody writes a user
// guide, and this is what says so.
static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- "
               "add or remove a PT_ABOUT_BUTTON_n to match" );

NibPlugin::NibPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//-------------------------------------------------------------------
	// Defaults. Chosen to draw something recognisable on ordinary footage
	// the moment the effect is dropped on a clip, because a line-art filter
	// whose default is a blank sheet reads as broken.
	//-------------------------------------------------------------------
	params[ PT_SOURCE ] = static_cast< float >( Source::Luma );

	params[ PT_SCALE ]     = 0.35f;//about 1.1 px: fine enough to find detail
	params[ PT_SHARPNESS ] = 0.94f;//tau 0.97 -- flat regions very nearly cancel
	params[ PT_THRESHOLD ] = 0.5f; //epsilon 0, the meaningful middle
	params[ PT_FALLOFF ]   = 0.62f;//a pen line rather than a wash
	params[ PT_WEIGHT ]    = 0.0f;

	params[ PT_FLOW ]      = 0.85f;
	params[ PT_COHERENCE ] = 0.45f;
	params[ PT_LENGTH ]    = 0.4f;
	params[ PT_PASSES ]    = 1.0f;//element 1 == two iterations

	params[ PT_STABILITY ] = 0.35f;

	params[ PT_INK_R ] = 0.05f;
	params[ PT_INK_G ] = 0.04f;
	params[ PT_INK_B ] = 0.06f;

	params[ PT_PAPER ]   = static_cast< float >( Paper::Colour );
	params[ PT_PAPER_R ] = 0.96f;
	params[ PT_PAPER_G ] = 0.95f;
	params[ PT_PAPER_B ] = 0.92f;
	params[ PT_DIM ]     = 0.25f;

	params[ PT_MIX ] = 1.0f;

	params[ PT_PRESET ] = 0.0f;//Custom: the sliders are the truth

	//-------------------------------------------------------------------
	// Declaration. Every numeric parameter is a plain 0..1 float even where
	// it stands for a radius in pixels: SetParamInfo clamps a standard
	// default into 0..1 before SetParamRange could widen it, so the ranges
	// live in Controls.cpp and nowhere else.
	//-------------------------------------------------------------------
	SetOptionParamInfo( PT_SOURCE, "Detect On", static_cast< int >( Source::Count ), params[ PT_SOURCE ] );
	for( int i = 0; i < static_cast< int >( Source::Count ); ++i )
		SetParamElementInfo( PT_SOURCE, i, kSourceNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_SCALE, "Scale", FF_TYPE_STANDARD );
	SetParamInfof( PT_SHARPNESS, "Sharpness", FF_TYPE_STANDARD );
	SetParamInfof( PT_THRESHOLD, "Threshold", FF_TYPE_STANDARD );
	SetParamInfof( PT_FALLOFF, "Falloff", FF_TYPE_STANDARD );
	SetParamInfof( PT_WEIGHT, "Weight", FF_TYPE_STANDARD );

	SetParamInfof( PT_FLOW, "Flow", FF_TYPE_STANDARD );
	SetParamInfof( PT_COHERENCE, "Coherence", FF_TYPE_STANDARD );
	SetParamInfof( PT_LENGTH, "Length", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_PASSES, "Passes", kMaxIterations, params[ PT_PASSES ] );
	for( int i = 0; i < kMaxIterations; ++i )
		SetParamElementInfo( PT_PASSES, i, kPassesNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_STABILITY, "Stability", FF_TYPE_STANDARD );

	//Consecutive red/green/blue parameters are what a host needs to show a
	//swatch rather than three sliders.
	SetParamInfof( PT_INK_R, "Ink", FF_TYPE_RED );
	SetParamInfof( PT_INK_G, "Ink_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_INK_B, "Ink_Blue", FF_TYPE_BLUE );

	SetOptionParamInfo( PT_PAPER, "Paper", static_cast< int >( Paper::Count ), params[ PT_PAPER ] );
	for( int i = 0; i < static_cast< int >( Paper::Count ); ++i )
		SetParamElementInfo( PT_PAPER, i, kPaperNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_PAPER_R, "Paper Colour", FF_TYPE_RED );
	SetParamInfof( PT_PAPER_G, "Paper_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_PAPER_B, "Paper_Blue", FF_TYPE_BLUE );

	SetParamInfof( PT_DIM, "Dim", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	//-------------------------------------------------------------------
	// Groups. SetParamGroup collapses runs of consecutive same-group ids, so
	// this depends entirely on the id order in Controls.h -- reorder that and
	// a group silently splits in two.
	//-------------------------------------------------------------------
	SetParamGroup( PT_SOURCE, "Source" );
	for( unsigned int id = PT_SCALE; id <= PT_WEIGHT; ++id )
		SetParamGroup( id, "Line" );
	for( unsigned int id = PT_FLOW; id <= PT_PASSES; ++id )
		SetParamGroup( id, "Flow" );
	SetParamGroup( PT_STABILITY, "Time" );
	for( unsigned int id = PT_INK_R; id <= PT_DIM; ++id )
		SetParamGroup( id, "Ink" );
	SetParamGroup( PT_MIX, "Output" );
	SetParamGroup( PT_PRESET, "Preset" );
	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
		SetParamGroup( id, "About" );

}

//---------------------------------------------------------------------------
FFResult NibPlugin::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not
	//compile it is almost always the driver or the GL version, and knowing
	//which machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	//Held in locals so the pointers handed to Compile outlive the call.
	const std::string dogSource = DoGShaderSource();
	const std::string licSource = LicShaderSource();

	struct Stage
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	};
	const Stage stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &toneShader, kToneShader, "tone" },
		{ &tensorShader, kTensorShader, "tensor" },
		{ &tensorBlurShader, kTensorBlurShader, "tensor blur" },
		{ &dogShader, dogSource.c_str(), "dog" },
		{ &licShader, licSource.c_str(), "lic" },
		{ &reinjectShader, kReinjectShader, "reinject" },
		{ &thresholdShader, kThresholdShader, "threshold" },
		{ &stabiliseShader, kStabiliseShader, "stabilise" },
		{ &compositeShader, kCompositeShader, "composite" },
	};

	for( const Stage& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the plugin
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Nib: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	historyValid = false;

	diag::info( "initialised" );

	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool NibPlugin::ensureBuffers( GLsizei width, GLsizei height )
{
	//Every intermediate is floating point, and that is load-bearing rather
	//than luxurious. The DoG response is a *difference* of two nearly equal
	//blurs: with tau near 1 the interesting part of the signal is a few
	//thousandths of the tone, which an 8-bit buffer quantises to nothing at
	//all. The symptom in an 8-bit chain is not a subtle loss of quality --
	//Sharpness stops working entirely above about 0.9, which is the half of
	//its range worth having.
	const bool ok =
		copyBuffer.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Linear )
		&& tensorBuffer.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Linear )
		&& tensorTemp.Ensure( width, height, GL_RGBA16F, PassBuffer::Sampling::Linear )
		&& toneBuffer[ 0 ].Ensure( width, height, GL_R16F, PassBuffer::Sampling::Linear )
		&& toneBuffer[ 1 ].Ensure( width, height, GL_R16F, PassBuffer::Sampling::Linear )
		&& dogBuffer.Ensure( width, height, GL_R16F, PassBuffer::Sampling::Linear )
		&& licBuffer.Ensure( width, height, GL_R16F, PassBuffer::Sampling::Linear )
		&& inkBuffer.Ensure( width, height, GL_R16F, PassBuffer::Sampling::Linear )
		//The history is read texel-for-texel and must not be filtered: a
		//stabilised line read between texels creeps a fraction of a texel per
		//frame and dissolves into a smear over a few seconds.
		&& historyBuffer[ 0 ].Ensure( width, height, GL_R16F, PassBuffer::Sampling::Nearest )
		&& historyBuffer[ 1 ].Ensure( width, height, GL_R16F, PassBuffer::Sampling::Nearest );

	if( !ok )
		return false;

	if( width != bufferWidth || height != bufferHeight )
	{
		bufferWidth  = width;
		bufferHeight = height;
		historyValid = false;
	}

	return true;
}

//---------------------------------------------------------------------------
FFResult NibPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );

	const GLsizei width  = static_cast< GLsizei >( source.Width );
	const GLsizei height = static_cast< GLsizei >( source.Height );
	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	//The host's viewport, captured before anything rebinds it. ScopedFBOBinding
	//restores the framebuffer but *not* the viewport (SDK b1afaf9), so without
	//this the composite inherits whatever the last pass set -- which in most
	//viewers reads as "blown out to white with a small picture in the corner"
	//rather than as a viewport bug.
	GLint hostViewport[ 4 ] = {};
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//Every allocation up front. See ensureBuffers.
	if( !ensureBuffers( width, height ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( source );

	const float texelX = 1.0f / static_cast< float >( width );
	const float texelY = 1.0f / static_cast< float >( height );

	//-------------------------------------------------------------------
	// 1. Copy.
	//-------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( copyBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		copyBuffer.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( source.Handle );

		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		copyShader.Set( "HalfTexel",
		                0.5f / static_cast< float >( width ),
		                0.5f / static_cast< float >( height ) );
		quad.Draw();
	}

	//-------------------------------------------------------------------
	// 2. Tone.
	//-------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( toneBuffer[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		toneBuffer[ 0 ].ResizeViewPort();
		ScopedShaderBinding shader( toneShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( copyBuffer.TextureID() );

		toneShader.Set( "CopyTexture", 0 );
		toneShader.Set( "SourceMode", params[ PT_SOURCE ] );
		quad.Draw();
	}
	toneCurrent = 0;

	//-------------------------------------------------------------------
	// 3. The structure tensor.
	//-------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( tensorBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		tensorBuffer.ResizeViewPort();
		ScopedShaderBinding shader( tensorShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( toneBuffer[ 0 ].TextureID() );

		tensorShader.Set( "ToneTexture", 0 );
		tensorShader.Set( "TexelSize", texelX, texelY );
		quad.Draw();
	}

	//-------------------------------------------------------------------
	// 4. Smooth it, separably. Horizontal into the temp, vertical back.
	//-------------------------------------------------------------------
	{
		const float coherence = CoherenceFromParam( params[ PT_COHERENCE ] );
		const int taps        = tapsFor( coherence );

		struct Axis
		{
			PassBuffer* target;
			PassBuffer* source;
			float dx, dy;
		};
		const Axis axes[] = {
			{ &tensorTemp, &tensorBuffer, texelX, 0.0f },
			{ &tensorBuffer, &tensorTemp, 0.0f, texelY },
		};

		for( const Axis& axis : axes )
		{
			ScopedFBOBinding fbo( axis.target->GetGLID(), ScopedFBOBinding::RB_REVERT );
			axis.target->ResizeViewPort();
			ScopedShaderBinding shader( tensorBlurShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( axis.source->TextureID() );

			tensorBlurShader.Set( "TensorTexture", 0 );
			tensorBlurShader.Set( "Direction", axis.dx, axis.dy );
			tensorBlurShader.Set( "Sigma", coherence );
			tensorBlurShader.Set( "Taps", taps );
			quad.Draw();
		}
	}

	//-------------------------------------------------------------------
	// 5-7. The iterated pair: DoG across the flow, LIC along it, then
	//      reinject into the tone for the next round.
	//-------------------------------------------------------------------
	const float scale     = ScaleFromParam( params[ PT_SCALE ] );
	const float tau       = SharpnessFromParam( params[ PT_SHARPNESS ] );
	const float threshold = ThresholdFromParam( params[ PT_THRESHOLD ] );
	const float falloff   = FalloffFromParam( params[ PT_FALLOFF ] );
	const float flow      = FlowFromParam( params[ PT_FLOW ] );
	const float length    = LengthFromParam( params[ PT_LENGTH ] );

	const int dogTaps = tapsFor( scale * kSigmaRatio );
	const int licTaps = tapsFor( length );

	const int iterations =
		std::clamp( static_cast< int >( std::lround( params[ PT_PASSES ] ) ) + 1, 1, kMaxIterations );

	for( int iteration = 0; iteration < iterations; ++iteration )
	{
		//DoG, reading whichever tone buffer the last reinjection wrote.
		{
			ScopedFBOBinding fbo( dogBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
			dogBuffer.ResizeViewPort();
			ScopedShaderBinding shader( dogShader.GetGLID() );

			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding toneTexture( toneBuffer[ toneCurrent ].TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding flowTexture( tensorBuffer.TextureID() );

			dogShader.Set( "ToneTexture", 0 );
			dogShader.Set( "FlowTexture", 1 );
			dogShader.Set( "TexelSize", texelX, texelY );
			dogShader.Set( "Sigma", scale );
			dogShader.Set( "Tau", tau );
			dogShader.Set( "Taps", dogTaps );
			quad.Draw();
		}

		//LIC. At Flow 0 this pass is skipped entirely rather than run with a
		//zero weight: skipping it is what makes the control an honest A/B
		//against plain isotropic XDoG, and it is also the cheapest the
		//plugin ever gets.
		if( flow > 0.001f )
		{
			ScopedFBOBinding fbo( licBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
			licBuffer.ResizeViewPort();
			ScopedShaderBinding shader( licShader.GetGLID() );

			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding dogTexture( dogBuffer.TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding flowTexture( tensorBuffer.TextureID() );

			licShader.Set( "DoGTexture", 0 );
			licShader.Set( "FlowTexture", 1 );
			licShader.Set( "TexelSize", texelX, texelY );
			licShader.Set( "Sigma", length );
			licShader.Set( "Strength", flow );
			licShader.Set( "Taps", licTaps );
			quad.Draw();
		}

		const bool lastIteration = iteration == iterations - 1;
		if( lastIteration )
			break;

		//Reinject into the other tone buffer.
		{
			const int next = 1 - toneCurrent;

			ScopedFBOBinding fbo( toneBuffer[ next ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			toneBuffer[ next ].ResizeViewPort();
			ScopedShaderBinding shader( reinjectShader.GetGLID() );

			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding toneTexture( toneBuffer[ toneCurrent ].TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding licTexture( ( flow > 0.001f ? licBuffer : dogBuffer ).TextureID() );

			reinjectShader.Set( "ToneTexture", 0 );
			reinjectShader.Set( "LicTexture", 1 );
			reinjectShader.Set( "Threshold", threshold );
			reinjectShader.Set( "Falloff", falloff );
			quad.Draw();

			toneCurrent = next;
		}
	}

	//-------------------------------------------------------------------
	// 8. Threshold, and the weight.
	//-------------------------------------------------------------------
	{
		const float weight = WeightFromParam( params[ PT_WEIGHT ] );

		ScopedFBOBinding fbo( inkBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		inkBuffer.ResizeViewPort();
		ScopedShaderBinding shader( thresholdShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( ( flow > 0.001f ? licBuffer : dogBuffer ).TextureID() );

		thresholdShader.Set( "LicTexture", 0 );
		thresholdShader.Set( "TexelSize", texelX, texelY );
		thresholdShader.Set( "Threshold", threshold );
		thresholdShader.Set( "Falloff", falloff );
		thresholdShader.Set( "Weight", weight );
		thresholdShader.Set( "WeightTaps", weight > 0.01f ? 1 : 0 );
		quad.Draw();
	}

	//-------------------------------------------------------------------
	// 9. Stabilise, ping-ponged against the previous frame.
	//-------------------------------------------------------------------
	const int history      = historyCurrent;
	const int historyWrite = 1 - historyCurrent;
	{
		ScopedFBOBinding fbo( historyBuffer[ historyWrite ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		historyBuffer[ historyWrite ].ResizeViewPort();
		ScopedShaderBinding shader( stabiliseShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding inkTexture( inkBuffer.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding historyTexture( historyBuffer[ history ].TextureID() );

		stabiliseShader.Set( "InkTexture", 0 );
		stabiliseShader.Set( "HistoryTexture", 1 );
		stabiliseShader.Set( "Attack", AttackFromParam( params[ PT_STABILITY ] ) );
		stabiliseShader.Set( "Release", ReleaseFromParam( params[ PT_STABILITY ] ) );
		stabiliseShader.Set( "Reset", historyValid ? 0.0f : 1.0f );
		quad.Draw();
	}
	historyCurrent = historyWrite;
	historyValid   = true;

	//-------------------------------------------------------------------
	// 10. Composite, back into the host's framebuffer and viewport.
	//-------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		ScopedShaderBinding shader( compositeShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding inkTexture( historyBuffer[ historyCurrent ].TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding copyTexture( copyBuffer.TextureID() );

		compositeShader.Set( "InkTexture", 0 );
		compositeShader.Set( "CopyTexture", 1 );
		compositeShader.Set( "Ink", params[ PT_INK_R ], params[ PT_INK_G ], params[ PT_INK_B ] );
		compositeShader.Set( "PaperColour", params[ PT_PAPER_R ], params[ PT_PAPER_G ], params[ PT_PAPER_B ] );
		compositeShader.Set( "PaperMode", params[ PT_PAPER ] );
		compositeShader.Set( "Dim", params[ PT_DIM ] );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult NibPlugin::DeInitGL()
{
	copyShader.FreeGLResources();
	toneShader.FreeGLResources();
	tensorShader.FreeGLResources();
	tensorBlurShader.FreeGLResources();
	dogShader.FreeGLResources();
	licShader.FreeGLResources();
	reinjectShader.FreeGLResources();
	thresholdShader.FreeGLResources();
	stabiliseShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	copyBuffer.Destroy();
	tensorBuffer.Destroy();
	tensorTemp.Destroy();
	toneBuffer[ 0 ].Destroy();
	toneBuffer[ 1 ].Destroy();
	dogBuffer.Destroy();
	licBuffer.Destroy();
	inkBuffer.Destroy();
	historyBuffer[ 0 ].Destroy();
	historyBuffer[ 1 ].Destroy();

	bufferWidth  = 0;
	bufferHeight = 0;
	historyValid = false;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
char* NibPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		// Function-local rather than a member: the line is built from
		// compile-time facts, so it is the same for every instance, and the
		// host only needs the pointer to outlive the call.
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

//---------------------------------------------------------------------------
FFResult NibPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult NibPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing, so they are handled
	// before any of the bookkeeping below: pressing one is not the operator
	// editing a control.
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	const float previous = params[ index ];
	params[ index ]      = value;

	//Changing what an edge *is* invalidates the history, because the numbers
	//being blended no longer measure the same thing. Without this, moving
	//Scale with Stability high leaves the old scale's drawing decaying
	//underneath the new one, which reads as the control having two effects.
	if( index == PT_SOURCE || index == PT_SCALE || index == PT_PASSES )
		historyValid = false;

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters --
	// hosts that honour the value events echo the preset's own values
	// straight back through here, and that echo must not un-set the preset.
	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
void NibPlugin::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );

		// The same invalidation SetFloatParameter does, and for the same
		// reason: this writes params[] directly, so without it a preset is
		// the one way to change the scale without dropping the history.
		if( id == PT_SOURCE || id == PT_SCALE || id == PT_PASSES )
			historyValid = false;
	}
}

//---------------------------------------------------------------------------
float NibPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

} // namespace nib
