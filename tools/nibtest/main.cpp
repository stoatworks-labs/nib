/**
    nibtest -- render Nib offline, and measure what its flow field is doing.

    It drives the REAL plugin class over a synthetic test card. A test that
    exercises a reimplementation tests the reimplementation.

        nibtest --out /tmp/frame.png     the card, drawn
        nibtest --card /tmp/card.png     the test card on its own
        nibtest --list                   every parameter and its default
        nibtest --presets /tmp/p.png     every preset, checked live and distinct
        nibtest --flow                   does smoothing the tensor actually help?
        nibtest --bench                  time a frame at 720p through 4K

    The card is built so that one region has an **analytically known** edge
    direction: a set of concentric rings, where the tangent at any pixel is
    exactly perpendicular to the radius. That is what makes `--flow` a
    measurement rather than a picture -- the flow field's answer can be
    subtracted from the truth and the error reported in degrees.

    Several frames are rendered rather than one, always: the temporal filter
    needs to settle, and a single frame would report the unstabilised
    behaviour of a plugin whose default Stability is not zero.
*/

#include "Controls.h"
#include "Nib.h"
#include "Presets.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace nib;

namespace
{
constexpr float kPi = 3.14159265358979324f;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Integer hashing, for the noise. Never fract(sin(x)*43758.5453): that is the
// driver's answer, and two machines disagree about it.
//---------------------------------------------------------------------------
uint32_t lowbias32( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

float unitNoise( uint32_t a, uint32_t b )
{
	return static_cast< float >( lowbias32( a ^ lowbias32( b ) ) ) / 4294967296.0f;
}

//---------------------------------------------------------------------------
// The test card.
//
// The rings on the left are the load-bearing part: their tangent direction is
// known in closed form, so `--flow` has something to be right or wrong about.
// Everything else is there to give the drawing something to draw.
//---------------------------------------------------------------------------
constexpr float kRingsCentreU = 0.22f;
constexpr float kRingsCentreV = 0.5f;
constexpr float kRingsInner   = 0.05f;
constexpr float kRingsOuter   = 0.19f;
constexpr float kRingPeriod   = 0.022f;

std::vector< unsigned char > buildCard( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	const float w      = static_cast< float >( width );
	const float h      = static_cast< float >( height );
	const float aspect = w / h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			//A soft gradient everywhere, as the floor. Not flat: a detector
			//that only works on flat backgrounds is not a detector.
			float r = 0.10f + 0.14f * u;
			float g = r;
			float b = r;

			//Concentric rings, left. Analytic tangent, and closely enough
			//spaced that an isotropic filter at any useful scale merges
			//neighbouring rings while a flow-guided one does not.
			const float rdx = ( u - kRingsCentreU ) * aspect;
			const float rdy = v - kRingsCentreV;
			const float rr  = std::sqrt( rdx * rdx + rdy * rdy );
			if( rr > kRingsInner && rr < kRingsOuter )
			{
				const float phase = std::fmod( rr, kRingPeriod ) / kRingPeriod;
				r = g = b = phase < 0.5f ? 0.88f : 0.12f;
			}

			//Parallel diagonal bars, right of centre. A second known
			//direction, and a straight one.
			if( u > 0.52f && u < 0.74f && v > 0.12f && v < 0.88f )
			{
				const float t     = u * aspect * 0.5f + v * 0.866f;
				const float phase = std::fmod( t, 0.05f ) / 0.05f;
				r = g = b = phase < 0.5f ? 0.92f : 0.08f;
			}

			//A plain disc, right. Something with one clean closed contour.
			const float ddx = ( u - 0.86f ) * aspect;
			const float ddy = v - 0.5f;
			if( std::sqrt( ddx * ddx + ddy * ddy ) < 0.11f )
				r = g = b = 0.95f;

			//Two fields of equal luminance, bottom left: invisible to a luma
			//reading, obvious to a saturation one.
			if( v < 0.10f && u < 0.42f )
			{
				const bool right = u > 0.21f;
				r                = right ? 0.10f : 0.9333f;
				g                = right ? 0.2775f : 0.0f;
				b                = 0.0f;
			}

			const size_t o                  = ( static_cast< size_t >( y ) * width + x ) * 4;
			image[ o + 0 ] = static_cast< unsigned char >( std::min( 255.0f, r * 255.0f ) );
			image[ o + 1 ] = static_cast< unsigned char >( std::min( 255.0f, g * 255.0f ) );
			image[ o + 2 ] = static_cast< unsigned char >( std::min( 255.0f, b * 255.0f ) );
			image[ o + 3 ] = 255;
		}
	}

	return image;
}

/// The card with deterministic per-frame noise. The only way to test
/// Stability -- on a still picture a temporal filter provably does nothing --
/// and the condition under which tensor smoothing is supposed to pay for
/// itself.
std::vector< unsigned char > addNoise( const std::vector< unsigned char >& card, int frame, float amount )
{
	std::vector< unsigned char > noisy = card;
	if( amount <= 0.0f )
		return noisy;

	const float scale = amount * 255.0f;
	for( size_t i = 0; i < noisy.size(); i += 4 )
	{
		const float jitter =
			( unitNoise( static_cast< uint32_t >( i / 4 ), static_cast< uint32_t >( frame ) ) - 0.5f ) * scale;

		for( int c = 0; c < 3; ++c )
		{
			const float value = static_cast< float >( noisy[ i + c ] ) + jitter;
			noisy[ i + c ]    = static_cast< unsigned char >( std::min( 255.0f, std::max( 0.0f, value ) ) );
		}
	}
	return noisy;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

std::vector< unsigned char > readBackRaw( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

//---------------------------------------------------------------------------
// Shader compilation, for the flow probe. The plugin uses ffglex for this;
// the probe cannot, because ffglex::FFGLShader insists on a vertex shader
// with the SDK's attribute layout and the probe draws its own quad.
//---------------------------------------------------------------------------
GLuint compileStage( GLenum type, const std::string& source, std::string& error )
{
	const GLuint stage    = glCreateShader( type );
	const char* const src = source.c_str();
	glShaderSource( stage, 1, &src, nullptr );
	glCompileShader( stage );

	GLint ok = GL_FALSE;
	glGetShaderiv( stage, GL_COMPILE_STATUS, &ok );
	if( ok == GL_TRUE )
		return stage;

	GLint length = 0;
	glGetShaderiv( stage, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetShaderInfoLog( stage, length, nullptr, log.data() );
	error = log;
	glDeleteShader( stage );
	return 0;
}

GLuint buildProbeProgram( std::string& error )
{
	static const char* const vertex = R"(#version 410 core
layout( location = 0 ) in vec2 vPosition;
out vec2 uv;
void main()
{
	uv = vPosition * 0.5 + 0.5;
	gl_Position = vec4( vPosition, 0.0, 1.0 );
}
)";

	const GLuint vs = compileStage( GL_VERTEX_SHADER, vertex, error );
	if( vs == 0 )
		return 0;

	const GLuint fs = compileStage( GL_FRAGMENT_SHADER, FlowProbeShaderSource(), error );
	if( fs == 0 )
	{
		glDeleteShader( vs );
		return 0;
	}

	const GLuint program = glCreateProgram();
	glAttachShader( program, vs );
	glAttachShader( program, fs );
	glLinkProgram( program );
	glDeleteShader( vs );
	glDeleteShader( fs );

	GLint ok = GL_FALSE;
	glGetProgramiv( program, GL_LINK_STATUS, &ok );
	if( ok == GL_TRUE )
		return program;

	GLint length = 0;
	glGetProgramiv( program, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	glGetProgramInfoLog( program, length, nullptr, log.data() );
	error = log;
	glDeleteProgram( program );
	return 0;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	std::string kind;
};

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_RED: return "red";
	case FF_TYPE_GREEN: return "green";
	case FF_TYPE_BLUE: return "blue";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( NibPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ),
		                                 kindName( plugin.GetParamType( i ) ) } );
	}
	return list;
}

bool applySetting( NibPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	for( const NamedParameter& parameter : listParameters( plugin ) )
	{
		if( parameter.name != name )
			continue;
		plugin.SetFloatParameter( parameter.index, std::strtof( value.c_str(), nullptr ) );
		return true;
	}

	error = "no parameter called '" + name + "'";
	return false;
}

//---------------------------------------------------------------------------
// A rendering session: context, card, textures, and a render() that drives
// the real plugin the way a host would.
//---------------------------------------------------------------------------
struct Session
{
	int width  = 1280;
	int height = 720;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;

	std::vector< unsigned char > card;
	ProcessOpenGLStruct process = {};
	FFGLTextureStruct inputStruct = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	bool begin( NibPlugin& plugin, int w, int h )
	{
		width  = w;
		height = h;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}

		card          = buildCard( width, height );
		sourceTexture = makeTexture( width, height, card.data() );
		outputTexture = makeTexture( width, height, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
		return true;
	}

	/// Render `frames` frames, optionally re-uploading noisy source each one.
	/// More than one always: the temporal filter has to settle or the picture
	/// is of a plugin mid-transient.
	bool run( NibPlugin& plugin, int frames, float noise )
	{
		for( int frame = 0; frame < frames; ++frame )
		{
			if( noise > 0.0f )
			{
				const std::vector< unsigned char > noisy = addNoise( card, frame, noise );
				glBindTexture( GL_TEXTURE_2D, sourceTexture );
				glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, noisy.data() );
				glBindTexture( GL_TEXTURE_2D, 0 );
			}

			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );

			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	std::vector< unsigned char > readback() const
	{
		return flipRows( readBackRaw( outputFBO, width, height ), width, height );
	}
};

//---------------------------------------------------------------------------
// --flow. The measurement this plugin has to survive.
//
// Claim under test: averaging the *structure tensor* over a neighbourhood is
// a better estimate of which way the drawing runs than the per-pixel gradient
// is, once there is any noise at all -- and that is the entire justification
// for the tensor pass existing rather than a plain Sobel direction.
//
// The rings give an analytic answer, so the flow field's direction can simply
// be subtracted from the truth. Coherence is swept from its minimum (barely
// any smoothing, which is as close to the raw gradient as the plugin gets) to
// a working value, under fixed noise. If the error does not fall, the pass is
// not earning its cost and the claim in AGENTS.md is wrong.
//---------------------------------------------------------------------------
int runFlowCheck()
{
	constexpr int kWidth  = 960;
	constexpr int kHeight = 540;
	constexpr float kNoise = 0.10f;

	std::string error;
	const GLuint probe = buildProbeProgram( error );
	if( probe == 0 )
	{
		std::fprintf( stderr, "flow probe failed to build: %s\n", error.c_str() );
		return 1;
	}

	//A float target, because the direction is a unit vector with a sign and
	//an 8-bit readback would quantise the error being measured to coarser
	//than the differences between the settings being compared.
	GLuint probeTexture = 0;
	glGenTextures( 1, &probeTexture );
	glBindTexture( GL_TEXTURE_2D, probeTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, kWidth, kHeight, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );
	const GLuint probeFBO = makeFramebuffer( probeTexture );

	GLuint vao = 0, vbo = 0;
	glGenVertexArrays( 1, &vao );
	glBindVertexArray( vao );
	const float quad[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
	glGenBuffers( 1, &vbo );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( quad ), quad, GL_STATIC_DRAW );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 0, nullptr );
	glBindVertexArray( 0 );

	struct Result
	{
		float coherence;
		double meanErrorDegrees;
		double sampled;
	};
	std::vector< Result > results;

	//0 is the minimum of the Coherence control, which maps to sigma 0.5 --
	//about as unsmoothed as the tensor ever gets. The rest walk up to a
	//working value.
	const float settings[] = { 0.0f, 0.25f, 0.5f, 0.75f };

	for( float setting : settings )
	{
		NibPlugin plugin;
		plugin.SetFloatParameter( PT_COHERENCE, setting );

		Session session;
		if( !session.begin( plugin, kWidth, kHeight ) )
			return 1;
		if( !session.run( plugin, 8, kNoise ) )
			return 1;

		//Probe the flow the plugin just computed.
		glBindFramebuffer( GL_FRAMEBUFFER, probeFBO );
		glViewport( 0, 0, kWidth, kHeight );
		glUseProgram( probe );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, plugin.FlowTextureID() );
		glUniform1i( glGetUniformLocation( probe, "FlowTexture" ), 0 );
		glBindVertexArray( vao );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
		glBindVertexArray( 0 );

		std::vector< float > pixels( static_cast< size_t >( kWidth ) * kHeight * 4 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, kWidth, kHeight, GL_RGBA, GL_FLOAT, pixels.data() );

		const float aspect = static_cast< float >( kWidth ) / static_cast< float >( kHeight );

		double sum   = 0.0;
		double count = 0.0;

		for( int y = 0; y < kHeight; ++y )
		{
			for( int x = 0; x < kWidth; ++x )
			{
				const float u = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( kWidth );
				const float v = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( kHeight );

				const float dx = ( u - kRingsCentreU ) * aspect;
				const float dy = v - kRingsCentreV;
				const float rr = std::sqrt( dx * dx + dy * dy );

				//Inside the ring field only, and away from its two rims where
				//the truth stops being the rings' tangent.
				if( rr < kRingsInner + 0.02f || rr > kRingsOuter - 0.02f )
					continue;

				//The truth: tangent to a circle is perpendicular to the radius.
				const float tx = -dy / rr;
				const float ty = dx / rr;

				const size_t o = ( static_cast< size_t >( y ) * kWidth + x ) * 4;
				float fx       = pixels[ o + 0 ];
				float fy       = pixels[ o + 1 ];

				const float len = std::sqrt( fx * fx + fy * fy );
				if( len < 1e-6f )
					continue;
				fx /= len;
				fy /= len;

				//An orientation, not a direction: the eigenvector's sign is
				//arbitrary, so 179 degrees out is the same as 1 degree out.
				//Comparing without the absolute value would report a correct
				//field as maximally wrong on half its pixels.
				const float dot   = std::fabs( fx * tx + fy * ty );
				const float angle = std::acos( std::min( 1.0f, dot ) ) * 180.0f / kPi;

				sum += angle;
				count += 1.0;
			}
		}

		plugin.DeInitGL();
		glDeleteTextures( 1, &session.sourceTexture );
		glDeleteTextures( 1, &session.outputTexture );
		glDeleteFramebuffers( 1, &session.outputFBO );

		results.push_back( Result { CoherenceFromParam( setting ), count > 0 ? sum / count : 0.0, count } );
	}

	glDeleteBuffers( 1, &vbo );
	glDeleteVertexArrays( 1, &vao );
	glDeleteFramebuffers( 1, &probeFBO );
	glDeleteTextures( 1, &probeTexture );
	glDeleteProgram( probe );

	std::printf( "flow: mean angular error against the rings' analytic tangent, %.0f%% noise\n",
	             kNoise * 100.0f );
	for( const Result& r : results )
		std::printf( "  coherence sigma %5.2f px   %6.2f degrees   (%.0f pixels)\n",
		             r.coherence, r.meanErrorDegrees, r.sampled );

	if( results.size() < 2 || results.front().sampled < 1000.0 )
	{
		std::printf( "  FAIL  not enough sampled pixels to conclude anything\n" );
		return 1;
	}

	const double worst = results.front().meanErrorDegrees;
	double best        = worst;
	for( const Result& r : results )
		best = std::min( best, r.meanErrorDegrees );

	//A third is not a tuned number, it is a floor: anything less and the pass
	//is not worth its cost, and the honest thing would be to delete it and
	//use the raw gradient.
	const double improvement = worst > 0.0 ? ( worst - best ) / worst : 0.0;
	std::printf( "  best is %.0f%% better than the least-smoothed setting\n", improvement * 100.0 );

	if( improvement < 0.33 )
	{
		std::printf( "  FAIL  smoothing the tensor is not earning the pass it costs\n" );
		return 1;
	}

	std::printf( "  ok    the flow field is a better direction estimate than the gradient\n" );
	return 0;
}

//---------------------------------------------------------------------------
// Contact sheet. Same shape as the rest of the fleet's: run the real plugin
// once per entry, tile the frames, and assert two things a human would
// otherwise have to keep noticing -- every tile has a drawing in it, and no
// two tiles are the same picture.
//---------------------------------------------------------------------------
double tileEnergy( const std::vector< unsigned char >& tile )
{
	double sum = 0.0;
	for( size_t i = 0; i < tile.size(); i += 4 )
		sum += tile[ i ] + tile[ i + 1 ] + tile[ i + 2 ];
	return sum / ( static_cast< double >( tile.size() / 4 ) * 3.0 * 255.0 );
}

double tileDifference( const std::vector< unsigned char >& a, const std::vector< unsigned char >& b )
{
	double sum = 0.0;
	for( size_t i = 0; i < a.size(); i += 4 )
		sum += std::abs( int( a[ i ] ) - int( b[ i ] ) )
		     + std::abs( int( a[ i + 1 ] ) - int( b[ i + 1 ] ) )
		     + std::abs( int( a[ i + 2 ] ) - int( b[ i + 2 ] ) );
	return sum / ( static_cast< double >( a.size() / 4 ) * 3.0 * 255.0 );
}

int runPresetSheet( const std::string& path )
{
	constexpr int tileW = 480, tileH = 270;
	constexpr int columns = 4;

	std::vector< std::vector< unsigned char > > tiles;
	std::vector< std::string > names;

	for( int entry = 0; entry < presets::kCount; ++entry )
	{
		NibPlugin plugin;
		//Element 0 of the host dropdown is Custom, so preset i is element
		//i + 1. Going through SetFloatParameter rather than poking params is
		//the point: it is the path the host takes, so the sheet also proves
		//applyPreset, not just the numbers in the table.
		plugin.SetFloatParameter( PT_PRESET, static_cast< float >( entry + 1 ) );

		Session session;
		if( !session.begin( plugin, tileW, tileH ) )
			return 1;
		if( !session.run( plugin, 10, 0.0f ) )
			return 1;

		tiles.push_back( session.readback() );
		names.push_back( presets::kPresets[ entry ].name );

		plugin.DeInitGL();
		glDeleteTextures( 1, &session.sourceTexture );
		glDeleteTextures( 1, &session.outputTexture );
		glDeleteFramebuffers( 1, &session.outputFBO );
	}

	const int count = static_cast< int >( tiles.size() );
	const int rows  = ( count + columns - 1 ) / columns;
	std::vector< unsigned char > image(
		static_cast< size_t >( tileW * columns ) * static_cast< size_t >( tileH * rows ) * 4, 0 );

	for( int i = 0; i < count; ++i )
	{
		const int cx = ( i % columns ) * tileW;
		const int cy = ( i / columns ) * tileH;
		for( int y = 0; y < tileH; ++y )
		{
			const size_t to   = ( static_cast< size_t >( cy + y ) * ( tileW * columns ) + cx ) * 4;
			const size_t from = static_cast< size_t >( y ) * tileW * 4;
			std::memcpy( image.data() + to, tiles[ i ].data() + from, static_cast< size_t >( tileW ) * 4 );
		}
	}

	if( !writePng( path, tileW * columns, tileH * rows, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", path.c_str() );
		return 1;
	}

	int failures = 0;
	for( int i = 0; i < count; ++i )
	{
		const double energy = tileEnergy( tiles[ i ] );
		if( energy < 0.002 )
		{
			std::printf( "  FAIL  preset '%s' rendered black (energy %.5f)\n", names[ i ].c_str(), energy );
			++failures;
		}
	}
	for( int i = 0; i < count; ++i )
	{
		for( int j = i + 1; j < count; ++j )
		{
			const double difference = tileDifference( tiles[ i ], tiles[ j ] );
			if( difference < 0.001 )
			{
				std::printf( "  FAIL  presets '%s' and '%s' rendered the same picture (mad %.6f)\n",
				             names[ i ].c_str(), names[ j ].c_str(), difference );
				++failures;
			}
		}
	}

	std::printf( "wrote %s -- %d presets%s\n", path.c_str(), count,
	             failures == 0 ? ", all live and distinct" : "" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };

	for( const Size& size : sizes )
	{
		NibPlugin plugin;
		Session session;
		if( !session.begin( plugin, size.w, size.h ) )
			return 1;

		//Warm up: the first frames allocate, compile pipeline state and
		//settle the temporal filter. Timing them measures the driver.
		if( !session.run( plugin, 10, 0.0f ) )
			return 1;

		constexpr int kTimed = 30;
		const auto start     = std::chrono::steady_clock::now();
		if( !session.run( plugin, kTimed, 0.0f ) )
			return 1;
		glFinish();
		const auto end = std::chrono::steady_clock::now();

		const double ms =
			std::chrono::duration< double, std::milli >( end - start ).count() / static_cast< double >( kTimed );
		std::printf( "  %-6s %6.2f ms/frame\n", size.name, ms );

		plugin.DeInitGL();
		glDeleteTextures( 1, &session.sourceTexture );
		glDeleteTextures( 1, &session.outputTexture );
		glDeleteFramebuffers( 1, &session.outputFBO );
	}
	return 0;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/nib.png";
	std::string cardPath;
	std::string presetsPath;
	std::vector< std::string > settings;
	int width   = 1280;
	int height  = 720;
	int frames  = 12;
	float noise = 0.0f;
	bool doList = false;
	bool doFlow = false;
	bool doBench = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" || argument == "-h" )
		{
			std::printf(
				"nibtest -- render Nib offline and measure its flow field\n\n"
				"  --out PATH            render the card and write it here\n"
				"  --card PATH           write the test card itself\n"
				"  --size WxH            render size (default 1280x720)\n"
				"  --frames N            frames to render before reading back (default 12)\n"
				"  --noise F             per-frame noise on the card, 0..1. What Stability is for.\n"
				"  --set \"Name=V\"        set a parameter by its display name. Repeatable.\n"
				"  --list                print every parameter and its default, then exit\n"
				"  --presets PATH        contact sheet of every preset, checked live and distinct\n"
				"  --flow                measure the flow field against an analytic tangent\n"
				"  --bench               time a frame at 720p through 4K\n"
				"  --help\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--presets" && hasNext )
			presetsPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--noise" && hasNext )
			noise = std::strtof( argv[ ++i ], nullptr );
		else if( argument == "--list" )
			doList = true;
		else if( argument == "--flow" )
			doFlow = true;
		else if( argument == "--bench" )
			doBench = true;
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
		}
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}

	//--list needs no GL at all: the parameters are declared in the
	//constructor.
	if( doList )
	{
		NibPlugin plugin;
		std::printf( "%-3s %-16s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-16s %-9s %.4f\n", parameter.index, parameter.name.c_str(),
			             parameter.kind.c_str(), parameter.value );
		return 0;
	}

	if( !cardPath.empty() )
	{
		const std::vector< unsigned char > card = buildCard( width, height );
		if( !writePng( cardPath, width, height, card ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s -- the test card, %dx%d\n", cardPath.c_str(), width, height );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;

	if( doFlow )
		result = runFlowCheck();
	else if( !presetsPath.empty() )
		result = runPresetSheet( presetsPath );
	else if( doBench )
		result = runBench();
	else
	{
		NibPlugin plugin;
		for( const std::string& setting : settings )
		{
			std::string error;
			if( !applySetting( plugin, setting, error ) )
			{
				std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
				return 2;
			}
		}

		Session session;
		if( !session.begin( plugin, width, height ) || !session.run( plugin, frames, noise ) )
			result = 1;
		else
		{
			if( writePng( outPath, width, height, session.readback() ) )
				std::printf( "wrote %s -- %dx%d, %d frames\n", outPath.c_str(), width, height, frames );
			else
			{
				std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
				result = 1;
			}
		}
		plugin.DeInitGL();
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
