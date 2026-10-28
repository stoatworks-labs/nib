/**
 * Nib — browser demo.
 *
 * Line drawing as an FFGL effect: the clip turned into ink, continuous closed
 * strokes on paper rather than a glowing outline. The one idea, from
 * `AGENTS.md`: **the smoothing follows the drawing.** A structure tensor says
 * which way the drawing runs at each pixel; a difference of Gaussians is then
 * sampled ACROSS that flow, and a line integral convolution runs ALONG it.
 * Flow at 0 skips the LIC and leaves plain isotropic XDoG — the A/B ships as
 * the "Unguided (XDoG)" preset.
 *
 * **Everything that draws is the plugin's own GLSL.** nib has no CPU half: no
 * readback, no contour following, no tracer. Strokes come out of the GPU
 * chain and nothing else, so there is nothing to port but the host glue.
 * `shaders.js` is the twelve `R"( ... )"` pieces of `source/Shaders.cpp`,
 * spliced by `demo/tools/check_shaders.py --write` and never typed; the same
 * script compares them character for character, checks the DoG and LIC passes
 * are assembled as `DoGShaderSource()` and `LicShaderSource()` assemble them,
 * and `tools/verify.sh` runs it.
 *
 * What IS ported here, by hand, and checked only by a reader:
 *
 *   - `Controls.cpp`: every 0..1-to-physical mapping, in float as the plugin
 *     does it (`Math.fround` at each step), so a tap count lands where the
 *     plugin's does.
 *   - `Presets.h`: the eight instruments, values unedited.
 *   - The host glue of `Nib.cpp`: the constructor's declarations and defaults;
 *     ProcessOpenGL's pass order, buffer formats and samplings, tap counts,
 *     iteration count and the LIC skip at Flow 0; SetFloatParameter's rule
 *     that Detect On, Scale or Passes drops the history.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Preset** is an option parameter in the plugin (element 0 Custom). Here the
 * eight are the kit's preset menu in the panel header, as on tinsel's page. As
 * in `applyPreset`, a preset leaves Detect On and Mix alone: the kit's menu
 * would otherwise put them back to their defaults, so `params.apply` is
 * wrapped to carry them through.
 *
 * **The clock.** The plugin has none: it takes no host time and nothing in
 * it moves on its own. The one thing that runs over frames is the stabilise
 * pass, which blends each frame's ink into the last. Resolume keeps
 * processing an effect on a paused clip, so a still picture settles; the
 * kit draws a paused frame once per change. So while paused, this page keeps
 * calling the chain on the held frame for three seconds after any change, and
 * a fade set by Stability completes as it would in the host.
 *
 * **The About block is not a parameter here**, as on every page in the suite.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import * as S from './shaders.js';

//===========================================================================
// PORT: source/Controls.cpp and the constants in Shaders.h / Controls.h.
// Float arithmetic, as the plugin does it.
//===========================================================================

const f = Math.fround;

/// value^0 = low, value^1 = high, with every octave the same width of travel.
const geometric = (value, low, high) => f(f(low) * f(Math.pow(f(f(high) / f(low)), f(value))));
const linear = (value, low, high) => f(f(low) + f(f(f(high) - f(low)) * f(value)));

const ScaleFromParam = (v) => geometric(v, 0.3, 8.0);
const SharpnessFromParam = (v) => linear(v, 0.5, 1.0);
const ThresholdFromParam = (v) => linear(v, -0.1, 0.1);
const FalloffFromParam = (v) => geometric(v, 0.5, 60.0);
const WeightFromParam = (v) => linear(v, 0.0, 3.0);
const FlowFromParam = (v) => f(v);
const CoherenceFromParam = (v) => geometric(v, 0.5, 12.0);
const LengthFromParam = (v) => geometric(v, 1.0, 32.0);
const AttackFromParam = (v) => linear(v, 1.0, 0.55);
const ReleaseFromParam = (v) => geometric(f(1.0 - f(v)), 0.04, 1.0);

const kSigmaRatio = f(1.6);
const kMaxTaps = 32;
const kMaxIterations = 3;

/// Nib.cpp's tapsFor(): three sigma, clamped to the shaders' loop bound.
const tapsFor = (sigma) => Math.min(kMaxTaps, Math.max(1, Math.ceil(f(sigma * 3.0))));

//===========================================================================
// The parameters, from NibPlugin's constructor: names, groups, order,
// element lists and defaults. Preset and the About block are not on the
// panel (see the header).
//===========================================================================

const SOURCE_NAMES = ['Luma', 'Red', 'Green', 'Blue', 'Saturation'];
const PAPER_NAMES = ['Paper Colour', 'Source', 'Dimmed Source', 'Transparent'];
const PASSES_NAMES = ['1', '2', '3'];

const pct = (v) => `${Math.round(v * 100)}%`;

const PARAMS = [
  //---- Source ---------------------------------------------------------------
  {
    id: 'source', name: 'Detect On', type: 'option', default: 0, group: 'Source',
    elements: SOURCE_NAMES,
    hint: 'Which channel carries the drawing. A red line on a blue field has enormous chroma contrast and almost no luminance contrast, so Saturation is not an exotic setting.',
  },

  //---- Line -----------------------------------------------------------------
  {
    id: 'scale', name: 'Scale', type: 'standard', default: 0.35, group: 'Line',
    display: (v) => `σ ${ScaleFromParam(v).toFixed(2)} px`,
    hint: 'The inner Gaussian, 0.3 to 8 px: how fine a drawing. Small finds every pore, large finds only the shapes. The outer Gaussian is 1.6 times this.',
  },
  {
    id: 'sharpness', name: 'Sharpness', type: 'standard', default: 0.94, group: 'Line',
    display: (v) => `τ ${SharpnessFromParam(v).toFixed(3)}`,
    hint: 'How much of the outer Gaussian is subtracted. At 1 flat regions cancel exactly; below about 0.8 the difference stops being a band-pass and becomes a blurred copy of the picture.',
  },
  {
    id: 'threshold', name: 'Threshold', type: 'standard', default: 0.5, group: 'Line',
    display: (v) => `ε ${ThresholdFromParam(v).toFixed(3)}`,
    hint: 'Where ink starts, −0.1 to 0.1 with the middle exactly on zero. The response is centred on zero in flat regions, so zero is the meaningful middle and not merely the midpoint.',
  },
  {
    id: 'falloff', name: 'Falloff', type: 'standard', default: 0.62, group: 'Line',
    display: (v) => `φ ${FalloffFromParam(v).toFixed(1)}`,
    hint: 'The softness of the ink\'s edge: low is a wash, high a hard pen line. Below about 0.4 the ramp is wider than the response itself and the drawing washes out to a stain.',
  },
  {
    id: 'weight', name: 'Weight', type: 'standard', default: 0.0, group: 'Line',
    display: (v) => `${WeightFromParam(v).toFixed(2)} px`,
    hint: 'A heavier nib: the ink dilated by up to 3 px after the threshold, not blurred, so a weighted line is as solid as a thin one. Compounds with Scale.',
  },

  //---- Flow -----------------------------------------------------------------
  {
    id: 'flow', name: 'Flow', type: 'standard', default: 0.85, group: 'Flow',
    display: (v) => (FlowFromParam(v) > 0.001 ? pct(FlowFromParam(v)) : 'off · XDoG'),
    hint: 'How far the line integral convolution along the flow replaces the raw response. At 0 the pass is skipped and what remains is plain isotropic XDoG: the plugin with its idea taken out.',
  },
  {
    id: 'coherence', name: 'Coherence', type: 'standard', default: 0.45, group: 'Flow',
    display: (v) => `σ ${CoherenceFromParam(v).toFixed(2)} px`,
    hint: 'The structure tensor\'s smoothing, 0.5 to 12 px: how far apart two edges may be before they are allowed to disagree about which way the drawing runs. It earns its keep against noise.',
  },
  {
    id: 'length', name: 'Length', type: 'standard', default: 0.4, group: 'Flow',
    display: (v) => `${LengthFromParam(v).toFixed(1)} px`,
    hint: 'The LIC\'s half-length, 1 to 32 px: how far along a stroke to reach for the rest of it. Does nothing at Flow 0, where the LIC does not run.',
  },
  {
    id: 'passes', name: 'Passes', type: 'option', default: 1, group: 'Flow',
    elements: PASSES_NAMES,
    hint: 'How many times the DoG and LIC run. Each iteration darkens what the last one believed and runs again, which is what closes the gaps in a faint edge.',
  },

  //---- Time -----------------------------------------------------------------
  {
    id: 'stability', name: 'Stability', type: 'standard', default: 0.35, group: 'Time',
    display: (v) => `↑${AttackFromParam(v).toFixed(2)} ↓${ReleaseFromParam(v).toFixed(3)}`,
    hint: 'Two per-frame rates from one control, asymmetric on purpose: ink appears at once (↑) and fades slowly (↓), because a line that arrives late is drawn on the wrong frame.',
  },

  //---- Ink ------------------------------------------------------------------
  // FF_TYPE_RED/GREEN/BLUE on three consecutive parameters, which is what a
  // host needs to show one swatch rather than three sliders.
  { id: 'inkR', name: 'Ink', type: 'colour', default: 0.05, group: 'Ink' },
  { id: 'inkG', name: 'Ink_Green', type: 'colour', default: 0.04, group: 'Ink' },
  { id: 'inkB', name: 'Ink_Blue', type: 'colour', default: 0.06, group: 'Ink' },
  {
    id: 'paper', name: 'Paper', type: 'option', default: 0, group: 'Ink',
    elements: PAPER_NAMES,
    hint: 'What sits behind the ink: the swatch below, the clip itself, the clip held back by Dim, or nothing at all for an alpha layer.',
  },
  { id: 'paperR', name: 'Paper Colour', type: 'colour', default: 0.96, group: 'Ink' },
  { id: 'paperG', name: 'Paper_Green', type: 'colour', default: 0.95, group: 'Ink' },
  { id: 'paperB', name: 'Paper_Blue', type: 'colour', default: 0.92, group: 'Ink' },
  {
    id: 'dim', name: 'Dim', type: 'standard', default: 0.25, group: 'Ink', display: pct,
    hint: 'How far the Dimmed Source paper is held back. Read by that paper only.',
  },

  //---- Output ---------------------------------------------------------------
  {
    id: 'mix', name: 'Mix', type: 'standard', default: 1.0, group: 'Output', display: pct,
    hint: 'The drawing against the clip. At 0 the clip comes back as it went in.',
  },
];

//===========================================================================
// PORT: source/Presets.h. Values unedited, in the plugin's order.
//===========================================================================

/// The parameters a preset sets, in the fixed order `presets::Param` declares.
const PRESET_PARAM_IDS = [
  'scale', 'sharpness', 'threshold', 'falloff', 'weight',
  'flow', 'coherence', 'length', 'passes',
  'stability',
  'inkR', 'inkG', 'inkB',
  'paper', 'paperR', 'paperG', 'paperB', 'dim',
];

const PRESET_TABLE = [
  ['Pen and Ink', [0.35, 0.94, 0.5, 0.62, 0.0, 0.85, 0.45, 0.4, 1, 0.35,
    0.05, 0.04, 0.06, 0, 0.96, 0.95, 0.92, 0.25]],
  ['Unguided (XDoG)', [0.35, 0.94, 0.5, 0.62, 0.0, 0.0, 0.45, 0.4, 0, 0.35,
    0.05, 0.04, 0.06, 0, 0.96, 0.95, 0.92, 0.25]],
  ['Charcoal', [0.55, 0.9, 0.52, 0.45, 0.4, 0.7, 0.6, 0.55, 1, 0.45,
    0.13, 0.11, 0.10, 0, 0.88, 0.85, 0.78, 0.25]],
  ['Technical Pen', [0.28, 0.97, 0.5, 0.8, 0.12, 1.0, 0.5, 0.62, 2, 0.4,
    0.02, 0.02, 0.03, 0, 1.0, 0.99, 0.97, 0.25]],
  ['Alpha Ink', [0.35, 0.94, 0.5, 0.7, 0.1, 0.85, 0.45, 0.4, 1, 0.35,
    1.0, 1.0, 1.0, 3, 0.96, 0.95, 0.92, 0.25]],
  ['Overlay', [0.4, 0.92, 0.5, 0.68, 0.25, 0.9, 0.5, 0.45, 1, 0.4,
    1.0, 0.97, 0.9, 2, 0.96, 0.95, 0.92, 0.35]],
  ['Cyanotype', [0.3, 0.96, 0.5, 0.85, 0.2, 1.0, 0.55, 0.5, 1, 0.4,
    0.93, 0.96, 1.0, 0, 0.06, 0.20, 0.42, 0.25]],
  ['Woodcut', [0.6, 0.99, 0.46, 0.95, 0.32, 0.95, 0.75, 0.7, 1, 0.5,
    0.04, 0.03, 0.03, 0, 0.93, 0.90, 0.82, 0.25]],
];

const PRESETS = Object.fromEntries(
  PRESET_TABLE.map(([name, values]) => [
    name,
    Object.fromEntries(values.map((value, i) => [PRESET_PARAM_IDS[i], value])),
  ]),
);

//===========================================================================
// The chain: Nib.cpp's ProcessOpenGL, pass for pass.
//===========================================================================

const VERSION_LINE = '#version 410 core\n';

class NibRenderer {
  constructor(gl, quad) {
    this.gl = gl;
    this.quad = quad;

    // InitGL's stage table: every pass compiled against kVertexShader, the
    // two flow-guided ones assembled as DoGShaderSource()/LicShaderSource().
    const dogSource = VERSION_LINE + S.FLOW_LIBRARY + S.DOG_MAIN;
    const licSource = VERSION_LINE + S.FLOW_LIBRARY + S.LIC_MAIN;
    this.p = {
      copy: new Program(gl, S.VERTEX, S.COPY, 'copy'),
      tone: new Program(gl, S.VERTEX, S.TONE, 'tone'),
      tensor: new Program(gl, S.VERTEX, S.TENSOR, 'tensor'),
      tensorBlur: new Program(gl, S.VERTEX, S.TENSOR_BLUR, 'tensor blur'),
      dog: new Program(gl, S.VERTEX, dogSource, 'dog'),
      lic: new Program(gl, S.VERTEX, licSource, 'lic'),
      reinject: new Program(gl, S.VERTEX, S.REINJECT, 'reinject'),
      threshold: new Program(gl, S.VERTEX, S.THRESHOLD, 'threshold'),
      stabilise: new Program(gl, S.VERTEX, S.STABILISE, 'stabilise'),
      composite: new Program(gl, S.VERTEX, S.COMPOSITE, 'composite'),
    };

    // ensureBuffers(): every intermediate floating point, and the history
    // read texel-for-texel (nearest), as in the plugin.
    const linearBuffer = () => new PassBuffer(gl, { filter: 'linear' });
    this.copyBuffer = linearBuffer();
    this.tensorBuffer = linearBuffer();
    this.tensorTemp = linearBuffer();
    this.toneBuffer = [linearBuffer(), linearBuffer()];
    this.dogBuffer = linearBuffer();
    this.licBuffer = linearBuffer();
    this.inkBuffer = linearBuffer();
    this.historyBuffer = [
      new PassBuffer(gl, { filter: 'nearest' }),
      new PassBuffer(gl, { filter: 'nearest' }),
    ];

    this.toneCurrent = 0;
    this.historyCurrent = 0;
    this.historyValid = false;
    this.bufferWidth = 0;
    this.bufferHeight = 0;

    // SetFloatParameter drops the history when Detect On, Scale or Passes is
    // set; applyPreset does the same for the ones it covers. Compared here per
    // frame, which catches a slider, a preset and a pasted link alike.
    this.lastEdge = null;

    this.stats = null;
  }

  ensureBuffers(width, height) {
    const gl = this.gl;
    this.copyBuffer.ensure(width, height, gl.RGBA16F);
    this.tensorBuffer.ensure(width, height, gl.RGBA16F);
    this.tensorTemp.ensure(width, height, gl.RGBA16F);
    this.toneBuffer[0].ensure(width, height, gl.R16F);
    this.toneBuffer[1].ensure(width, height, gl.R16F);
    this.dogBuffer.ensure(width, height, gl.R16F);
    this.licBuffer.ensure(width, height, gl.R16F);
    this.inkBuffer.ensure(width, height, gl.R16F);
    this.historyBuffer[0].ensure(width, height, gl.R16F);
    this.historyBuffer[1].ensure(width, height, gl.R16F);

    if (width !== this.bufferWidth || height !== this.bufferHeight) {
      this.bufferWidth = width;
      this.bufferHeight = height;
      this.historyValid = false;
    }
  }

  render({ input, params, width: outWidth, height: outHeight }) {
    const gl = this.gl;
    const quad = this.quad;
    const P = this.p;
    const v = (id) => f(params.get(id));

    const edge = `${params.get('source')}|${params.get('scale')}|${params.get('passes')}`;
    if (edge !== this.lastEdge) {
      this.lastEdge = edge;
      this.historyValid = false;
    }

    const width = input.width;
    const height = input.height;
    if (width <= 0 || height <= 0) return;

    this.ensureBuffers(width, height);
    gl.disable(gl.BLEND);

    const texelX = f(1 / width);
    const texelY = f(1 / height);

    // 1. Copy. The kit's input texture is exactly the picture, so MaxUV is 1.
    this.copyBuffer.bind();
    P.copy.use();
    bindTexture(gl, 0, input.texture);
    P.copy.setSampler('InputTexture', 0);
    P.copy.set('MaxUV', 1, 1);
    P.copy.set('HalfTexel', 0.5 / width, 0.5 / height);
    quad.draw();

    // 2. Tone.
    this.toneBuffer[0].bind();
    P.tone.use();
    bindTexture(gl, 0, this.copyBuffer.texture);
    P.tone.setSampler('CopyTexture', 0);
    P.tone.set('SourceMode', v('source'));
    quad.draw();
    this.toneCurrent = 0;

    // 3. The structure tensor.
    this.tensorBuffer.bind();
    P.tensor.use();
    bindTexture(gl, 0, this.toneBuffer[0].texture);
    P.tensor.setSampler('ToneTexture', 0);
    P.tensor.set('TexelSize', texelX, texelY);
    quad.draw();

    // 4. Smooth it, separably. Horizontal into the temp, vertical back.
    const coherence = CoherenceFromParam(v('coherence'));
    const coherenceTaps = tapsFor(coherence);
    for (const axis of [
      { target: this.tensorTemp, source: this.tensorBuffer, dx: texelX, dy: 0 },
      { target: this.tensorBuffer, source: this.tensorTemp, dx: 0, dy: texelY },
    ]) {
      axis.target.bind();
      P.tensorBlur.use();
      bindTexture(gl, 0, axis.source.texture);
      P.tensorBlur.setSampler('TensorTexture', 0);
      P.tensorBlur.set('Direction', axis.dx, axis.dy);
      P.tensorBlur.set('Sigma', coherence);
      P.tensorBlur.setInt('Taps', coherenceTaps);
      quad.draw();
    }

    // 5-7. The iterated pair: DoG across the flow, LIC along it, then reinject
    //      into the tone for the next round.
    const scale = ScaleFromParam(v('scale'));
    const tau = SharpnessFromParam(v('sharpness'));
    const threshold = ThresholdFromParam(v('threshold'));
    const falloff = FalloffFromParam(v('falloff'));
    const flow = FlowFromParam(v('flow'));
    const length = LengthFromParam(v('length'));

    const dogTaps = tapsFor(f(scale * kSigmaRatio));
    const licTaps = tapsFor(length);
    const licRuns = flow > 0.001;

    const iterations = Math.min(kMaxIterations, Math.max(1, Math.round(v('passes')) + 1));

    for (let iteration = 0; iteration < iterations; iteration += 1) {
      // DoG, reading whichever tone buffer the last reinjection wrote.
      this.dogBuffer.bind();
      P.dog.use();
      bindTexture(gl, 0, this.toneBuffer[this.toneCurrent].texture);
      bindTexture(gl, 1, this.tensorBuffer.texture);
      P.dog.setSampler('ToneTexture', 0);
      P.dog.setSampler('FlowTexture', 1);
      P.dog.set('TexelSize', texelX, texelY);
      P.dog.set('Sigma', scale);
      P.dog.set('Tau', tau);
      P.dog.setInt('Taps', dogTaps);
      quad.draw();

      // LIC. At Flow 0 the pass is skipped entirely, not run at zero weight:
      // that is what makes the control the honest A/B against plain XDoG.
      if (licRuns) {
        this.licBuffer.bind();
        P.lic.use();
        bindTexture(gl, 0, this.dogBuffer.texture);
        bindTexture(gl, 1, this.tensorBuffer.texture);
        P.lic.setSampler('DoGTexture', 0);
        P.lic.setSampler('FlowTexture', 1);
        P.lic.set('TexelSize', texelX, texelY);
        P.lic.set('Sigma', length);
        P.lic.set('Strength', flow);
        P.lic.setInt('Taps', licTaps);
        quad.draw();
      }

      if (iteration === iterations - 1) break;

      // Reinject into the other tone buffer.
      const next = 1 - this.toneCurrent;
      this.toneBuffer[next].bind();
      P.reinject.use();
      bindTexture(gl, 0, this.toneBuffer[this.toneCurrent].texture);
      bindTexture(gl, 1, (licRuns ? this.licBuffer : this.dogBuffer).texture);
      P.reinject.setSampler('ToneTexture', 0);
      P.reinject.setSampler('LicTexture', 1);
      P.reinject.set('Threshold', threshold);
      P.reinject.set('Falloff', falloff);
      quad.draw();
      this.toneCurrent = next;
    }

    // 8. Threshold, and the weight.
    const weight = WeightFromParam(v('weight'));
    this.inkBuffer.bind();
    P.threshold.use();
    bindTexture(gl, 0, (licRuns ? this.licBuffer : this.dogBuffer).texture);
    P.threshold.setSampler('LicTexture', 0);
    P.threshold.set('TexelSize', texelX, texelY);
    P.threshold.set('Threshold', threshold);
    P.threshold.set('Falloff', falloff);
    P.threshold.set('Weight', weight);
    P.threshold.setInt('WeightTaps', weight > 0.01 ? 1 : 0);
    quad.draw();

    // 9. Stabilise, ping-ponged against the previous frame.
    const history = this.historyCurrent;
    const historyWrite = 1 - this.historyCurrent;
    const reset = !this.historyValid;
    this.historyBuffer[historyWrite].bind();
    P.stabilise.use();
    bindTexture(gl, 0, this.inkBuffer.texture);
    bindTexture(gl, 1, this.historyBuffer[history].texture);
    P.stabilise.setSampler('InkTexture', 0);
    P.stabilise.setSampler('HistoryTexture', 1);
    P.stabilise.set('Attack', AttackFromParam(v('stability')));
    P.stabilise.set('Release', ReleaseFromParam(v('stability')));
    P.stabilise.set('Reset', reset ? 1 : 0);
    quad.draw();
    this.historyCurrent = historyWrite;
    this.historyValid = true;

    // 10. Composite, back into the host's framebuffer and viewport.
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, outWidth, outHeight);
    P.composite.use();
    bindTexture(gl, 0, this.historyBuffer[this.historyCurrent].texture);
    bindTexture(gl, 1, this.copyBuffer.texture);
    P.composite.setSampler('InkTexture', 0);
    P.composite.setSampler('CopyTexture', 1);
    P.composite.set('Ink', v('inkR'), v('inkG'), v('inkB'));
    P.composite.set('PaperColour', v('paperR'), v('paperG'), v('paperB'));
    P.composite.set('PaperMode', v('paper'));
    P.composite.set('Dim', v('dim'));
    P.composite.set('MixAmount', v('mix'));
    quad.draw();

    bindTexture(gl, 1, null);
    bindTexture(gl, 0, null);

    // What the chain did this frame, for the line under the picture. It
    // reports, it does not measure.
    const passes = 1 + 1 + 1 + 2 + iterations * (licRuns ? 2 : 1) + (iterations - 1) + 1 + 1 + 1;
    this.stats = {
      width, height, iterations, licRuns, passes,
      dogTaps, licTaps, coherenceTaps,
      weightOn: weight > 0.01,
      reset,
    };
  }
}

//===========================================================================
// The page.
//===========================================================================

let renderer = null;

const mounted = mountDemo({
  name: 'Nib',
  pluginId: 'NB01',
  kind: 'effect',
  tagline:
    'Line drawing as an FFGL effect. It turns a clip into ink: continuous, closed strokes on paper, rather than the glowing outline most edge effects give you. It asks which way the drawing runs first, then steers two filters by that answer in opposite directions: a difference of Gaussians across the flow, where a band-pass is actually measuring the edge, and a line integral convolution along it, which is what joins a per-pixel response into a stroke. Turn Flow to zero and the second filter is skipped, leaving plain XDoG; that comparison is the Unguided (XDoG) preset.',
  repo: 'https://github.com/stoatworks-labs/nib',
  page: 'https://stoatworks-labs.com/software/nib/',
  video: 'https://www.youtube.com/watch?v=T0nKJW0gyGo',

  blurb:
    'It is Nib’s own ten GLSL passes — tone, structure tensor, the DoG across the flow, the LIC along it, the threshold and the temporal filter — ported from the repository to WebGL2 unedited and run in the plugin’s order on half-float buffers, on generated clips in this page or your own. The plugin has no CPU half, so nothing that draws is a port; only its control mappings and presets are, ported from its C++.',

  // Float intermediates are load-bearing (AGENTS.md: an 8-bit chain loses
  // Sharpness above 0.9), so the page refuses to run without them.
  needFloat: true,
  // Paper: Transparent, and the Alpha Ink preset, carry real alpha.
  showBackdrop: true,
  presets: PRESETS,

  // The geometry card first: it is the nearest thing here to the harness's
  // test card (rings and diagonals, and a grid closely spaced enough to show
  // Scale and Weight compounding), and it draws at the defaults. The shape on
  // transparency is one clean closed contour. The synthetic scene is dark and
  // low in contrast, so at the defaults it draws little more than its
  // horizon -- the plugin does the same with it -- and wants Threshold up.
  sources: ['grid', 'alpha', 'detail', 'scene', 'spot', 'bars', 'ramp'],

  params: PARAMS,

  differences: [
    'Nothing that draws is a port. nib has no CPU half: no readback, no contour following, no tracer; every stroke comes out of the ten GPU passes, and those are the plugin’s GLSL, copied character for character (demo/tools/check_shaders.py fails the repository’s verify script if a character drifts, or if the DoG and LIC passes stop being assembled the way the plugin assembles them). What is ported is the host glue around them: the control mappings (Controls.cpp, in float as the plugin does them), the tap counts, the pass order, the iteration count and the LIC skip at Flow 0 (Nib.cpp’s ProcessOpenGL), and the presets (Presets.h). Only a reader checks those.',
    'Every intermediate is half float, as in the plugin: the difference of Gaussians is a difference of two nearly equal blurs, and 8 bits would lose it (Sharpness would stop working above about 0.9). WebGL2 needs EXT_color_buffer_float to render into half-float targets; without it the page stops and says so rather than drawing a plausible wrong picture.',
    'Preset is an option parameter in the plugin, with Custom as element 0 and a slider edit dropping back to it. Here the same eight instruments are the menu in the panel header, from the plugin’s own table, and as in the plugin a preset leaves Detect On and Mix where they were.',
    'Frames from this page were compared once, on 2026-10-28, with the plugin’s own harness (nibtest --pipe, which drives the real plugin class) fed the same input frames: 26 settings on an M4 Max, covering the defaults, every Line and Flow control moved, each paper, Saturation, seven presets and a paused Stability fade. In 23 of them every pixel came out within one level of 255 of the plugin’s; in the other three, 1, 1 and 4 pixels were further off, at most 12 levels, all on a hard ink edge where a rounding difference decides ink or paper. Nothing keeps that comparison running; a different GPU or browser will round differently.',
    'The kit’s clips are clean and synthetic, and Flow earns its keep against noise and texture, so it changes less here than on footage: taking it to 0 on the geometry card moves about one pixel in twenty. The fine-detail clip’s noise band shows it best (at a small Scale and a raised Threshold: scattered specks at 0, marks drawn out along the flow at 1), and your own footage shows it as the plugin’s video does.',
    'The plugin has no clock and no audio input, so nothing is missing for want of either. The one thing that runs over time is Stability, which blends each frame’s ink into the last, so it depends on the frame rate here as it does in Resolume. Resolume keeps processing an effect on a paused clip; this page, paused, keeps running the chain on the held frame for three seconds after any change, so a fade completes.',
    'The clips are the kit’s generated ones (or your own file), 8-bit and premultiplied as Resolume hands an effect its input. The harness’s test card (rings with a known tangent, a hatched panel, a disc, two equal-luminance fields) and Resolume’s bundled clips that the project video was rendered from are not here.',
    'Nothing here is measured. The plugin’s harness measures the flow field against concentric rings whose tangent is known in closed form (13.8° of error at the least-smoothed Coherence, 1.1° at the best, and the build fails below a 33% improvement), checks every preset is live and distinct, and fails if any control does nothing — that harness, not this page, is the reason to believe the maths. The plugin itself has never been loaded into Resolume.',
  ],

  createRenderer: (gl, quad) => {
    renderer = new NibRenderer(gl, quad);
    return renderer;
  },
});

//---------------------------------------------------------------------------
// After mounting: presets that leave Detect On and Mix alone, the paused
// settle, and the line under the picture.
//---------------------------------------------------------------------------
if (mounted) {
  const { params, state } = mounted;
  const query = new URLSearchParams(window.location.search);
  const embed = query.has('embed') && query.get('embed') !== '0';

  // applyPreset covers neither Detect On nor Mix (Presets.h says why); the
  // kit's apply() resets whatever a preset does not name, so carry them.
  const apply = params.apply.bind(params);
  params.apply = (values) => apply({ source: params.get('source'), mix: params.get('mix'), ...values });

  // Paused: keep calling the chain on the held frame for three seconds after
  // any change, as a host keeps processing an effect on a paused clip.
  let settleUntil = 0;
  let settling = false;
  const settle = () => {
    if (state.playing || performance.now() > settleUntil) {
      settling = false;
      return;
    }
    mounted.redraw();
    requestAnimationFrame(settle);
  };
  const kick = () => {
    settleUntil = performance.now() + 3000;
    if (!settling) {
      settling = true;
      requestAnimationFrame(settle);
    }
  };
  params.addEventListener('change', kick);
  document.querySelector('#demo')?.addEventListener('click', kick);
  document.querySelector('#demo')?.addEventListener('change', kick);

  if (!embed) {
    const stage = document.querySelector('.stage');
    if (stage) {
      const line = document.createElement('p');
      line.className = 'stage__status';
      line.id = 'nib-stats';
      line.setAttribute('aria-live', 'off');
      stage.append(line);
      setInterval(() => {
        const s = renderer?.stats;
        if (!s) return;
        line.textContent =
          `${s.passes} draws at ${s.width} × ${s.height}`
          + ` · ${s.iterations} iteration${s.iterations === 1 ? '' : 's'} of DoG (${s.dogTaps} taps a side)`
          + (s.licRuns ? ` and LIC (${s.licTaps} steps each way)` : ', LIC skipped: plain XDoG')
          + ` · tensor blur ${s.coherenceTaps} taps`
          + (s.weightOn ? ' · weight dilation on' : '');
      }, 250);
    }
  }

  // For headless checks: the page's pieces, read-only by convention.
  window.nibDemo = { mounted, get renderer() { return renderer; }, PARAMS, PRESETS };
}
