"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/shaders.js` holds the twelve GLSL pieces of `source/Shaders.cpp`: the
eight whole passes (kVertexShader, kCopyShader, kToneShader, kTensorShader,
kTensorBlurShader, kReinjectShader, kThresholdShader, kStabiliseShader,
kCompositeShader) and the three pieces the two flow-guided passes are
assembled from (kFlowLibrary, kDoGMain, kLicMain). That is two copies of the
same text, and two copies drift -- quietly, because a line drawn by a
*plausible* shader looks exactly like one drawn by the right one. The whole
claim of the page is that it runs the plugin's own shaders, so the claim needs
something enforcing it. `nibtest` drives the real plugin class and has never
heard of this page, and the glslc step in `tools/verify.sh` compiles the C++
copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each piece out of `source/Shaders.cpp` and the matching backtick literal
out of `demo/shaders.js`, and compares them exactly -- no whitespace
normalisation, no comment stripping: a comment updated on one side only is
exactly the drift worth catching.

It also fails when Shaders.cpp grows a piece this script does not list (a new
pass the page would silently lack), and it checks the JOIN as well as the
pieces: `DoGShaderSource()` and `LicShaderSource()` are the version line plus
kFlowLibrary plus their own main, and InitGL in Nib.cpp compiles every one of
the ten passes against kVertexShader. plugin.js must assemble them the same
way. A change to either side's assembly fails here and needs a reader.

The flow probe's `probeMain` is not listed and is not on the page: it lives
inside `FlowProbeShaderSource()`, exists only for `nibtest --flow`, and the
plugin never compiles it.

The one transformation is a decode, not a normalisation: a backtick cannot
appear raw inside a template literal, so --write escapes one as \\`. kFlowLibrary
and kDoGMain have them, in comments. This undoes that escape and REJECTS any
other backslash or any `${` on the JS side; there are none in the C++ bodies, so
either could only be somebody hiding a difference.

`--write` produces shaders.js from the C++ by exactly that escape, so the copy
is spliced by this script and never typed.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half, which for this plugin is small:
Controls.cpp (the 0..1 mappings), Presets.h (the table) and the host glue of
Nib.cpp's ProcessOpenGL and SetFloatParameter (tap counts, the pass order, the
iteration count, when the history is dropped) are a hand translation in
demo/plugin.js. Only a reader checks those. When you change one of them, change
the port too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# JS constant, C++ symbol (all in source/Shaders.cpp), in the order they are
# declared there.
SHADERS = [
    ("VERTEX", "kVertexShader"),
    ("COPY", "kCopyShader"),
    ("TONE", "kToneShader"),
    ("TENSOR", "kTensorShader"),
    ("TENSOR_BLUR", "kTensorBlurShader"),
    ("FLOW_LIBRARY", "kFlowLibrary"),
    ("DOG_MAIN", "kDoGMain"),
    ("LIC_MAIN", "kLicMain"),
    ("REINJECT", "kReinjectShader"),
    ("THRESHOLD", "kThresholdShader"),
    ("STABILISE", "kStabiliseShader"),
    ("COMPOSITE", "kCompositeShader"),
]

# How the pieces are joined, on each side. Each entry is (file, text that must
# appear in it verbatim). The C++ lines are the plugin's assembly; the JS lines
# are plugin.js's copy of it. A change to either is a change to what runs.
ASSEMBLY = [
    ("source/Shaders.cpp",
     'std::string DoGShaderSource()\n{\n\treturn std::string( "#version 410 core\\n" ) + kFlowLibrary + kDoGMain;\n}'),
    ("source/Shaders.cpp",
     'std::string LicShaderSource()\n{\n\treturn std::string( "#version 410 core\\n" ) + kFlowLibrary + kLicMain;\n}'),
    ("source/Nib.cpp", "if( stage.shader->Compile( kVertexShader, stage.fragment ) )"),
    ("source/Nib.cpp", '{ &copyShader, kCopyShader, "copy" },'),
    ("source/Nib.cpp", '{ &toneShader, kToneShader, "tone" },'),
    ("source/Nib.cpp", '{ &tensorShader, kTensorShader, "tensor" },'),
    ("source/Nib.cpp", '{ &tensorBlurShader, kTensorBlurShader, "tensor blur" },'),
    ("source/Nib.cpp", '{ &dogShader, dogSource.c_str(), "dog" },'),
    ("source/Nib.cpp", '{ &licShader, licSource.c_str(), "lic" },'),
    ("source/Nib.cpp", '{ &reinjectShader, kReinjectShader, "reinject" },'),
    ("source/Nib.cpp", '{ &thresholdShader, kThresholdShader, "threshold" },'),
    ("source/Nib.cpp", '{ &stabiliseShader, kStabiliseShader, "stabilise" },'),
    ("source/Nib.cpp", '{ &compositeShader, kCompositeShader, "composite" },'),
    ("demo/plugin.js", "const VERSION_LINE = '#version 410 core\\n';"),
    ("demo/plugin.js", "const dogSource = VERSION_LINE + S.FLOW_LIBRARY + S.DOG_MAIN;"),
    ("demo/plugin.js", "const licSource = VERSION_LINE + S.FLOW_LIBRARY + S.LIC_MAIN;"),
    ("demo/plugin.js", "copy: new Program(gl, S.VERTEX, S.COPY, 'copy'),"),
    ("demo/plugin.js", "tone: new Program(gl, S.VERTEX, S.TONE, 'tone'),"),
    ("demo/plugin.js", "tensor: new Program(gl, S.VERTEX, S.TENSOR, 'tensor'),"),
    ("demo/plugin.js", "tensorBlur: new Program(gl, S.VERTEX, S.TENSOR_BLUR, 'tensor blur'),"),
    ("demo/plugin.js", "dog: new Program(gl, S.VERTEX, dogSource, 'dog'),"),
    ("demo/plugin.js", "lic: new Program(gl, S.VERTEX, licSource, 'lic'),"),
    ("demo/plugin.js", "reinject: new Program(gl, S.VERTEX, S.REINJECT, 'reinject'),"),
    ("demo/plugin.js", "threshold: new Program(gl, S.VERTEX, S.THRESHOLD, 'threshold'),"),
    ("demo/plugin.js", "stabilise: new Program(gl, S.VERTEX, S.STABILISE, 'stabilise'),"),
    ("demo/plugin.js", "composite: new Program(gl, S.VERTEX, S.COMPOSITE, 'composite'),"),
]

HEADER = """// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \\` for a backtick inside a comment.
"""


def read(*parts):
    with open(os.path.join(REPO, *parts)) as handle:
        return handle.read()


def from_cpp(source, symbol):
    """The piece's text: its raw strings, joined as the compiler joins them.

    Returns (text, count of raw strings), or (None, 0) if the symbol is not
    there or what follows its `=` is not a run of adjacent raw strings ending
    in `;`.
    """
    match = re.search(r'const char\* const ' + symbol + r' = ((?:R"\(.*?\)"\s*)+);', source, re.S)
    if match is None:
        return None, 0
    parts = re.findall(r'R"\((.*?)\)"', match.group(1), re.S)
    # Nothing but whitespace between the raw strings: re-joining them must
    # give back exactly what the regex matched.
    rebuilt = re.sub(r'R"\(.*?\)"', "", match.group(1), flags=re.S)
    if rebuilt.strip() != "":
        return None, 0
    return "".join(parts), len(parts)


def cpp_pieces(source):
    """Every raw-string piece Shaders.cpp defines at file scope, by symbol."""
    return re.findall(r'^(?:static )?const char\* const (k\w+) = R"\(', source, re.M)


def from_js(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def write(cpp):
    out = [HEADER]
    for name, symbol in SHADERS:
        body, count = from_cpp(cpp, symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp (or not a run of adjacent raw strings)")
            return 1
        if "\\" in body or "${" in body:
            print(f"FAIL  {symbol} holds a backslash or ${{; the escape scheme cannot carry it")
            return 1
        joined = f", {count} adjacent raw strings joined" if count > 1 else ""
        out.append(f"\n// {symbol}, source/Shaders.cpp{joined}\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
        handle.write("".join(out))
    print("wrote demo/shaders.js")
    return 0


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check(cpp, js):
    problems = 0

    listed = {symbol for _, symbol in SHADERS}
    found = cpp_pieces(cpp)
    for symbol in found:
        if symbol not in listed:
            print(f"FAIL  source/Shaders.cpp defines {symbol}, which this check does not list -- "
                  "add it to SHADERS (and to the page, or say on the page why not)")
            problems += 1
    if not found:
        # A check that silently looks at nothing is worse than no check.
        print("FAIL  no pieces found in source/Shaders.cpp -- the extraction has gone stale")
        problems += 1

    for name, symbol in SHADERS:
        cpp_text, count = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp (or not a run of adjacent raw strings)")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            joined = f", {count} raw strings joined" if count > 1 else ""
            print(f"ok    {name:<14} matches {symbol} ({len(cpp_text)} chars{joined})")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    for path, text in ASSEMBLY:
        try:
            body = read(*path.split("/"))
        except FileNotFoundError:
            print(f"FAIL  {path} is missing")
            problems += 1
            continue
        if text in body:
            print(f"ok    assembly: {path} still says {text.splitlines()[0][:60]}")
        else:
            print(f"FAIL  assembly: {path} no longer says: {text.splitlines()[0]}")
            problems += 1
    return problems


def main(argv):
    cpp = read("source", "Shaders.cpp")
    if "--write" in argv:
        return write(cpp)
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check(cpp, js)
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for a drifted copy "
              "(never edit shaders.js by hand); an assembly or unlisted-piece failure needs a reader")
        return 1
    print(f"all {len(SHADERS)} shader pieces and their assembly are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
