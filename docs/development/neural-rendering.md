# DLSS Neural Rendering (NGX Feature 18)

Community Shaders drives NVIDIA's DLSS Neural Rendering model (`nvngx_dlssnr.dll`,
NGX feature 18) directly, as part of the Upscaling feature. NVIDIA ships no public
integration for it; the runtime DLL is proprietary and user-supplied, and the
parameter contract (`DLSSNR.*` string keys) is reverse-engineered, not documented.

Source layout:

| Path | Role |
|---|---|
| `src/Features/Upscaling/NeuralRendering.{h,cpp}` | public `NeuralRendering` class; thin adapter to the backend |
| `src/Features/Upscaling/NeuralRendering/Backend.{h,cpp}` | `NeuralRenderingBackend` - owns the D3D11↔D3D12 shared textures, the colour/guide transfer passes, the failure latch |
| `src/Features/Upscaling/NeuralRendering/Runtime.{h,cpp}` | `NeuralRendering::Runtime` - loads the snippet DLL, drives NGX feature 18 create/evaluate/release, the `GetModuleFileNameW` caller-gate hook |
| `src/Features/Upscaling/NeuralRendering/D3D12Interop.{h,cpp}` | private D3D12 device/queue, shared-fence ping-pong with the game's D3D11 context |
| `features/Upscaling/Shaders/Upscaling/NeuralRendering/` | `EncodeColorCS` / `DecodeColorCS` / `CopyDepthGuideCS` and `ColorTransfer.hlsli` |

## Placement

The `Placement` setting chooses where in the frame the model runs.

### Before Upscaling (`neuralRenderingPlacement == 0`)

Runs inside `Upscaling::Upscale()`, on the render-resolution colour that is about
to be handed to DLSS. The model's output replaces the DLSS input colour; DLSS then
upscales the enhanced render-resolution frame to display resolution as normal.
Colour and the depth/motion guides are all at render resolution.

The render-resolution colour is the game's *jittered* raster: every frame is
projected with the sub-pixel Halton offset DLSS later removes. Feature 18 has no
jitter parameter (neither does OptiScaler's pre-SR path), so the backend
compensates for it itself; see *Jitter* below. `Upscaling::Upscale()` passes the
same offset Streamline receives (`-jitter`) through `NeuralRendering::Options`.

### After Upscaling (`neuralRenderingPlacement == 1`, default)

Runs in `Upscaling::PerformUpscaling()` after the colour upscale and before
`UpscaleDepth()`, on the display-resolution upscaled frame, before RCAS
sharpening / copy-back. Depth and motion therefore remain the exact
render-resolution guides used for the colour upscale. The backend tracks the
colour region and guide region separately (`FrameInputs::guideWidth/guideHeight`).

### Separate Upscaling (`neuralRenderingPlacement == 2`, experimental)

This follows the deferred residual experiment in
[`wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/blob/main/docs/DEFERRED-NR-DLSS.md).
It runs Feature 18 at render resolution without changing the colour passed to the
game's normal DLSS feature. The backend forms the signed scene-linear difference
between the matched-residual NR result and the untouched render raster, compresses
it around neutral grey as `0.5 + 0.5*d/(1+abs(d))`, and passes that carrier through
a second private DLSS Super Resolution feature with its own parameter block and
temporal history. After the main DLSS evaluation, the full-resolution carrier is
decoded and added to the clean main-SR result; alpha remains renderer-owned.

Private DLSS creation is submitted one frame before its first evaluation. Until it
is ready, and on any allocation/runtime/evaluation mismatch, the clean main-SR
frame is retained. Resolution, placement, quality/preset, and loading-transition
changes reset the private history. This mode requires the NVIDIA DLSS-SR runtime,
adds another DLSS evaluation plus render/display-resolution FP16 carrier storage,
and deliberately feeds DLSS biased residual data rather than natural imagery.
The private D3D12 device is initialized through the resident NGX core with the
same project identity as Streamline and the Streamline directory in its feature
path; initializing only through the Feature 18 snippet cannot load DLSS-SR.

### Finished Image (`neuralRenderingPlacement == 3`, experimental)

Runs from `PostProcessingExtensions::Main_HDRTonemapBlendCinematic_Render` (`src/Hooks.cpp`),
the single hook point that sees every path the frame's tonemap can take: right after
`State::HandlePostProcessing()` when Effects11 replaces the pass outright and owns the tonemap
itself, and right after the vanilla tonemap/passthrough call otherwise - which covers Post
Processing owning the tonemap too, since in that case the vanilla call just takes its
passthrough branch over Post Processing's already-finished result (see
`PostProcessing::PreProcess()`'s own comment). Either way, by the time
`Upscaling::ApplyNeuralRenderingFinishedImage()` runs, the game render target it's given
(`output` - `kFRAMEBUFFER`, or HDR Display's float16 texture while it redirects that slot
around ISHDR; never `kMAIN`) holds a genuinely finished, display-referred frame -
not the linear HDR scene colour the Before/After Upscaling placements have to approximate with
a display-matched proxy (see *Colour domain*). Unlike every other placement, Finished Image does not
depend on any one feature owning the tonemap.

By this point the colour is display resolution and resolved onto the unjittered grid, but the
guides are **not**: `UpscaleDepth()` expands only `kMAIN` depth (plus refraction normals, SAO
camera Z and the underwater mask) to display resolution, while motion vectors
(`RE::RENDER_TARGETS::kMOTION_VECTOR`) and the material-category snapshot stay at render
resolution in the top-left of their allocations, carrying the frame's TAA jitter. An earlier
version treated all three as native and unjittered, which misscaled the model's motion vectors
and every guide lookup below native and swung them with the jitter phase - flicker plus edits
landing offset from the silhouettes they belonged to. So
`Upscaling::CaptureNeuralRenderingFinishedImageGuides()` runs in `PerformUpscaling()` just before
`UpscaleDepth()`: it copies `kMAIN` depth into a private snapshot while it is still on the
motion-vector raster and records that render-resolution extent (before
`dynamicResolutionLock = 1`). `Upscaling::EvaluateNeuralRenderingFinishedImage()` then passes the
snapshot as depth and sets the guide extent and guide jitter exactly as the After Upscaling
placement does. The captured guides are consumed on use, so the model runs at most once per
upscaled frame: a second tonemap-pass call with a different colour target would otherwise reset
its temporal history every frame. It reads the colour's active resolution from `globals::game::graphicsState->screenWidth/Height`
(the same authoritative source every other Neural Rendering call site uses), not from
`GetDesc()` on any of the individual resources - those can legitimately be a larger,
differently-padded allocation than the frame's active region, so comparing them against each
other for a "do these agree" fail-closed check is a false alarm waiting to happen (an earlier
version of this placement did exactly that and silently no-op'd every frame as a result).

The output resource is **not** the `neuralRenderingTexture` the After Upscaling and Separate
Upscaling placements use: that one matches `kMAIN`, whose format differs from `kFRAMEBUFFER`
(UNORM in SDR) and from HDR Display's float16 redirect, and `CopyResource` between mismatched
formats is silently dropped by D3D11 - a second earlier version of this placement ran the model
every frame and discarded the result exactly that way. Instead
`Upscaling::EnsureNeuralRenderingFinishedImageTexture()` lazily allocates
`neuralRenderingFinishedImageTexture` to match `output`'s own width, height, format and sample
count (recreated when any change), and `ApplyNeuralRenderingFinishedImage()` copies it back into
`output` in place on success. When `kFRAMEBUFFER`'s slot has a null texture pointer (it can alias
the swap-chain backbuffer through its views alone), the texture is recovered from its SRV. A
target that cannot take a typed UAV write - sRGB, typeless or multisampled - fails closed with a
one-time warning naming the format.

**Fails closed.** `EvaluateNeuralRenderingFinishedImage()` returns false - leaving the caller's
buffer untouched - unless Finished Image is the active placement, the backend is available, and
the depth/motion guides exist. `ApplyNeuralRenderingFinishedImage()` additionally skips while a
main menu or loading screen is open, matching every pipeline pass's own
`DisableInMainLoadingMenu()`-style guard.

#### Why not literally before blur

This placement is inspired by the "Present Enhanced" idea from other DLSS Neural Rendering
injector projects: run last, on the final presented image, carrying a matched guide bundle
tagged to that exact frame, and fail closed if it cannot prove the guides correspond. Those
projects are generic injectors sitting at `Present`, where a game's blur/DOF effects have
already been baked into the frame by the time they see it - so "last, before Present" is also
"after any blur."

Community Shaders is not a generic injector. Its own Post Processing pipeline
(`PostProcessing::FeaturePipelineIndex`) deliberately runs Depth of Field and Motion Blur
*before* Composite/Colour Grading/LUT, in linear HDR space, for physically correct blur -
tonemapping and grading are the pipeline's last stages, not its first. There is consequently no
point in this pipeline that is "after grading, before blur": blur is the earliest thing that
happens, not the latest. Finished Image therefore runs at the latest point Community Shaders'
hook architecture actually exposes - right after the frame's tonemap, whoever performed it -
which satisfies "after most everything" without literally preceding a blur stage that, here,
already ran first. A user
relying on vanilla's own late, post-tonemap Depth of Field/Motion Blur (Community Shaders'
replacements disabled) is not covered by this guarantee; extending it would need a new hook
inside vanilla's own image-space effect chain, which does not exist today.

## Motion vectors

The Before and After placements pass the game's **raw** motion-vector target
(`RE::RENDER_TARGETS::kMOTION_VECTOR`) to `NeuralRendering::Evaluate`, not the
`motionVectorCopyTexture` that `EncodeTexturesCS` produces for DLSS.

Separate Upscaling also gives raw vectors to Feature 18, but its private DLSS-SR
history receives `motionVectorCopyTexture` with scale `(1, 1)`, exactly matching
the main DLSS guide contract. This keeps the NR model's and DLSS reconstructor's
different motion expectations isolated.

That copy is a 5x5 *dilated* field: each pixel adopts a closer, faster-moving
neighbour's vector, faded back toward its own vector for anything nearer than
~10,000 game units, so only far scenery is fully substituted. Streamline is told
the field is pre-dilated (`Streamline.cpp`), so DLSS skips its own dilation. The
dilation is deliberate: a two-texel rim of background around each moving
silhouette is tagged with the foreground's motion, DLSS's history-rejection then
kills the would-be ghost trail behind the object.

Neural Rendering's motion input feeds *the model's own* temporal state, not
DLSS's, and NVIDIA's DLSS-NR integration guidance (and the driver-level path, and
OptiScaler) supplies plain per-pixel vectors with the model dilating internally.
Fed the dilated rim, the model either re-decides those pixels every frame (a
flickering outline that tracks moving edges) or trusts it and smears foreground
detail into the background. An undilated field is the model's training
distribution; the dilated one is not.

The copy is allocated with the game target's exact format
(`Upscaling.cpp`, `motionVectorCopyTexture` creation), so the backend's shared
texture, the subresource copy, and the motion-vector scale / subrects are
identical whichever resource is passed. DLSS keeps its dilated copy unchanged.

## Why there is no separate "Inside Upscaling" mode

OptiScaler's DLSS-NR fork offers *before / inside / after* the upscaler. "Inside"
there is its `IFeature_Dx11wDx12` / `DualFeature` path: the model spliced into
OptiScaler's **own replacement upscaler** at render resolution, with `DualEnlarger`
choosing which upscaler performs the final enlarge. It is possible only because
OptiScaler replaces the game's upscaler and controls its internal pipeline.

Community Shaders calls `slEvaluateFeature(kFeatureDLSS)` (and FFX `ffxDispatch`)
as black boxes - there is no hook point *inside* the upscale. The frame positions
actually available to CS are:

1. before the reactive / transparency / motion-vector encode pass,
2. after that pass, before `slEvaluateFeature`,
3. after `slEvaluateFeature`, before RCAS / copy-back.

(1) and (2) are indistinguishable for Neural Rendering because the encode pass
reads the game's render targets, not the colour buffer NR would have altered. (2)
is "Before Upscaling"; (3) is "After Upscaling". There is no fourth position, and
CS's NR already runs through a D3D11↔D3D12 bridge - structurally the same place as
OptiScaler's "inside the bridge" call site - so "Before Upscaling" *is* the
inside-the-bridge position in OptiScaler's taxonomy.

## Colour domain

Evaluation carries a `NeuralRendering::ColorDomain` (`TransferParams.ColorDomain`, the slot
that used to be `CategoryPadding`, so the constant-buffer layout is unchanged). Before, After and
Separate Upscaling pass `kSceneLinear` and get exactly the behaviour described below. Finished
Image runs after the tonemap, where `kFRAMEBUFFER` holds a gamma-2.2 display-referred frame, so
it passes `kDisplayGamma` instead: the encode decodes with the 2.2 curve HDR Display uses
(`Color::GammaToLinearSafe`), scales down only pixels whose linear peak exceeds one (a single
hue-preserving factor, so an SDR frame reaches the model unchanged), and re-encodes with the same
curve; the resolve decodes original, proxy and model with that curve, applies the edit in linear
light and re-encodes. Treating the finished frame as scene-linear - what an earlier version did -
Reinhard-compressed and re-encoded an already-encoded image, handing the model a washed-out,
over-bright proxy. The one exception is HDR Display's float16 redirect when the scene reaching it
is linear (Linear Lighting, or Post Processing owning the tonemap) - the same test `HDROutputCS`
applies - which keeps `kSceneLinear`.

`ColorTransfer.hlsli` maps the linear open-ended HDR scene colour into the
display-referred (tone-mapped + sRGB) domain the model was trained on - and it
does so through the display transform the frame is actually about to receive,
so the model sees the frame the way the user will (see *Display-matched proxy*
below). The model
answer is **not** inverse-Reinhard decoded: that inverse has an unbounded slope
near white and turned tiny output changes into severe HDR flicker. Instead the
resolve compares model and proxy luminance, adds a shared `1/512` shadow floor,
and clamps the ratio to `0.5..2.0`. Chroma is carried over the same way, as the
model's change *relative to the proxy it saw*: both are expressed as
luma-normalised colour, the per-channel ratio between them (guarded to
`0.25..4`) is applied to the original's normalised colour, and the result is
rescaled onto the guarded scene luminance. With a hue-preserving scalar proxy
this is exactly the model's own palette, and a model no-op reproduces the
original whatever the proxy's colour was.

That transferred chroma is then **hue-guarded** against the original
(`kNeuralHueGuardStart..End` in `ColorTransfer.hlsli`, on the luma-weighted
magnitude of the original's chroma offset from neutral). Where the original is
near neutral the model may only move chroma along the original's own hue axis -
more or less saturated, never rotated and never past neutral - so a small,
consistent bias in the model's palette cannot tint whole shaded surfaces: a grey
stays grey. The lock releases smoothly as the original carries more chroma of
its own (a bluish shadow or pale skin sits around 0.1 in this metric, saturated
foliage around 0.3), so intentional recolouring of clearly coloured materials
survives. This is what stops the model's green cast on neutral shading, which was
most visible in the low-contrast Finished Image placement.

`Color Strength` blends from stable renderer chroma at zero to that guarded
model chroma at one, fading only in near-black pixels where normalized colour is
numerically ambiguous. HDR headroom and alpha remain renderer-owned. No temporal
accumulator or midpoint blend is involved; every frame is independently
re-anchored.

### Display-matched proxy

An earlier version built the scene-linear proxy as a plain hue-preserving
Reinhard of the raw linear scene: no exposure, no grading. That is not the frame
the user sees. The game's eye adaptation alone moves the frame by several stops
between an interior and a sunlit exterior, and ISHDR then applies a white point,
saturation, tint, brightness and a contrast curve on top. Shown a frame that was
far darker or flatter than the displayed one, the model pushed local tone and
contrast hard to "fix" it, and the game's adaptation and contrast then amplified
that edit again on the way to the screen - neural shading stacked on top of game
shading. Finished Image did not suffer from this because it runs on the finished
frame, which is why the Before/After/Separate Upscaling placements looked
noticeably more contrasty than it.

The pre-tonemap placements therefore pass a `NeuralRendering::DisplayTransform`
(`Options::display`) that `EncodeColorCS` applies to the scene-linear colour
before the sRGB encode (`ApplyNeuralDisplayTransform`, `ColorTransfer.hlsli`):

- **Exposure.** The game's adaptation, `AvgTex.y / AvgTex.x` exactly as ISHDR's
  BLEND pass applies it, multiplied by Post Processing's Histogram Auto Exposure
  (`0.18 * 2^compensation / clamp(adapted, range)`, the Composite pass's
  formula) when that feature is active. Both are GPU values, so the encode and
  decode bind the adaptation texture / buffer themselves (`t1`/`t2` and
  `t5`/`t6`) and resolve the transform per pixel from the `TransferParams`
  constants plus those two reads.
- **Vanilla grading.** When the vanilla tonemap owns the frame
  (`State::GetTonemapOwner() == kVanilla`) the proxy replicates ISHDR.hlsl's SDR
  path stage for stage: the luminance-driven Reinhard with white point (or the
  Hejl-Burgess-Dawson curve when `Param.z` selects it), saturation / tint /
  brightness, and the shadow-aware contrast around the adapted luminance. Bloom,
  the fade overlay and the HDR display mapping are omitted.

Only the model's *view* changes. The edit is still a luminance ratio and a
relative chroma change measured against the proxy and applied to the untouched
linear colour; the exposure cancels out of that ratio, and the relative chroma
transfer (above) is what keeps the proxy's baked-in saturation and tint from
being read back as a model edit and applied a second time.

**Where the inputs come from.** `Upscaling::CaptureNeuralRenderingDisplayTransform()`
runs from the `Main_HDRTonemapBlendCinematic_Render` hook right after the vanilla
pass: it reads `Param`, `Cinematic` and `Tint` from the pass's
`ImageSpaceShaderParam::pixelConstantGroup` (float4 slots c2-c4 of ISHDR's
`PerGeometry`, i.e. floats 8, 12 and 16) and takes the adaptation texture from
pixel-shader slot 2, which the pass leaves bound; a target larger than 64 texels
on a side is rejected as not being the adaptation. Values outside what an
imagespace can express (or a non-vanilla tonemap owner, or Effects11 replacing
the pass) invalidate the capture and the proxy falls back to the previous
exposure-less Reinhard. The capture is consumed by the *next* frame's
pre-tonemap placements, one frame of latency on values that are temporally
smoothed anyway. The first successful capture is logged once
(`[Upscaling] Neural Rendering display transform captured: ...`); compare its
saturation / contrast / brightness / white against Post Processing's
*Debug -> Game ImageSpace Values* panel to confirm the constant layout on a new
game build.

**Known approximations.** Under the Post Processing tonemap owner only the
exposure is replicated; its tonemapper and grading are not, so the proxy is an
exposed classic Reinhard. Under Effects11 nothing is captured and the proxy is
the plain Reinhard. The luminance edit is applied in scene-linear light and then
passes through the real tonemap and contrast, so its final magnitude is still
reshaped by them (a contrast of 1.2 makes a mid-tone edit ~20% stronger in log
space); `Transfer Strength` remains the knob for that residual.

## Jitter

The model re-decides its local tone and structure whenever the framing changes
(OptiScaler measured the same: a reprojected temporal accumulator on the edit
was a dead end twice, because "an old answer does not belong to a new frame").
Before the upscaler the framing changes every frame by up to a pixel, which
showed up as shadows and detail drifting around; after the upscaler the frame is
already unjittered and was stable.

`EncodeColorCS` therefore resamples the scene colour onto the **unjittered**
pixel grid (Catmull-Rom, taps clamped to the active region, result clamped to the
2x2 neighbourhood so HDR speculars cannot ring) before encoding it, so the model
sees a stable framing. `DecodeColorCS` samples the model's answer *and the exact
proxy it was given* (an SRV over the shared input texture, not a re-encode) back
at each original pixel's jittered position and forms the luminance ratio from
that pair. Only the edit is ever resampled: DLSS still receives the original
jittered sample scaled by it, so its input stays sharp and correctly jittered.
Both passes share one `TransferParams` constant buffer (`JitterOffset`,
`ColorStrength`) and a linear clamp sampler. With a zero offset - the After
Upscaling placement - every sample lands exactly on a texel centre and the pass
degenerates to the previous per-texel resolve.

The depth and motion-vector guides are not shifted: a sub-pixel move of a
single-texel guide is a no-op under nearest sampling, and the model tolerates
the same half-pixel guide/colour offset in every DLSS title.

Shared colour/output resources use the active colour extent rather than the
game target's padded native allocation. Before-upscale mode therefore creates
Feature 18 at render resolution; after-upscale mode creates it at display
resolution. Loading transitions request a one-frame history reset.

## Model resolution

The `Model Resolution` controls (ported from
[DLSSNR-Cost-Scaler](https://github.com/xenmods/DLSSNR-Cost-Scaler)) run
Feature 18 on a raster smaller (or larger) than the colour region it processes.
`Uniform` applies one scale; `Per-Axis` is the proxy's experimental anamorphic
mode and scales width and height independently (its suggested 0.65 x 0.85 cuts
the neural workload by ~45%). Per axis the model extent is
`round(active * scale) & ~1`, floored at 64, with scale 1 left exact so the
native path is byte-for-byte the previous behaviour.

The scaling lives entirely in the existing colour transfer:

- The shared colour/output textures are allocated at the **model raster**
  (`Backend.cpp`, `EnsureResources`); the depth/motion guides stay at the guide
  extent. `TransferParams` carries `ActiveSize` and `WorkSize` so both passes know
  the mapping.
- `EncodeColorCS` dispatches over the model raster. Each model texel covers
  scene position `(id + 0.5) * active / work` on the unjittered grid, resampled
  from `+ JitterOffset`. The per-axis footprint (`active / work` on that axis)
  decides the kernel: at or above native resolution on that axis (footprint
  <= 1) it keeps the same Catmull-Rom kernel the jitter compensation already
  used; below native resolution (footprint > 1) it instead exact-area box
  averages that axis (`SampleNeuralSourceAreaMinify`), since reconstructing a
  single point instead of integrating the region a shrunk model texel actually
  covers leaves source frequencies above the model's new Nyquist limit free to
  alias into the proxy - and because that aliasing changes phase as the camera
  moves, the resolve reads it as neural shimmer. This is the box downsample
  [OptiScaler's DLSSNR fork](https://github.com/Dagherbou/OptiScaler_DLSSNR/discussions/2)
  uses below native. Filtering per axis means an anisotropic scale like
  0.65 x 0.85 only boxes the axis that is actually shrinking; the box's largest
  footprint is 4 source texels at the 0.25x minimum scale, so a direct
  exact-area implementation is cheap.
- `DecodeColorCS` dispatches over the active extent and samples the model answer
  and the proxy at `(pixel + 0.5 - JitterOffset) / ActiveSize`, i.e. bilinearly
  on the model raster. The luminance ratio and chroma are formed from that pair
  and applied to the untouched full-resolution pixel, so native detail is kept -
  this is the same "matched residual" idea as the proxy's `EnlargementMode = 1`,
  expressed as a ratio rather than an additive delta. The proxy's alternative
  direct (bilinear + RCAS) mode is not offered: it would need the model output
  inverse-tonemapped, which *Colour domain* above explains was a dead end, and
  DLSS sharpening already covers RCAS.

Feature 18 is created at the model raster, so a scale change rebuilds it. The
backend debounces the request (`SettleModelRaster`, 12 stable frames) so a
slider drag does not drain the interop queue every frame; the previous raster
keeps running until the value settles.

Two unbound-by-default hotkeys (`Neural Rendering Scale Up/Down Key`, Settings ->
Keybindings) step the active scale (both axes in Per-Axis mode) by 0.05 with a
HUD message, the proxy's PageUp/PageDown equivalent.

**Motion-vector scale under scaling.** The proxy multiplies `DLSSNR.MVecScaleX/Y`
by `work / native`. Community Shaders deliberately does not: the guide fix
(commit `7310537a`) established empirically that the model derives the
guide-to-colour ratio from the per-resource subrects, and folding a resolution
ratio into the scale on top of that counted it twice. The scale therefore stays
the guide resolution; only the colour/output subrects change.

## Transfer strength

`Transfer Strength` (0..2, default 1) is the proxy's overall edit weight,
expressed in the ratio design: `ResolveNeuralColor` raises the model/proxy
luminance ratio to it (log-space scaling, clamped afterwards by the existing
`0.5..2.0` guard) and multiplies the chroma blend by its saturated value. One is
therefore bit-exact with the previous behaviour, zero returns the untouched
frame, and two exaggerates the model's relative change. Unlike the `DLSSNR.*`
tuning parameters it is a per-frame constant, so it responds immediately.

## Luminosity strength

`Luminosity Strength` (0..2, default 1) is a second exponent applied on top of
`Transfer Strength`, but only to the luminance ratio - `ResolveNeuralColor`
raises the model/proxy ratio to `editWeight * luminosityStrength` instead of
`editWeight` alone. The chroma blend (`Color Strength`) is untouched, so lowering
it damps the light/dark swings a strong transfer can read as overly contrasty
while keeping the model's colour and detail edit at whatever strength the other
sliders already give it. One reproduces the pre-luminosity-strength behaviour
exactly; zero freezes luminance at the original regardless of `Transfer
Strength`.

## Per-category colour, transfer, luminosity strengths and hue guard

Skin, Hair, Eyes, Foliage, Landscape, Equipment, and Everything Else each carry
their own colour, transfer, and luminosity multipliers plus their own hue guard
toggle (`NeuralRendering::CategoryStrengths`). The category controls shape the
local resolve first; the global `Color Strength`, `Transfer Strength`, and
`Luminosity Strength` values multiply those results afterwards as the final
layer of adjustment. Unlike the old opt-in `Per-Category Strengths` checkbox,
category lookup is unconditional now: each category's hue guard needs to know
which material a pixel is on every pixel, so `materialCategoriesSRV` is a hard
requirement of `ValidateInputs` rather than only when per-category strengths
were enabled, and the category capture runs whenever Neural Rendering is
enabled with DLSS rather than only when that checkbox was set.

Only Hair hue-guards by default (`CategoryStrengths::hueGuard`); the other six
categories default off, matching the general observation that a colour bias is
most objectionable on hair's near-neutral shading and least useful to clamp on
categories users are more likely to want fully recoloured. `DecodeColorCS`
blends each category's hue-guard toggle through the same tent filter as the
strengths - as a continuous 0..1 "amount" rather than switching discretely - so
a material boundary softens the guard instead of flipping it outright, and
`ResolveNeuralColor` takes that blended amount (`hueGuardAmount`) instead of a
plain bool.

Classification stays entirely inside Community Shaders and does not use a DLSS
control-mask parameter. `Lighting.hlsl` and `RunGrass.hlsl` already know their
material permutations, so they store the category in the G channel of `Masks2`,
which Community Shaders creates as `R16G16_UNORM`
(`NeuralRenderingCategories::Encode`: id / 255, exact in 16-bit unorm). R keeps
upstream's vertex AO with upstream's values, alpha and blending, so the deferred
composite's AO is unchanged, and effect shaders keep upstream's `Masks2` write.
Untagged pixels resolve as Everything Else.

A category id cannot be blended: a lerp of two ids is a third id, and additive
Hair (2) over Equipment (6) stores 8, which decodes as Everything Else (0). A draw
has one source alpha for every channel of a target, and AO needs the material
alpha, so per-channel write masks keep the two apart (`Upscaling/CategoryBlend.h`):

- The deferred blend table (`Deferred::OverrideBlendStates`) mirrors RT0 onto
  `Masks2` as upstream does, but enables G only for unblended entries
  (`GetDeferredMasks2WriteMask`). Opaque and alpha-tested geometry, nearly all of
  it, writes AO and category in one draw.
- Blended deferred lighting passes are drawn a second time into G alone by
  `Upscaling::RenderDeferredPass`. These are the decals with alpha blending, which is how FaceGen hairlines, brows,
  beards and scars are authored, plus Terrain Blending's terrain. The redraw
  blend table (`MakeCategoryRedrawBlendDesc`) masks off every GBuffer target and
  `Masks2`'s R and blends G with source alpha. `ExtraFlags::NeuralCategoryRedraw`
  makes `Lighting.hlsl` output the same stochastic 0/1 coverage the normals use
  as `Masks2`'s alpha. The redraw binds a variant of the first draw's
  depth-stencil state with an inclusive test and no depth or stencil writes. It
  then restores that state and the deferred table.
- Forward lighting draws write G only, with source alpha over coverage when RT0
  blends and an overwrite when it does not (`MakeForwardCategoryBlendDesc`).

Coverage is dithered per frame (`FrameCount`-seeded), and `DecodeColorCS`'s tent
filter turns it back into a coverage-weighted mix of the category strengths. A
pixel whose category matches neither overlapping surface therefore points at
classification or at a writer outside these paths, not at blending.
`tools/test-neural-category-blending.cpp` checks all three rules on D3D11 WARP,
including that R and RT0 match upstream bit for bit.

Other alpha-blended lighting geometry (loose strands, lashes, translucent
clothing) is sorted and drawn after `Deferred::EndDeferred` through the forward
`Lighting.hlsl` permutation, with the deferred targets unbound. The capture
therefore spans the whole frame, all in `Upscaling`:

1. `BeginNeuralRenderingCategoryCapture` (`Deferred::StartDeferred`) arms the
   capture when Neural Rendering is on with DLSS. `StartDeferred` clears
   `Masks2`. `BSBatchRenderer_RenderPassImmediately<N>` hooks the same three
   dispatch sites as Light Limit Fix (the third is SE-only). It hands every
   deferred-pass dispatch to `RenderDeferredPass`. That function redraws a
   pass only when two things hold:
   - its lighting draw actually ran, which `BSLightingShader_SetupNeuralCategory`
     records;
   - the blend state the draw left bound blends RT0.

   Hooks further down the chain may defer a pass instead of drawing it, as
   Terrain Blending does. Upscaling installs its hook last, so Terrain Blending
   later draws those passes below it; `RenderTerrainBlendingPasses` therefore
   routes its blended terrain loop through `RenderDeferredPass` itself.
2. From `Deferred::EndDeferred` on, the same hook binds `Masks2` to `SV_Target7`
   around each forward lighting draw of the main world view, and
   `Deferred::ResetBlendStates(true)` selects the forward category variants.
   Reflection and cubemap passes are skipped. So is any draw whose slot-0 target
   is not the one the deferred pass restored, since a mismatched size would fail
   `OMSetRenderTargets`. The hook restores the previous slot-7 target and target
   mode after each draw.
3. `FinishNeuralRenderingCategoryCapture` (start of `Main_PostProcessing`)
   copies `Masks2` into the snapshot texture every Neural Rendering evaluation
   reads, disarms the capture and restores the original engine blend states.

Do not snapshot `Masks2` before the blended decals and copy it back after the
composite: that erases every decal's category, which is how FaceGen hairlines,
brows and beards read as the face or clothing beneath them. Do not store the
category in AO's bits either. Keeping it exact then requires binary coverage in
place of the material alpha, which dithers the AO of every blended surface.

`DecodeColorCS` reads that snapshot's G channel at the guide (render) resolution,
nearest-neighbour maps it to the active colour raster, and selects the category
multipliers before calling `ResolveNeuralColor`.

**Debug view.** The Neural Rendering settings tab's Debug section has a "Show
Material Categories" checkbox (`Upscaling::Settings::neuralRenderingDebugCategoryView`,
threaded through `NeuralRendering::Options::debugCategoryView` and
`NeuralRenderingBackend::FrameInputs::debugCategoryView` into the `TransferParams`
cbuffer's `DebugCategoryView` flag). When set, `DecodeColorCS` skips the resolve
entirely and writes a fixed, maximally-distinguishable colour per pixel's nearest-
neighbour category (`NeuralRenderingCategories::DebugColor`) instead - the raw
classification, not the tent-filtered strengths above, since a resolved strength
can't be mapped back to a category id. This is for tuning category boundaries
(e.g. checking a hairline isn't reading as Skin); the model still evaluates
normally underneath, so it carries the full Neural Rendering cost rather than
being a cheap preview. While it is on, `Upscaling::LogNeuralCategoryDraw` also
writes one `CommunityShaders.log` line per actor geometry and render stage
(`deferred`, `deferred, category redraw`, `forward`, or `forward, Masks2 unbound`)
with its technique, material, decal and alpha flags and the resolved
`IsHumanoidActor`/`IsHair`, so a pixel reading the wrong category can be traced
to the draw behind it. A blended deferred shape with a `deferred` line but no
`deferred, category redraw` line got no category.

Two categories cannot be derived from the shader permutation alone, and
`Upscaling::BSLightingShader_SetupNeuralCategory` (hooked onto
`BSLightingShader::SetupGeometry`, for every lighting draw that writes `Masks2`:
the deferred pass and the forward draws described above) resolves them per pass
from the geometry's owning actor:

- `ExtraFlags::IsHumanoidActor` is set when the actor's race has the
  `ActorTypeNPC` keyword; `Lighting.hlsl` maps that to Equipment after the
  skin, hair, eye, foliage and landscape branches. Bare skin (body as well as
  face) goes through the skin-tint permutations and is Skin before the flag is
  consulted, so Equipment ends up as armor, clothing and wielded weapons,
  including rigid (non-skinned) weapons, shields and helmets. Creature bodies
  stay Skin.
- `ExtraFlags::IsHair` is set from either of two signals. The pass's material
  is hair tint or its shader property carries the hair soft-lighting flag,
  which covers wigs and other hair worn as equipment. Or the geometry belongs
  to one of the actor's hair or facial-hair head parts or their extra parts
  (vanilla hairlines are Misc-type extra parts of their hair) and its material
  is not an accessory material - environment map, glow map, parallax,
  multilayer parallax or eye - since hair records also carry accessories such
  as environment-mapped earrings as extra parts. Head parts hang under the
  actor's skinned face node named by the part's editor ID: a FaceGen head names
  each shape, a runtime-assembled head names the part's root node. The hook
  therefore matches the geometry and each ancestor up to the face node's child
  against the NPC's head parts. The `HAIR` technique only covers pieces
  authored with the hair-tint shader type; `IsHair` overrides the
  technique-derived category in every permutation except `HAIR` itself.

## Depth-aware silhouette preservation

With the model below the colour resolution its edit is bilinearly upsampled,
so at a geometric silhouette the background's edit bleeds a texel or two into
thin foreground geometry. `NeuralSilhouetteWeight` (`ColorTransfer.hlsli`) ports
the proxy's guard: it reads the game depth (the same guide the model received,
bound as `t3` of `DecodeColorCS`) at the guide texel the colour pixel maps to,
measures the relative depth range of the five-texel cross and fades the edit
weight towards 0.25 across discontinuities above 2%. The backend forces it off
at native scale, where there is no upsample to bleed, so the 1.0 path stays
bit-exact.

## Alternating frames

`Alternate Frames` is the proxy's experimental "VRNR": Feature 18 runs on even
frames only. On odd frames the backend skips the encode, guide copies and the
D3D12 submission entirely and runs `DecodeColorCS` alone against the shared
textures, which still hold the previous frame's proxy/answer pair (D3D11
already waited on that submission). `NeuralStaleEditWeight` compares the stale
proxy's luminance with the fresh frame encoded the same way and fades the edit
wherever they differ, so moving content shows the clean current frame instead
of a misplaced ratio. A pending history reset, a raster change or a latched
failure always forces an evaluation, so the first frame after enabling is never
a skip.

Two known compromises, both shared with the proxy: the model's own temporal
state sees every second frame, and the motion vectors it is given describe one
frame of motion although two elapsed. Doubling the motion-vector scale on
evaluated frames would be the obvious refinement and has not been tried.

## Model tuning parameters

`DLSSNR.Intensity` / `Style` / `LocalToneStrength` / `LocalStructureStrength` /
`SkinStructureStrength` / `UseAutoMask` / `Hint.Render.Preset` are **latched when
the feature is created**. Writing them only at evaluate does nothing. `Runtime`
sets them at create time; changing a slider therefore needs the feature rebuilt,
which today means toggling Neural Rendering off and on. A debounced in-place
rebuild (drain the interop queue, `ResetFeature()`, recreate) is a follow-up - it
must go through the same GPU-idle path `EnsureResources()` uses, never a bare
`release()`.
