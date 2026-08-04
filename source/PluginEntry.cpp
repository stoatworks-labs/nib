#include "Nib.h"

/**
    The one registration.

    This file is listed directly in the Nib MODULE target, not in nib_core:
    `CFFGLPluginInfo` registers itself from a file-scope constructor and
    nothing ever references it by name, so in a STATIC archive the linker is
    entitled to drop the whole translation unit -- giving a bundle that loads,
    exports `plugMain`, and reports that it contains no plugins. The core
    stays an OBJECT library for the same reason.

        nm -gU Nib.bundle/Contents/MacOS/Nib | grep plugMain
*/
namespace
{
class NibEffect : public nib::NibPlugin
{
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< NibEffect >,                           // Create method
	"NB01",                                               // Plugin unique ID of maximum length 4
	"Nib",                                                // Plugin name
	2,                                                    // API major version number
	1,                                                    // API minor version number
	0,                                                    // Plugin major version number
	1,                                                    // Plugin minor version number
	FF_EFFECT,                                            // Plugin type
	"Flow-guided line drawing: coherent ink strokes from a clip",
	"Nib FFGL effect"                                     // About
);

extern "C" const char* NibBuildStamp()
{
	return "nib " NIB_VERSION ", built " __DATE__ " " __TIME__;
}
