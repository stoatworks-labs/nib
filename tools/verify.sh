#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# The build is universal on purpose. An arm64-only bundle builds and tests
# perfectly well here and then fails to load in an Intel Resolume, and the
# build log calls it a success either way -- so the architecture is checked
# with lipo, never with the log.
#
#     tools/verify.sh [BUILD_DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$REPO/build-verify}"

cd "$REPO"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '\033[31mFAIL\033[0m %s\n' "$1"; exit 1; }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# Shaders the plugin assembles at run time.
# Mirrors DoGShaderSource()/LicShaderSource()/FlowProbeShaderSource().
ASSEMBLED = {
	"DoGShader":       [ "#version 410 core\n", "kFlowLibrary", "kDoGMain" ],
	"LicShader":       [ "#version 410 core\n", "kFlowLibrary", "kLicMain" ],
	"FlowProbeShader": [ "#version 410 core\n", "kFlowLibrary", "probeMain" ],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	# An int indexes the raw strings that are not assigned to a name, in source
	# order. A literal starts with #version. Anything else names a constant
	# above -- and a name that has moved is a KeyError here, not a silent skip.
	if isinstance( p, int ):       return unnamed[ p ]
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

#---------------------------------------------------------------------------
step "Shaders"
#---------------------------------------------------------------------------
shaders_compile || fail "a shader does not compile"

#---------------------------------------------------------------------------
step "Submodule"
#---------------------------------------------------------------------------
if [[ ! -f external/ffgl/CMakeLists.txt ]]; then
	fail "FFGL SDK missing -- run: git submodule update --init --recursive"
fi
echo "ok   FFGL SDK present at $(git -C external/ffgl rev-parse --short HEAD)"

#---------------------------------------------------------------------------
step "Build (universal)"
#---------------------------------------------------------------------------
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)" >/dev/null
echo "ok   built"

#---------------------------------------------------------------------------
step "Bundle"
#---------------------------------------------------------------------------
binary="$BUILD/Nib.bundle/Contents/MacOS/Nib"

[[ -f "$binary" ]] || fail "no binary at $binary"

# Universal. The failure this catches ships a plugin that simply does not
# appear in half the Resolume installs it is given to.
arches="$(lipo -archs "$binary")"
[[ "$arches" == *arm64* ]]  || fail "no arm64 slice (got: $arches)"
[[ "$arches" == *x86_64* ]] || fail "no x86_64 slice (got: $arches)"

# The entry point. A bundle whose registration got dropped by the linker still
# loads and still exports this -- the OBJECT-library note in CMakeLists.txt is
# what actually guards the registration; this catches a build that produced no
# module at all.
# Captured, then matched from a herestring -- never `nm ... | grep -q`.
# Under `set -o pipefail` a `grep -q` that finds its match exits
# immediately, the writer upstream takes SIGPIPE, and the PIPELINE
# reports failure even though the symbol is there. It is output-size
# dependent, so it fires on the bigger binary first and looks
# intermittent. A herestring is not a pipeline, so nothing can SIGPIPE.
symbols=$( nm -gU "$binary" 2>/dev/null || true )
grep -q '_plugMain' <<<"$symbols" || fail "plugMain not exported"

echo "ok   Nib: $arches, plugMain exported"

#---------------------------------------------------------------------------
step "Checks"
#---------------------------------------------------------------------------
# The flow measurement is the one that matters: it is the only thing standing
# between "the tensor pass is worth its cost" as a claim and as a fact. The
# preset sheet proves every instrument is alive and that no two are the same
# picture.
"$BUILD/nibtest" --flow
"$BUILD/nibtest" --presets "$BUILD/presets-sheet.png"

#---------------------------------------------------------------------------
step "Pipe"
#---------------------------------------------------------------------------
# The fleet's --pipe contract: whole frames only, a cue naming no parameter
# refused with exit 2, and exit 1 -- never SIGPIPE's 141 -- on a failed render
# or a closed stdout. Under `set -e` a failing pipeline would end the script
# before it could say which case failed, so the checks run with it off.
set +e
pipe_frame=$(( 64 * 36 * 4 ))
pipe_raw=$( mktemp ); pipe_many=$( mktemp ); pipe_cues=$( mktemp )
head -c $(( pipe_frame * 5 / 2 )) /dev/zero > "$pipe_raw"
head -c $(( pipe_frame * 40 )) /dev/zero > "$pipe_many"

pipe_got=$( "$BUILD/nibtest" --pipe --size 64x36 < "$pipe_raw" 2>/dev/null | wc -c | tr -d ' ' )
pipe_status=${PIPESTATUS[0]}
[[ "$pipe_status" -eq 0 && "$pipe_got" = "$(( pipe_frame * 2 ))" ]] \
	|| fail "--pipe: 2.5 frames in gave $pipe_got bytes out (want $(( pipe_frame * 2 ))), exit $pipe_status"
echo "ok   2.5 frames in, exactly 2 frames out, clean exit"

# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever nibtest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$pipe_cues"
"$BUILD/nibtest" --pipe --size 64x36 --script "$pipe_cues" < "$pipe_raw" >/dev/null 2>&1
pipe_status=$?
[[ "$pipe_status" -eq 2 ]] || fail "--pipe: a cue naming no parameter gave exit $pipe_status, not 2"
echo "ok   a cue naming no parameter is refused (exit 2)"

# A failed render stops the stream with exit 1 and nothing after it. The
# failure is injected by the harness (--fail-render-at), because the plugin
# only fails on input no ffmpeg would send.
pipe_got=$( "$BUILD/nibtest" --pipe --size 64x36 --fail-render-at 1 < "$pipe_raw" 2>/dev/null | wc -c | tr -d ' ' )
pipe_status=${PIPESTATUS[0]}
[[ "$pipe_status" -eq 1 && "$pipe_got" = "$pipe_frame" ]] \
	|| fail "--pipe: a failed render at frame 1 gave exit $pipe_status and $pipe_got bytes (want 1 and $pipe_frame)"
echo "ok   a failed render at frame 1: exit 1, one frame out"

# A reader that takes one byte and goes away: forty frames is far more than a
# pipe buffer holds, so a write after head leaves must fail. Exit 1, said on
# stderr -- not the 141 of a process SIGPIPE killed before it could say anything.
"$BUILD/nibtest" --pipe --size 64x36 < "$pipe_many" 2>/dev/null | head -c 1 >/dev/null
pipe_status=${PIPESTATUS[0]}
[[ "$pipe_status" -eq 1 ]] || fail "--pipe: a closed stdout gave exit $pipe_status, not 1"
echo "ok   a closed stdout (| head -c 1): exit 1"
rm -f "$pipe_raw" "$pipe_many" "$pipe_cues"
set -e

#---------------------------------------------------------------------------
step "Dead controls"
#---------------------------------------------------------------------------
# The only thing that catches a uniform whose name does not match the C++.
python3 tools/sweep.py --build "$(basename "$BUILD")"

printf '\n\033[32mall green\033[0m\n'
