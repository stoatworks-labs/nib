# nib

Flow-guided line drawing — an FFGL effect plugin for Resolume Arena/Avenue
that turns a clip into ink strokes. C++/GLSL, CMake MODULE → universal
`.bundle` (macOS) + Windows `.dll`. Public MIT repo. ID `NB01`.

Read `AGENTS.md` before changing the tensor pass, either convolution, or the
iteration.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render offline: `./build/nibtest --out /tmp/frame.png`
- The test card on its own: `./build/nibtest --card /tmp/card.png`
- List parameters: `./build/nibtest --list`
- Set anything by name: `./build/nibtest --set "Flow=0" --set "Passes=2"`
- Under noise (what Stability and Coherence are for):
  `./build/nibtest --noise 0.12 --frames 20 --out /tmp/n.png`

## Verify
- Everything: `tools/verify.sh`
- **The flow measurement**: `./build/nibtest --flow` — mean angular error
  against an analytic tangent at four Coherence settings. This is the check
  that decides whether the tensor pass is worth its cost; it fails below a
  33% improvement.
- Presets, checked live and distinct: `./build/nibtest --presets /tmp/p.png`
- No dead controls: `python3 tools/sweep.py`
- Render cost: `./build/nibtest --bench`
- Universal + exports: `lipo -archs` and `nm -gU … | grep _plugMain` —
  never trust the build log for either.

## Notes
- **The one idea is that the smoothing follows the drawing.** The DoG runs
  *across* the flow; the LIC runs *along* it. `Flow` at 0 skips the LIC pass
  entirely and leaves plain isotropic XDoG — that A/B is preset 2.
- **Orientations are averaged as a tensor, never as an angle.** `g` and `−g`
  are the same orientation and opposite vectors; averaging vectors destroys
  the agreement the pass exists to find.
- **An eigenvector has no sign.** The LIC walk aligns each step against the
  last one, or it oscillates on the spot and looks like a weak blur.
- **Every intermediate is float.** The DoG is a difference of two nearly
  equal blurs; 8-bit quantises the signal away and Sharpness dies above 0.9.
- All host parameters are 0..1 and mapped in `Controls.cpp`. `SetParamInfo`
  clamps a standard default into 0..1 before `SetParamRange` can widen it.
  Option parameters hold the element value.
- `step`, `flat`, `active`, `filter`, `input`, `output`, `sample`, `common`,
  `layout` are GLSL reserved words. Shader errors surface only at runtime, in
  the diagnostics log.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never
  the build log.
- Public repo. "Commit" = commit **and** push.

## Browser demo
- `demo/` is the page at https://nib-demo.stoatworks-labs.com (Cloudflare Worker
  `nib-demo`, a ROUTE on a proxied AAAA 100:: record -- the zone's custom domains
  are full; deleting the record takes the page dark with a green deploy).
  `deploy.yml` redeploys it on a push to main; by hand: `cf-run npx wrangler deploy`.
- `demo/shaders.js` is GENERATED: after changing `source/Shaders.cpp` run
  `python3 demo/tools/check_shaders.py --write`; verify.sh fails on drift, on a new
  piece the check does not list, and on a change to how the DoG/LIC passes are joined.
- `demo/plugin.js` ports Controls.cpp, Presets.h and ProcessOpenGL's host glue by
  hand, and nothing checks those but a reader: change them with the C++.
- Verify a change to the page: serve `demo/` locally, load it headless, no console
  errors, every program's `missing` set empty (`nibDemo.renderer.p`), move a control
  and see the picture change, and `?embed=1` renders. The cross-check against
  `nibtest --pipe` is in AGENTS.md; nothing reruns it automatically.
- `demo/vendor/` is the shared kit: never edit it.

## Not done yet
- Never loaded into Resolume; no OFX port. The frame pipe
  (`nibtest --pipe`/`--script`) exists since 2026-09-25 and filmed the project
  video. Windows ships from CI but has never run in a host; no Linux build.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It exists for the failure that actually happens: a shader that
will not compile, which otherwise looks like "the plugin does nothing" with
no message anywhere. It records which of the ten passes it was, and the GL
vendor/renderer next to it.

    ~/Library/Logs/nib/nib.YYYY-MM-DD.log
