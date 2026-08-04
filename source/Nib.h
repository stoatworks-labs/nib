#pragma once

#include "Controls.h"
#include "PassBuffer.h"
#include "Shaders.h"

#include <FFGLSDK.h>

namespace nib
{
/**
    The plugin.

    One effect, ten passes, no variants. See Shaders.h for the chain and
    AGENTS.md for why the flow field exists at all.
*/
class NibPlugin : public CFFGLPlugin
{
public:
	NibPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// The smoothed structure tensor, for `nibtest --flow` to probe. Nothing
	/// in the plugin's own operation reads this; it is here so the harness can
	/// measure the direction estimate the shipping shaders actually use,
	/// rather than a copy of it written for the test.
	GLuint FlowTextureID() const
	{
		return tensorBuffer.TextureID();
	}

private:
	void applyPreset( int presetIndex );

	/// Allocate every buffer for this picture size. Called once per frame
	/// before anything binds a texture, and a no-op unless the size changed.
	///
	/// All of it up front, never mid-chain: `FFGLFBO::Initialise` sizes its
	/// colour texture under a scoped binding, and every `ffglex::Scoped*`
	/// binding clears to 0 on scope exit rather than restoring what was
	/// there -- so allocating between passes silently unbinds the texture the
	/// current pass is reading. The symptom is the dangerous part: correct on
	/// every frame except the one that allocates.
	bool ensureBuffers( GLsizei width, GLsizei height );

	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader toneShader;
	ffglex::FFGLShader tensorShader;
	ffglex::FFGLShader tensorBlurShader;
	ffglex::FFGLShader dogShader;
	ffglex::FFGLShader licShader;
	ffglex::FFGLShader reinjectShader;
	ffglex::FFGLShader thresholdShader;
	ffglex::FFGLShader stabiliseShader;
	ffglex::FFGLShader compositeShader;

	ffglex::FFGLScreenQuad quad;

	PassBuffer copyBuffer;
	PassBuffer tensorBuffer;
	PassBuffer tensorTemp;   ///< the horizontal half of the separable blur
	PassBuffer toneBuffer[ 2 ];  ///< ping-ponged by the reinjection
	PassBuffer dogBuffer;
	PassBuffer licBuffer;
	PassBuffer inkBuffer;
	PassBuffer historyBuffer[ 2 ];

	int toneCurrent    = 0;
	int historyCurrent = 0;
	bool historyValid  = false;

	GLsizei bufferWidth  = 0;
	GLsizei bufferHeight = 0;
};

} // namespace nib
