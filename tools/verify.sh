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
step "Dead controls"
#---------------------------------------------------------------------------
# The only thing that catches a uniform whose name does not match the C++.
python3 tools/sweep.py --build "$(basename "$BUILD")"

printf '\n\033[32mall green\033[0m\n'
