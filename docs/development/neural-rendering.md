# DLSS Neural Rendering (NGX Feature 18)

Neural Rendering is Personal-original and owned by the Neural Rendering feature. It consumes
renderer/Upscaling resources at several pipeline stages through a minimal documented seam, but is
not part of the Bottle-owned Upscaling feature. Its Separate Upscaling design follows wilsjo2's
OptiScaler deferred-residual experiment, and its proxy raster follows xenmods' DLSSNR-Cost-Scaler.

It drives NVIDIA's DLSS Neural Rendering model (`nvngx_dlssnr.dll`, NGX feature 18) directly.
NVIDIA ships no public integration for it; the runtime DLL is proprietary and user-supplied, and
the parameter contract (`DLSSNR.*` string keys) is reverse-engineered, not documented.

Source layout:

| Path | Role |
|---|---|
| `src/Features/NeuralRendering.{h,cpp}` | the `NeuralRendering` feature: settings and UI, placement logic, category capture, Finished Image, comparison capture, hooks; thin adapter to the backend |
| `src/Features/NeuralRendering/Backend.{h,cpp}` | `NeuralRenderingBackend` - owns the D3D11↔D3D12 shared textures, the colour/guide transfer passes, the failure latch |
| `src/Features/NeuralRendering/Runtime.{h,cpp}` | `NeuralRenderingNGX::Runtime` - loads the snippet DLL, drives NGX feature 18 create/evaluate/release, the `GetModuleFileNameW` caller-gate hook |
| `src/Features/NeuralRendering/D3D12Interop.{h,cpp}` | private D3D12 device/queue, shared-fence ping-pong with the game's D3D11 context |
| `features/Neural Rendering/Shaders/NeuralRendering/` | `EncodeColorCS` / `DecodeColorCS` / `CopyDepthGuideCS` / `EncodeResidualCS` / `ApplyResidualCS` and `ColorTransfer.hlsli` |

Where it hooks into the frame:

| Site | Call | Purpose |
|---|---|---|
| Upscaling seam S1: `Upscaling::Upscale()`, DLSS branch | `NeuralRendering::PrepareUpscaleInput()` | Before Upscaling (substitutes the DLSS input), Separate Upscaling (prepares the private residual) |
| Upscaling seam S2+S3: `Upscaling::PerformUpscaling()`, between `Upscale()` and `UpscaleDepth()` | `NeuralRendering::ResolveUpscaledFrame()` | After / Separate Upscaling (result written back into Upscaling's sharpener input), Finished Image depth snapshot |
| `Main_PostProcessing` call site, chained on Upscaling's hook | `NeuralRendering::Main_PostProcessing` | forward category capture finish, history reset, placement changes, comparison capture |
| `Hooks.cpp` tonemap hook | `CaptureDisplayTransform()`, `ApplyFinishedImage()` | display-matched proxy, Finished Image |
| `Deferred.cpp` | `CaptureCategories()`, `RestoreCategories()` | material-category snapshot around decals |
| `BSLightingShader::SetupGeometry`, `BSBatchRenderer::RenderPassImmediately` | own hooks | humanoid/hair flags, forward-draw category writes |
| `LoadingMenu` close | own `MenuOpenCloseEvent` sink | temporal history reset |

Settings live in the "Neural Rendering" section of the settings JSON. Builds before the split
stored them as `neuralRendering*` keys in the "Upscaling" section; `State::LoadFromJson`
migrates those once (`NeuralRendering::MigrateLegacyUpscalingSettings`). Shaders moved from
`Data\Shaders\Upscaling\NeuralRendering\` to `Data\Shaders\NeuralRendering\`; stale copies in the
old folder are unused.

## Placement

The `Placement` setting chooses where in the frame the model runs.

### Before Upscaling (`placement == 0`)

Runs from seam S1 in `Upscaling::Upscale()`, on the render-resolution colour that is about
to be handed to DLSS. The model's output replaces the DLSS input colour; DLSS then
upscales the enhanced render-resolution frame to display resolution as normal.
Colour and the depth/motion guides are all at render resolution.

The render-resolution colour is the game's *jittered* raster: every frame is
projected with the sub-pixel Halton offset DLSS later removes. Feature 18 has no
jitter parameter (neither does OptiScaler's pre-SR path), so the backend
compensates for it itself; see *Jitter* below. `NeuralRendering::PrepareUpscaleInput()` passes the
same offset Streamline receives (`-jitter`) through `NeuralRendering::Options`.

### After Upscaling (`placement == 1`)

Runs from seam S2 in `Upscaling::PerformUpscaling()` after the colour upscale and before
`UpscaleDepth()`, on the display-resolution upscaled frame, before RCAS
sharpening / copy-back. Depth and motion therefore remain the exact
render-resolution guides used for the colour upscale. The backend tracks the
colour region and guide region separately (`FrameInputs::guideWidth/guideHeight`).

### Separate Upscaling (`placement == 2`, experimental)

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

### Finished Image (`placement == 3`, default)

Runs from `PostProcessingExtensions::Main_HDRTonemapBlendCinematic_Render` (`src/Hooks.cpp`),
the single hook point that sees every path the frame's tonemap can take: right after
`State::HandlePostProcessing()` when Effects11 replaces the pass outright and owns the tonemap
itself, and right after the vanilla tonemap/passthrough call otherwise - which covers Post
Processing owning the tonemap too, since in that case the vanilla call just takes its
passthrough branch over Post Processing's already-finished result (see
`PostProcessing::PreProcess()`'s own comment). Either way, by the time
`NeuralRendering::ApplyFinishedImage()` runs, the game render target it's given
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
`NeuralRendering::CaptureFinishedImageGuides()` runs in `PerformUpscaling()` just before
`UpscaleDepth()`: it copies `kMAIN` depth into a private snapshot while it is still on the
motion-vector raster and records that render-resolution extent (before
`dynamicResolutionLock = 1`). `NeuralRendering::EvaluateFinishedImage()` then passes the
snapshot as depth and sets the guide extent and guide jitter exactly as the After Upscaling
placement does. The captured guides are consumed on use, so the model runs at most once per
upscaled frame: a second tonemap-pass call with a different colour target would otherwise reset
its temporal history every frame. It reads the colour's active resolution from `globals::game::graphicsState->screenWidth/Height`
(the same authoritative source every other Neural Rendering call site uses), not from
`GetDesc()` on any of the individual resources - those can legitimately be a larger,
differently-padded allocation than the frame's active region, so comparing them against each
other for a "do these agree" fail-closed check is a false alarm waiting to happen (an earlier
version of this placement did exactly that and silently no-op'd every frame as a result).

The output resource is **not** the `outputTexture` the After Upscaling and Separate
Upscaling placements use: that one matches `kMAIN`, whose format differs from `kFRAMEBUFFER`
(UNORM in SDR) and from HDR Display's float16 redirect, and `CopyResource` between mismatched
formats is silently dropped by D3D11 - a second earlier version of this placement ran the model
every frame and discarded the result exactly that way. Instead
`NeuralRendering::EnsureFinishedImageTexture()` lazily allocates
`finishedImageTexture` to match `output`'s own width, height, format and sample
count (recreated when any change), and `ApplyFinishedImage()` copies it back into
`output` in place on success. When `kFRAMEBUFFER`'s slot has a null texture pointer (it can alias
the swap-chain backbuffer through its views alone), the texture is recovered from its SRV. A
target that cannot take a typed UAV write - sRGB, typeless or multisampled - fails closed with a
one-time warning naming the format.

**Fails closed.** `EvaluateFinishedImage()` returns false - leaving the caller's
buffer untouched - unless Finished Image is the active placement, the backend is available, and
the depth/motion guides exist. `ApplyFinishedImage()` additionally skips while a
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

Bottle's Streamline constants report that copy as undilated, which makes DLSS
dilate it a second time; Upscaling fix F1 (`maintenance-policy.yaml`) restores
`motionVectorsDilated = eTrue`. That concerns DLSS SR only, not the model.

## Depth convention (Reverse Z)

Reverse Z (CORE, on by default, latched at boot) stores depth as
`1 - conventional depth`: near is 1, far is 0. It arrived on Personal after
Neural Rendering was written against conventional depth, and three depth
consumers must follow it or edges flicker:

-   **Feature 18**: `DLSSNR.DepthInverted` is set from
    `FrameInputs::depthInverted` (`Runtime::Execute`), filled from
    `ReverseZ::IsActive()` in `MakeFrameInputs`.
-   **Separate Upscaling's private DLSS SR**: the same flag adds
    `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted` at create time
    (`Runtime::ExecuteSuperResolution`). It cannot change while the feature
    exists, which holds because Reverse Z is boot-latched.
-   **`NeuralSilhouetteWeight`**: maps its samples back to conventional depth
    under `REVERSE_Z` (see *Depth-aware silhouette preservation*).

`CopyDepthGuideCS` copies depth verbatim in both conventions. The main DLSS
instance reads the same state through Streamline's `depthInverted` (Bottle).
Runtime-compiled shaders get the `REVERSE_Z` define from `Util::CompileShader`.

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
(`Color::GammaToLinearSafe`), brings only over-range pixels back into 0..1 (a single
hue-preserving factor, so an SDR frame reaches the model unchanged; on an HDR target a soft
shoulder, see *HDR highlight roll-off* below), and re-encodes with the same
curve; the resolve decodes original, proxy and model with that curve, applies the edit in linear
light and re-encodes. Treating the finished frame as scene-linear - what an earlier version did -
Reinhard-compressed and re-encoded an already-encoded image, handing the model a washed-out,
over-bright proxy. The one exception is HDR Display's float16 redirect when the scene reaching it
is linear (Linear Lighting, or Post Processing owning the tonemap) - the same test `HDROutputCS`
applies - which keeps `kSceneLinear`.

### Scene gamma (Linear Lighting off)

A third domain, `kSceneGamma`, exists for the pre-tonemap placements while Linear
Lighting is **off**. `kMAIN` then holds gamma-encoded values, and in SDR `ISHDR.hlsl`
grades them and writes the result straight out - it only calls `LinearToGammaSafe`
under `ENABLE_LL`. Treating that buffer as linear was wrong twice over: the proxy
sRGB-encoded an already-encoded frame, handing the model a washed-out image, and
the resolve multiplied the raw encoded buffer by a ratio measured in linear light,
which makes the edit roughly 2.2x stronger in stops than the model asked for.

`SceneColorDomain()` picks `kSceneGamma` for Before, After and Separate Upscaling
whenever Linear Lighting is not in effect (`IsLinearLightingActive()`: its setting is off,
or the flat world map is open, where Linear Lighting stands down). In that domain
`NeuralDomainToLinear` / `NeuralLinearToDomain` use `kNeuralSceneGamma` (2.2, the
curve ISHDR itself linearises this pipeline's output with in its HDR path; Open
Shaders uses Skyrim's 1.6 instead, and the constant is kept in one place so the two
can be A/B'd), so the resolve decodes the original, applies the edit in linear light
and re-encodes with no special case of its own. `EncodeNeuralProxy` puts exposure
where the game puts it - on the encoded values - and then either runs the ISHDR
replica there and decodes its display-encoded result, or decodes first and hands
linear light to whichever other curve is selected.

The **Legacy** proxy curve is exempt: it stays in `kSceneLinear` whatever Linear
Lighting is doing, because reproducing the September 2026 build means reproducing
its encoding behaviour too. If you play with Linear Lighting off, part of that
build's punch may be exactly this - the ratio applied to gamma-encoded values is
roughly the linear-light edit raised to 2.2.

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

`Color Strength` (0..2, default 1) raises the guarded per-channel chroma ratio
above to itself before it is applied: zero collapses the ratio to one in every
channel, reproducing the renderer's own chroma exactly (indistinguishable from
no colour transfer); one is the model's transferred chroma unchanged; above one
extrapolates the same relative colour change further, still inside the
per-channel guard. That result still fades to the renderer's own chroma only in
near-black pixels, where normalized colour is numerically ambiguous, and is
gated by `Transfer Strength` the same way the luminance edit is. HDR headroom
and alpha remain renderer-owned. No temporal accumulator or midpoint blend is
involved; every frame is independently re-anchored.

### HDR highlight roll-off (Finished Image)

On HDR Display's float16 redirect a gamma-encoded finished frame carries
highlights above one: the redirect's 1.0 is paper white, and `HDROutputCS`
PQ-encodes it against `hdrPaperWhite`, so the display shows values up to
`hdrPeakNits / hdrPaperWhite` (about 3.9 at the 800/203 defaults) and clips
beyond. The model was trained on SDR frames and needs 0..1. The original encode
scaled any pixel whose linear peak exceeded one down to exactly one, which kept
hue but flattened every highlight of a given hue to the same value - a sunlit
cloud, a specular glint and a torch flame all read as the same white, so the model
had no highlight structure to work with.

`EvaluateFinishedImage` now passes that display peak as `Options::highlightWhite`
(-> `TransferParams.HighlightWhite` -> `NeuralDisplayTransform::highlightWhite`)
whenever the display-gamma domain runs on the redirect, and `EncodeNeuralProxy`
applies `NeuralHighlightRolloff` to the pixel's peak instead: the identity below
`kNeuralHighlightKnee` (0.8), then an extended-Reinhard shoulder with unit slope
at the knee that reaches exactly one at the display peak and clamps beyond it.
It is strictly increasing up to the peak (1.0 -> 0.90, 2.0 -> 0.98 at the
defaults), so highlights that differ on screen still differ to the model. The
factor is still one scalar per pixel, so hue is preserved. This is the "hybrid"
reversible proxy of RenoDX's DLSS 5 add-on / OptiScaler's DLSSNR fork (identity
midtones, unclipped highlights), bounded by the real display peak.

The resolve is unchanged: the luminance ratio and chroma are measured between
model and proxy in that rolled-off space and applied to the untouched original,
and `NeuralStaleEditWeight` encodes the fresh frame through the same transform.
Above the knee a proxy-space ratio therefore corresponds to a somewhat larger
linear change than it would below it; inverting the shoulder instead would
reintroduce the unbounded slope near white that *Colour domain* above rules out.
SDR targets (`highlightWhite` zero), a display peak at or below paper white, and
every scene-linear placement keep the previous encode bit-for-bit.

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

**Where the inputs come from.** `NeuralRendering::CaptureDisplayTransform()`
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
exposure is replicated; its tonemapper (one of several selectable curves -
ACES, Frostbite, Melon, ...) and its full colour grading (white balance,
contrast, saturation, split-toning, LUT) are not - reproducing those exactly
would need `colorgrading.hlsl`'s whole RenoDX/ACEScct-based pipeline, well
beyond what a proxy needs. Under Effects11 nothing is captured either. Both
cases fall back to `NeuralAcesFilmic` (`ColorTransfer.hlsli`), a small
dependency-free approximation of a generic filmic response - shadows and
midtones held close to linear, only the highlights rolled off - rather than a
plain Reinhard, which compresses continuously from black and reads to the
model as an implausibly flat, low-contrast frame regardless of which real
tonemapper the user actually has active. The luminance edit is applied in
scene-linear light and then passes through the real tonemap and contrast, so
its final magnitude is still reshaped by them (a contrast of 1.2 makes a
mid-tone edit ~20% stronger in log space); `Transfer Strength` remains the
knob for that residual.

### Raw Model Output

`Raw Model Output` (Finished Image only, `Options::rawModelOutput`) is a
diagnostic that bypasses `ResolveNeuralColor` entirely: `DecodeColorCS` writes
Feature 18's answer converted straight back to display-referred colour,
preserving the renderer's alpha, wherever `RawModelOutput` is set and
`ColorDomain` is `kNeuralColorDomainDisplayGamma`. It is gated to that domain
in the shader itself, not just the UI - in the scene-linear domain (Before/
After/Separate Upscaling) the same bytes are a display-referred, roughly 0-1
proxy answer, and dumping that into a linear HDR buffer the game's own
tonemapper still has to process is not a meaningful image, just a washed-out
frame. It exists to separate two possible causes of a weak-looking result: if
this looks dramatically stronger than the normal resolve, the model and its
tuning are fine and the attenuation is in the transfer/compositing math above;
if it looks weak too, the problem is upstream of the resolve entirely (model
input, guides, or colour-domain conversion). Not meant to be left on.

## Comparison aids

Besides the four-frame comparison screenshot (`ServiceComparison`), the settings
tab's **Compare** section has two runtime-only aids. They live in
`NeuralRendering::CompareView`, not `Settings`, so they are never saved and
cannot be left on by accident across sessions.

**Split Screen** (`CompareView::wipe`, `wipePosition`). `MakeOptions` passes the
split as `Options::wipePosition` (negative = off) -> `TransferParams.WipePosition`.
`DecodeColorCS` handles it before anything else: left of the split it writes the
input pixel through untouched (no edit and no debug view), and a two-pixel
black/white divider marks the split so it reads on bright and dark content alike.
The divider's white is 1.0 in the display-gamma domain and `1 / exposure` of the
proxy's display transform in scene linear, i.e. roughly mid-bright once the frame
is tonemapped. It works in every placement. Before Upscaling feeds DLSS a split
frame, which is fine for a visual comparison. The model still evaluates the whole
frame, so the right half is exactly what the full-screen result would be.

**Frame Hold** (`CompareView::frameHold`, Finished Image and After Upscaling). The model's tuning
parameters rebuild the feature and its history when they change, and the live
scene keeps moving, so judging a slider change by eye is unreliable. Frame Hold
freezes the input instead: on its first frame `CaptureFrameHold` copies the
finished colour, the depth snapshot and the category snapshot into `heldColor` /
`heldDepth` / `heldCategories` (each mirroring its source's description so a
whole-resource `CopyResource` is valid) and records the guide extent and guide
jitter. From then on the placement evaluates those instead of the live frame,
every frame, with a history reset on capture. The live guides are still
consumed, so the one-evaluation-per-frame contract is unchanged, and the result
still goes through the placement's usual copy-back. The screen therefore shows the
held frame with the current strengths and tuning while the game keeps running
underneath. The HUD is drawn later and stays live.

After Upscaling holds the same three resources, taken at its own point in the
frame: the upscaled colour `ResolveUpscaledFrame` is about to edit, the live
`kMAIN` depth, and the category snapshot. Finished Image holds its pre-`UpscaleDepth`
depth snapshot instead, because that is the raster its guides are on. `heldPlacement`
records which placement captured the hold, so switching placement releases it
rather than re-evaluating one placement's frame under another's guide contract.
Before and Separate Upscaling hand their frame straight to DLSS and have nothing
to freeze, so the checkbox is disabled there.

A held frame has no motion, so `Options::staticMotion` makes `EvaluateModel` clear
the shared motion-vector texture to zero instead of copying the live vectors
(which describe a scene that has moved on). It also disables alternating-frame
skips. The hold is released - with a history reset, since the model's history is
of the held frame - when the checkbox is cleared, Neural Rendering is disabled,
the placement changes, or the colour target's size or format changes (the next
frame then recaptures). Combining Frame Hold with Split Screen gives an on/off
comparison of one fixed frame, the equivalent of OptiScaler DLSSNR's frame hold
with "Apply the model" toggled.

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
Feature 18 on a raster smaller than the colour region it processes (0.25..1
per axis).
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
  decides the kernel: at native resolution on that axis (footprint 1) it
  keeps the same Catmull-Rom kernel the jitter compensation already
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

**No supersampling.** Scales above one (up to 2, supersampling the model
input) were offered until 2026-09-26 and removed: the model's cost grows with
the square of the scale, but the edit is applied as a luminance/chroma ratio to
the untouched full-resolution frame, so a finer model raster added next to
nothing visible. `kMaximumResolutionScale` is 1, the sliders and the scale
hotkeys stop at 1, and `LoadSettings` clamps a saved scale above one to native.

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
luminance ratio to it (log-space scaling, clamped afterwards by the `Max Ratio`
guard below) and multiplies the chroma blend by its saturated value. One is
therefore bit-exact with the previous behaviour, zero returns the untouched
frame, and two exaggerates the model's relative change. Unlike the `DLSSNR.*`
tuning parameters it is a per-frame constant, so it responds immediately.

### Ratio guard / Max Ratio

The luminance ratio above can be clamped to `1/Max Ratio..Max Ratio`
(`ColorTransfer.hlsli`, `ResolveNeuralColor`) so a single evaluation cannot
flash or collapse a pixel without bound. `Enable Ratio Guard` (default **off**)
controls whether that clamp applies at all; the `Max Ratio` slider (1..8,
default 2) only appears once it is on. Off, `Backend.cpp` sends the shader an
effectively unbounded value instead of a smaller one, so the clamp never binds
and the model's genuine light/dark change - including turning a lit surface
fully into shadow, which any finite guard eventually caps - reaches the frame
exactly as evaluated. On, raising `Max Ratio` allows a stronger effect at the
risk of flashing on an unstable model frame; values below one are treated as
one in the shader.

Earlier revisions exposed `Max Ratio` as an always-on 1..4 slider while
`Backend.cpp` separately hard-clamped the value it actually sent to the shader
to 8 - so past 4 in the UI (or an ini edit past 8) had no further effect, which
read as the guard being unliftable. The guard is now opt-in and its two ranges
match (1..8 both in the UI and in `Backend.cpp`).

## Broad and Detail luminosity

`Broad Luminosity` and `Detail Luminosity` (both 0..2, default 1) replace the
single `Luminosity Strength` they grew out of. Both scale only the luminance
edit - the chroma blend gated by `Color Strength` is untouched - but they scale
different halves of it.

`ResolveNeuralColor` measures the edit in stops,
`delta = log2((modelLuma + floor) / (proxyLuma + floor))`, and splits it:

```
low   = toneLow            // edge-aware blur of delta, or delta itself when absent
high  = delta - low
tone  = editWeight * categoryLuminosity * (low * Broad + high * Detail)
ratio = clamp(exp2(tone), 1 / maxRatio, maxRatio)
```

`low` is the region-level relighting - a whole wall or hillside the model wants
brighter - and `high` is its own local contrast and micro-detail. Everything
after `targetLuma` is unchanged, and the per-category luminosity multiplier
scales both bands equally, so whether the split is active depends only on the
two global values. **With `Broad == Detail == x` this is exactly
`ratio^(editWeight * categoryLuminosity * x)`** - the old single exponent - so an
upgraded config produces an identical frame, and the shader takes that path
whether or not the band data exists.

### The band passes

They run once per model evaluation, right after the answer is copied back, at the
model raster, and only while the two strengths differ. Two `R16G16_FLOAT`
textures at the model raster (`toneData`, `toneScratch`) are all they need; they
are ordinary D3D11 textures, never shared with D3D12. Half precision resolves
about 0.01 stops, far finer than the edit carries.

- **`PrepareToneDataCS`** reads the proxy and the answer with the same decode the
  resolve uses and writes `(log2 proxy luminance, delta)`. The proxy luminance
  rides along because the filter is edge-aware: it has to know where the *image*
  has an edge, not just where the edit does.
- **`FilterToneDataCS`**, dispatched horizontally then vertically (the `VERTICAL`
  define picks the axis), is a separable bilateral blur of `delta` alone:
  17 taps, Gaussian spatial weights, and an edge weight of
  `exp(-kNeuralBandEdgeSharpness * |change in log2 proxy luminance|)`. Separable
  bilateral filtering does not compose into a true 2D bilateral kernel, but the
  thing being filtered is a smooth edit map rather than an image. The horizontal
  pass writes `toneScratch`; the vertical one writes back into `toneData`, which
  no pass is reading at that point, so two textures suffice.
- **`DecodeColorCS`** samples `toneData.y` bilinearly at the same (possibly
  reprojected) model position it already samples the answer and the proxy at, so
  a reused alternating-frame answer carries its own band data with it.

`Band Radius` (2..32 model texels, default 8) sets how far the blur reaches. It
is scaled by the model resolution so it covers the same part of the screen at
0.5x and at 1x, and the tap count is fixed - the stride stretches (or shrinks) to
fit the radius - so the cost does not change with it. The stride is fractional and
the taps are read through the bilinear sampler; a whole-texel stride would round the
reach to multiples of eight texels and leave most of the range without effect. Open Shaders splits at a 5x5 kernel
with a 1-texel radius; at that size "high" is only the model's sharpening and
"broad" still contains all the region-level relighting that reads as the Neural
Rendering look, which is the thing worth controlling separately.
`kNeuralBandEdgeSharpness` is 2 (Open's value); 3-4 is tighter, for content where
the detail band shows halos.

`TransferParams.BandParams.z` tells the resolve whether the band textures hold
data for this evaluation; when they do not - equal strengths, a failed
allocation, a shader that would not compile, or the first frame after a raster
change - `hasToneData` is false, `low` falls back to `delta`, `high` is zero, and
only `Broad` applies. That is the same maths as the unsplit edit, so the fallback
is silent rather than a visible change.

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
were enabled, and `CaptureCategories` runs whenever Neural
Rendering is enabled rather than only when that checkbox was set.

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
material permutations, so they store the category in the low three bits of the
existing `R16_UNORM` `Masks2` value. The upper thirteen bits continue to carry
vertex AO, limiting the maximum AO representation change to `7/65535`. Untagged
pixels resolve as Everything Else. `Masks2` inherits each material's alpha
blend state, so `Lighting.hlsl` writes it with the same stochastic 0/1 coverage
the normals use rather than the material alpha: a lerp of two packed values
scrambles the discrete category, turning a blended hairline strand at alpha
below 0.5 over the face into Skin, or into an arbitrary category once the two
surfaces' AO bits differ. Each pixel therefore holds one surface's exact AO and
category; the dither is temporal (`FrameCount`-seeded), so the AO lerp is
recovered in expectation under DLSS/TAA, and only blended materials with a
vertex AO below 1 see any change at all.

Alpha-blended lighting geometry is not part of the deferred pass at all: the
engine sorts it and draws it after `Deferred::EndDeferred`, through the forward
`Lighting.hlsl` permutation, with the deferred targets unbound. Hair strands and
hairline scalps live there, which is why a deferred-only capture showed the
face's Skin under the roots and the background's Everything Else under the
blended mid-section while only the alpha-tested core read as Hair. The capture
is therefore three steps, all in `Upscaling`:

1. `CaptureCategories` (Deferred's blended-decals hook) copies
   the opaque categories out of `Masks2` before decals alpha-blend into it.
2. `RestoreCategories` (end of `Deferred::EndDeferred`, after
   `DeferredPasses` has consumed the decal-blended vertex AO) copies that
   snapshot back into `Masks2` and arms the forward capture. From here
   `BSBatchRenderer_RenderPassImmediately` binds `Masks2` to `SV_Target7` around
   each forward lighting draw of the main world view (reflection and cubemap
   passes are skipped, as is any draw whose slot-0 target is not the one the
   deferred pass restored, since a mismatched size would fail
   `OMSetRenderTargets`), and the forward `PS_OUTPUT` carries the same packed
   category write with the same 0/1 coverage.
3. `FinishCategoryCapture` (start of `Main_PostProcessing`)
   re-snapshots `Masks2` so every Neural Rendering evaluation keeps reading the
   snapshot texture, now with both the opaque and the forward categories.

`DecodeColorCS` reads that snapshot at the guide (render) resolution,
nearest-neighbour maps it to the active colour raster, and selects the category
multipliers before calling `ResolveNeuralColor`.

**Debug view.** The Neural Rendering settings tab's Debug section has a "Show
Material Categories" checkbox (`NeuralRendering::Settings::debugCategoryView`,
threaded through `NeuralRendering::Options::debugCategoryView` and
`NeuralRenderingBackend::FrameInputs::debugCategoryView` into the `TransferParams`
cbuffer's `DebugCategoryView` flag). When set, `DecodeColorCS` skips the resolve
entirely and writes a fixed, maximally-distinguishable colour per pixel's nearest-
neighbour category (`NeuralRenderingCategories::DebugColor`) instead - the raw
classification, not the tent-filtered strengths above, since a resolved strength
can't be mapped back to a category id. This is for tuning category boundaries
(e.g. checking a hairline isn't reading as Skin); the model still evaluates
normally underneath, so it carries the full Neural Rendering cost rather than
being a cheap preview.

Two categories cannot be derived from the shader permutation alone, and
`NeuralRendering::SetupGeometryCategory` (hooked onto
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
  to one of the actor's hair or facial-hair head parts (or their extra parts,
  which is how hairlines attach): head parts hang under the actor's skinned
  face node as children named by the part's editor ID, so the hook walks up
  from the geometry to the child of that node and matches its name against the
  NPC's head parts. The `HAIR` technique only covers pieces authored with the
  hair-tint shader type, and hair mods commonly author hairlines, braids and
  loose strands with the default or skin-tint type instead; those compiled as
  Equipment (humanoid), Skin (skin-tint, or skinned with the humanoid flag
  cleared) or Everything Else, which is what the category debug view showed
  along the hairline and braid. `IsHair` overrides the technique-derived
  category in every permutation except `HAIR` itself.

## Depth-aware silhouette preservation

Experimental and off by default (`depthAwareResolve = false`) since 2026-09-26;
the UI labels it "(Experimental)". Saved settings keep the user's own choice.

With the model below the colour resolution its edit is bilinearly upsampled,
so at a geometric silhouette the background's edit bleeds a texel or two into
thin foreground geometry. `NeuralSilhouetteWeight` (`ColorTransfer.hlsli`) ports
the proxy's guard: it reads the game depth (the same guide the model received,
bound as `t3` of `DecodeColorCS`) at the guide texel the colour pixel maps to,
measures the relative depth range of the five-texel cross and fades the edit
weight towards 0.25 across discontinuities above 2%. The backend forces it off
at native scale, where there is no upsample to bleed, so the 1.0 path stays
bit-exact.

The 2% threshold assumes conventional depth, where the far scene sits near 1.0
and only large near-field jumps clear it. Read raw under Reverse Z the ratio
becomes roughly `1 - zNear/zFar`: it clears 2% at almost every silhouette and on
grazing surfaces, and with jittered guides the weight swings between 0.25 and 1
each frame, which shows as edge flicker. Under `REVERSE_Z` the five samples are
therefore converted back with `1 - d` first. That map is affine, so it commutes
with the bilinear taps and gives exactly the conventional-depth result.

## Alternating frames

`Alternate Frames` is the proxy's experimental "VRNR": Feature 18 runs on even
frames only. On odd frames the backend skips the encode, guide copies and the
D3D12 submission entirely and runs `DecodeColorCS` alone against the shared
textures, which still hold the previous frame's proxy/answer pair (D3D11
already waited on that submission). A pending history reset, a raster change or
a latched failure always forces an evaluation, so the first frame after enabling
is never a skip.

**Reprojection.** That pair was computed for the previous frame, so every scene
point sat somewhere else in it. `TransferParams.StaleAnswer` tells the decode to
follow the game's motion vector (`MotionVectors`, `t7`, the raw
`kMOTION_VECTOR` target at the guide resolution) back to where the point was -
Skyrim stores current -> previous as a normalised UV offset over the active
region, the same normalisation as the decode's `uv`, so
`answerUV = uv + motion` - and sample the answer and its proxy there. Only
then does `NeuralStaleEditWeight` compare the reprojected stale proxy with the
fresh frame encoded the same way and fade the edit where they still differ
(disocclusion, a light switching, animated surfaces). A point whose previous
position is off screen has no answer to reuse and shows the clean frame. The
motion vector is read nearest-neighbour at the pixel's jitter-corrected guide
position (`NeuralGuidePosition`), as the category lookup is.

An earlier version sampled the stale pair at the pixel's *current* position.
Under any camera motion - which in Skyrim includes idle sway and head bob -
almost every pixel then failed the stale-edit comparison, so skip frames showed
the clean frame and evaluated frames the full edit: the whole image strobed
between enhanced and unenhanced at half the frame rate. The backend only skips
an evaluation when it has a motion-vector view to reproject through
(`FrameInputs::motionVectorsSRV`); every placement passes the game's.

**Motion across the skipped frame.** The model's own temporal state sees every
second frame, but the game's motion vectors describe one frame of motion. On an
evaluated frame whose previous evaluation was exactly two frames back
(`State::lastEvaluatedFrameIndex`), `EvaluateModel` therefore doubles
`DLSSNR.MVecScaleX/Y` - a constant-velocity extrapolation of this frame's motion
across the skipped one. A history reset keeps the plain one-frame scale (there is
no history to bridge), and a gap longer than two frames is not treated as a skip:
it only happens when `Run` was not called at all, e.g. while a menu paused the
game. The proxy does not do this; it hands the model one frame of motion for two.

## Presets and the Advanced section

A preset is a table of values (`NeuralRendering::PresetValues`, `kPresets` in
`NeuralRendering.cpp`) that `ApplyPreset()` writes into the ordinary settings. There is no
separate code path behind it: every slider still works once a preset is applied, and the
preset only records where the current values came from.

| Setting | Full (default) | Vanilla-Plus |
|---|---|---|
| Placement | Finished Image | After Upscaling |
| NR Style | Default (0) | Cinematic (2) |
| NR Intensity | 1.0 | 0.8 |
| Local Tone / Local Structure | 1.0 / 1.0 | 0.75 / 0.9 |
| Skin Structure | Auto (-1) | 0.9 |
| Automatic Mask | on | on |
| Proxy Curve | Display-matched | Legacy |
| Color Strength | 1.0 | 0 |
| Transfer Strength | 1.0 | 1.0 |
| Broad / Detail Luminosity | 1.0 / 1.0 | 1.0 / 1.0 |
| Band Radius | 8 | 8 |
| Ratio Guard | off | on, Max Ratio 2.0 |
| Category strengths | all 1.0 except Skin Color 0.6; Hair hue guard on | all 1.0, hue guards off |
| Depth-Aware Silhouette | off | off |

**Full** is the current look: the model's own answer applied to the finished frame, with no
guard on how far it may push a pixel. Skin Color Strength is 0.6 because the model's skin tint
is its most visible overreach; the `Settings` default matches, so a fresh install and
`Restore Defaults` agree.

**Vanilla-Plus** reproduces the 2026-09-09 build (`5947cf63`) on the current resolve rather
than by restoring old code. That build's edit was luminance-only, and `Color Strength` at zero
reproduces it *exactly*, not approximately: the resolve raises the model/proxy chroma ratio to
`colorStrength`, so zero collapses it to one in every channel, `normalizedTarget` reduces to
`normalizedOriginal`, and the two endpoints of the final `lerp` coincide whatever its weight
is. Its Style is Cinematic (2): the 2026-09-09 build asked for Style 3, which aliases 2 (the
72-case probe in kibblerz's findings), and a stored 3 has been clamped to 2 on load since
`NeuralRendering.cpp:586`. Its category values are neutral rather than absent, so moving on
from Vanilla-Plus starts from a clean state even though they do nothing while Color Strength
is zero. Its guard is the +-1 stop that build always applied; see *Ratio guard A/B* under
*What still has to be measured*.

Presets never touch Enable, Model Resolution, Alternate Frames, Show Advanced, or the
runtime-only comparison aids, so switching one never moves the frame's cost or hides a
comparison the user set up.

### Behaviour

Selecting a preset writes every row above. Style, Local Tone/Structure, Skin Structure,
Automatic Mask and Intensity are latched when Feature 18 is created, so the backend's
debounced `SettleTuning` recreate picks the change up on its own - nothing in `ApplyPreset`
tears the feature down.

`MatchesPreset()` compares every row **except Placement and NR Intensity**, with a 1e-4 float
tolerance, and runs once per frame while the menu is open. Those two are basic controls a user
is expected to move without leaving the preset: a Vanilla-Plus moved to Finished Image is
still Vanilla-Plus - luminance-only with a +-1-stop guard on the finished frame - and the
Legacy proxy simply has no effect there. `Max Ratio` only counts while the guard is on, and
`Band Radius` only while Broad and Detail differ, so a stale, hidden stored value cannot make a
preset look modified. Anything else shows
"`<preset>` (modified)" in the combo with a **Reset to preset** button beside it. This is
deliberately not a third "Custom" entry, which would lose track of which look the settings
came from.

Loading a config reads the stored values and never re-applies the preset, so modified settings
survive a restart. `RestoreDefaultSettings()` is `ApplyPreset(Full)` plus hiding Advanced.

### Layout

**Show Advanced Settings** (`Settings::showAdvanced`, saved, off by default) splits the tab.
Always visible: Enable, the comparison screenshot button, Preset (+ Reset to preset),
Placement, Model Resolution and its scale(s), Alternate Frames, NR Intensity, and Split Screen
with its position slider. Split Screen stays out of Advanced on purpose - it is how anyone
judges whether the edit is an improvement at all.

Behind Advanced: NR Style, Local Tone/Structure/Skin, Automatic Mask, Proxy Curve,
Color/Transfer/Broad/Detail strengths, Band Radius, Ratio Guard and Max Ratio, the
per-category trees, Depth-Aware Silhouette, Frame Hold, and the whole Debug group.

### Settings migration

New keys are `preset`, `showAdvanced`, `proxyCurve`, `broadLuminosity`, `detailLuminosity` and
`bandRadius`; `luminosityStrength` is dropped. `LoadSettings` reads which keys the config
actually carries *before* assigning it over the defaults, and:

- **no `preset` key** (a config from before presets): `preset = Full` and every stored value
  kept, so nobody's look changes silently and the combo shows "Full (modified)" wherever it
  differs - including the old Skin Color Strength of 1.0.
- **`luminosityStrength` present, the new keys absent**: copied into both `broadLuminosity` and
  `detailLuminosity`, which is the same edit; the old key is simply not written again.
- **no `showAdvanced` key**: defaulted to **true** when the loaded values differ from Full, so
  someone who tuned things before Advanced existed still sees their sliders, and false
  otherwise.
- `proxyCurve` is clamped below `kHdrLinear`: that one is never a stored choice.

The feature ini is `1-1-0`.

## Proxy curve

`Proxy Curve` (`Settings::proxyCurve` -> `TransferParams.ProxyCurve` ->
`NeuralDisplayTransform::proxyCurve`) chooses how the scene-linear placements build the image
the model sees. It has no effect on Finished Image, whose proxy is the finished frame itself,
and the combo is greyed out there. That holds even when HDR Display hands Finished Image a
scene-linear frame: `EvaluateFinishedImage` forces Display-matched (the identity transform)
unless the HDR path's HDR Linear override is in force.

| Value | What the model sees | Used by |
|---|---|---|
| Display-matched (0) | The ISHDR replica, or the ACES fallback when grading cannot be captured | Full |
| Neutwo (1) | Exposed scene linear through Open Shaders' `NeutwoEncode` - `c * rsqrt(peak^2 + 1)`, one hue-preserving scale, identity at black | option |
| Legacy (2) | The 2026-09-09 proxy: per-channel Reinhard `c / (1 + c)`, no exposure, no Linear Lighting decode | Vanilla-Plus |
| HDR Linear (3) | Exposed scene linear, float16, no curve and no clamp | never chosen by hand |

HDR Linear is not offered in the combo. It is selected at runtime by `ResolveProxyCurve()`,
and only while HDR Display is loaded *and* redirecting the framebuffer - the same test
Finished Image already uses to pick its colour domain - and only once the Model Contract probe
below has been run and its HDR contract enabled. While it is in force the combo shows
"HDR Linear (HDR Display)", disabled; the stored curve is not changed, so both presets return
to their own value as soon as HDR Display is off.

### Model space

How the proxy is *encoded* for the model is a separate question from which curve built it, and
`NeuralModelSpace(domain, proxyCurve, vanillaGrading)` answers it from constants alone, so the
encode and the decode always agree without either reading the adaptation textures:

- **HDR Linear** -> linear: no encode, and - unlike the 0-1 spaces - no `saturate` on the way
  back either, because the model was handed open-ended light and its answer is read in the
  same units.
- **display gamma**, or **Display-matched with vanilla grading** -> plain 2.2.
- everything else -> piecewise sRGB.

The 2.2 case is a fix. With Linear Lighting on, ISHDR encodes its SDR output as `pow(x, 1/2.2)`
followed by `FrameBuffer::ToSRGBColor` (`pow(x, FrameParams.x)`, the in-game gamma setting).
The replica encoded with piecewise sRGB instead, which lifts the proxy's shadows relative to
the frame actually on screen. It now uses 2.2, the same treatment Finished Image already gets.
`FrameParams.x` is not captured, so a non-default in-game gamma is still a small residual that
`Transfer Strength` corrects. Neutwo and Legacy keep sRGB: that is what the builds they
reproduce used.

## Model contract probe

Feature 18 has always been created here with **no creation flags and no selectors**. The other
DLSS Neural Rendering projects do set some: Open Shaders creates with
`IsHDR | DoSharpening | AutoExposure` (0x61) and writes `Hdr=1`, `SDR=0`, `AutoExposure=1`,
unit pre-exposure and exposure scale, and `Sharpness=0` every frame; DLSS5VKLayer sets
`DoSharpening | AutoExposure` always and `IsHDR` only when the DLL advertises it, and on its
HDR path sends float16 linear light with 1.0 at paper white, unbounded.

If Feature 18 accepts white-point-normalised linear light, the scene-linear placements can hand
the model exposed linear colour and the ISHDR replica and ACES fallback stop being needed at
all. Open's documentation asserts the HDR selectors do *not* permit unbounded input but cites
no test; DLSS5VKLayer sends it anyway. Neither has published an image-quality comparison, so
this has to be measured, and the machinery to measure it is what shipped here.

**Model Contract** (Advanced -> Debug, session-only, never saved) selects one of:

- **A - Current**: no creation flags, no selectors. What every released build does.
- **B - SDR + Auto Exposure**: creation flags `DoSharpening | AutoExposure`, with `DLSSNR.SDR=1`,
  `DLSSNR.Hdr=0`, `DLSSNR.AutoExposure=1`, `DLSSNR.InPreExposure` / `DLSSNR.InExposureScale` /
  `DLSS.Pre.Exposure` / `DLSS.Exposure.Scale` all 1.0, and `Sharpness=0`.
- **C - HDR**: B plus `IsHDR`, `DLSSNR.Hdr=1`, `DLSSNR.SDR=0`.

B and C deliberately do **not** add `MVLowRes`, so each step differs from the one before it in
exactly the thing being measured. (The plan this came from assumed contract A already set
`MVLowRes`; it does not - that flag is only ever set on Separate Upscaling's *private DLSS-SR*
feature, never on Feature 18. The 2026-09-09 build set no flags either, so Vanilla-Plus is
already faithful on this point and there is nothing to test.)

The flags are written under both `DLSS.Feature.Create.Flags` and `DLSSNR.Feature.Create.Flags`,
because Feature 18's own name for them is undocumented and setting a key nothing reads costs
nothing. `DoSharpening` with `Sharpness` at zero should be a no-op; stating it every frame is
what makes that true rather than assumed. The contract is part of `NeuralRenderingNGX::Tuning`,
so changing it goes through the same debounced recreate a Style change does.

**Hand the Model Linear Light** (the same Debug group) is the input variant: contract C plus
HDR Display active switches the proxy to HDR Linear. In a scene-linear placement that is
exposed scene light with no curve; on Finished Image over the HDR redirect it is the float16
frame passed through without `NeuralHighlightRolloff`. Either way the answer is read back as
linear in the input's units, and the luminance ratio itself is unchanged, so results stay
directly comparable across contracts.

### DLL identity

`Runtime::Probe` logs the runtime's file name, version, application id and API version, and
whether the DLL exports `NVSDK_NGX_D3D12_GetFeatureRequirements`. When it does,
`Runtime::LogFeatureRequirements` calls it once for Feature 18 on the adapter the private D3D12
device runs on and logs the result. The DLL's **SHA-256** follows on its own `[DLSSNR]` line:
the file is over 100 MB and Probe runs again on every runtime initialisation, so it is hashed
on a worker thread, once per file per session.

That query reports support, minimum GPU architecture and minimum OS version. It does **not**
report which creation flags the feature would accept - `NVSDK_NGX_FeatureRequirement` has no
such field, in either the D3D12 or the Vulkan form - so it cannot answer the HDR question on
its own. The A/B/C probe is what answers it.

The hash matters because output channel order is known to differ between builds carrying the
same 310.8 version (Open found a `.bgr` swap producing a full-frame blue cast). **Swap Output
R/B** in the Debug group swaps red and blue in the answer before the resolve reads it, so an
obviously miscoloured frame can be identified as such. Unknown builds are not refused the way
Open refuses them: that locks users out of DLLs that work.

## Debug views and readback

All of these live in `NeuralRendering::DebugState`, not `Settings`, so none of them is saved.

**Show Broad Band** / **Show Detail Band** render one half of the luminance edit on its own:
mid-grey where the model asks for no change, black and white at two stops down and up. In a
scene domain the value is divided by the proxy's exposure first, the same trick the split-screen
divider uses, so it still reads as mid-grey once the frame is tonemapped. Only available while
Broad and Detail differ - there are no separate bands otherwise - and mutually exclusive.

**Show Guard Clamping** tints every pixel the ratio guard actually caught: red where it stopped
the model brightening a pixel, blue where it stopped it darkening one, blended 0.6 over the
normal image so what is being clamped stays readable. With the guard off, nothing is marked.

**Measure Model Output Peak** reads back the brightest luminance the answer carried. Together
with the guard view it feeds one line in the settings tab: model peak and the share of sampled
pixels the guard clamped. `DecodeColorCS` accumulates both into a four-`uint`
`RWStructuredBuffer` at `u1` - two `InterlockedAdd`s and one `InterlockedMax` on `asuint` of the
luminance, which is monotonic over non-negative floats - from **one pixel per 8x8 block**. That
sampling keeps the atomic traffic on a single address bounded (about 130k samples at 4K) while
still being far more than a diagnostic needs; it is an estimate, not a census. The buffer is
copied into a three-deep ring of staging buffers and mapped three frames later with
`D3D11_MAP_FLAG_DO_NOT_WAIT`, so nothing stalls, and the counters are cleared before the decode
that fills them. The whole path is skipped unless one of the two toggles is on.

A peak above 1.0 means the model is answering outside the 0-1 range it was trained on - which
is the single most useful thing to know about a contract change.

## What still has to be measured

Everything above is implemented and compiles; none of it has been run in game. These are the
tests the design depends on, in the order they matter.

### Model contract (Step 0)

Run each on a held frame (Frame Hold now works at After Upscaling as well as Finished Image).
Contract C must be tested with HDR Display enabled and redirecting.

1. **Accepts**: creation and evaluation result codes for A, B and C.
2. **Output encoding**: with Intensity, Local Tone and Local Structure at 0, does output equal
   input? Under HDR Linear this shows whether the answer comes back linear in input units.
3. **Range**: scale the held input's highlights x2 and x4 above paper white. Does the output
   keep their structure or clip at 1? (Model Output Peak answers this directly.)
4. **Exposure invariance**: feed the held frame at x0.25, x1 and x4 exposure and compare the
   mean and spread of the resolved log-ratio map, under B with the 0-1 proxy as well as under C
   with linear light. If the edit barely moves, the proxy does not need to match the game's
   exposure at all - this is the direct answer to whether the flags spare the early proxies.
5. **Temporal stability**: live, static camera, 300 frames; mean frame-to-frame absolute change
   in log ratio per contract.
6. **Look**: split screen against contract A at the same placement.

Decision gate:

| Result | Action |
|---|---|
| Linear light passes tests 2-5 | Make HDR Linear Full's scene-linear default while HDR Display is active, on contract C. Display-matched and ACES become legacy options. |
| Only test 4 passes, with the 0-1 proxy | Keep the 0-1 proxy but drop exposure matching; Neutwo without exposure becomes Full's scene-linear default. |
| Nothing improves | Keep contract A and Display-matched, as shipped. |

Note one precision caveat for linear light in a scene-linear placement: the shared proxy and
answer textures mirror `kMAIN`'s format, which is usually `R11G11B10_FLOAT`. That holds
positive values well above one, but with less precision than float16. Finished Image over the
HDR redirect is float16 and unaffected.

### Vanilla-Plus fidelity

Build `5947cf63` as the reference with the same `nvngx_dlssnr.dll`. Fix the scene in both
builds - same save, `set gamehour`, `fw`, `tfc`, `tai`, a fixed camera, Linear Lighting in the
same state - and capture NR off and NR on at After Upscaling in each, using the matched
on/off screenshot feature. Compare `log2(on / off)` per pixel between the two builds.
**Pass: mean absolute difference under 0.02 stops, with no structured differences.** Repeat
once with Linear Lighting off, since Legacy has to reproduce that build's encoding behaviour.

### Presets and migration

Selecting each preset writes every row of the table and triggers one feature recreate. Editing
any Advanced control shows "(modified)"; Reset to preset clears it; changing Placement, scale,
Alternate Frames or Intensity does not. Settings and the modified state survive a restart.
Upgrading a config with `luminosityStrength` 0.7 gives Broad = Detail = 0.7 and an identical
log-ratio map. Restore Defaults gives Full with Advanced hidden.

### Luminosity split

Broad = Detail = x matches the pre-change build at Luminosity x on the same ratio-map test, and
the profiler shows no `NeuralRendering::ToneBands` event. Broad 0 / Detail 1 keeps texture and
contrast with no region-level relighting; Broad 1 / Detail 0 the reverse - check both with the
band views. Detail 2 at high-contrast edges (branches against sky, torches in interiors) should
show no halos at the default edge sharpness. Band Radius should cover the same screen area at
0.5x and 1x model scale. Alternating Frames, Frame Hold and Split Screen all still work with
the split active.

### Proxy curve and the Linear Lighting fix

Each curve at Before, After and Separate Upscaling; the combo greyed out on Finished Image.
With Linear Lighting off, After Upscaling, Display-matched: capture the proxy texture in
RenderDoc next to the finished frame - their brightness and contrast should now roughly match,
where before the fix the proxy is visibly washed out. The proxy's shadows should match the
finished frame at the default in-game gamma and at one non-default value. The DLL SHA-256
should appear in the log, and Swap Output R/B should produce a full-frame colour swap,
confirming the current build is already RGBA.

### Ratio guard A/B

This decides whether Vanilla-Plus keeps the 2026-09-09 build's Max Ratio 2.0 (+-1 stop) or moves
to Open's 4.0 (+-2 stops). Use Show Guard Clamping and the clamped-pixel percentage across a
midday exterior, a torch-lit interior, and a night or cave scene, with the same fixed-scene
setup as the fidelity test. Run Vanilla-Plus at Max Ratio 2.0, 4.0 and guard off on the same
held frame, then live for stability. Measure the clamped-pixel percentage, the 300-frame
stability metric, and a split-screen comparison of each run against guard off.

Starting thresholds, adjustable: if 2.0 clamps more than about 5% of pixels in normal scenes,
or visibly flattens shadows the model is clearly placing on purpose, move to 4.0. If 4.0's
stability metric is more than about 20% worse than 2.0's, keep 2.0. If both hold, prefer 2.0
for fidelity.

### Performance

Record the NR timers for every pass at 1440p and 4K, at 1x and 0.5x model scale, with the split
off and on. The tone passes should cost well under the model evaluation; if they do not, drop
to 9 taps per pass before reaching for a downsampled blur.

## Deferred

Two items from the same design are deliberately not implemented.

**Hejl-Burgess-Dawson replica.** When vanilla uses HBD (`Param.z > 0.5`) the replica runs HBD on
luminance only, while ISHDR's real path is `DisplayMapping::HuePreservingHejlBurgessDawson` - an
adaptive desaturation in ICtCp, per-channel HBD, then bloom. Porting that means lifting its
`PSHADER && BLEND` include guard or copying it. Deferred until after the contract probe: if
linear light or exposure invariance wins, the replica stops being the default and this stops
mattering.

**ACES fallback scaling.** Narkowicz's ACES fit expects input pre-scaled by about 0.6.
Unscaled it brightens midtones and crushes near-black in the proxy (0.18 -> 0.55 against
Neutwo's 0.46; 0.01 -> 0.05 against 0.10, as sRGB-encoded proxy output). Adding the 0.6 changes
Full's look on Post Processing and Effects11 setups, so it needs a split-screen check first;
Neutwo is available as the alternative in the meantime.

## Model tuning parameters

`DLSSNR.Intensity` / `Style` / `LocalToneStrength` / `LocalStructureStrength` /
`SkinStructureStrength` / `UseAutoMask` / `Hint.Render.Preset` are **latched when
the feature is created**. Writing them only at evaluate does nothing; `Runtime`
sets them at create time only (`Runtime::Execute`).

`NeuralRenderingBackend::State::SettleTuning` (`Backend.cpp`) debounces a changed
value the same way `SettleModelRaster` debounces a resolution-scale drag: once a
requested `NeuralRendering::Tuning` has been stable for `kTuningDebounceFrames`
(12) and actually differs from what is latched into the live handle, `Run()`
drains the interop queue (`D3D12Interop::WaitForIdle`), calls
`Runtime::ResetFeature()`, and lets the next `Runtime::Execute()` recreate the
feature with the new values - the same GPU-idle path a model-raster change
already uses, never a bare `release()` while the interop queue might still
reference the handle. A slider drag therefore settles on its own within about a
fifth of a second at 60 FPS; toggling Neural Rendering off and on is no longer
necessary.
