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

## Motion vectors

Both placements pass the game's **raw** motion-vector target
(`RE::RENDER_TARGETS::kMOTION_VECTOR`) to `NeuralRendering::Evaluate`, not the
`motionVectorCopyTexture` that `EncodeTexturesCS` produces for DLSS.

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

`ColorTransfer.hlsli` maps the linear open-ended HDR scene colour into the
display-referred (tone-mapped + sRGB) domain the model was trained on. The model
answer is **not** inverse-Reinhard decoded: that inverse has an unbounded slope
near white and turned tiny output changes into severe HDR flicker. Instead the
resolve compares model and proxy luminance, adds a shared `1/512` shadow floor,
and clamps the ratio to `0.5..2.0`. A hue-preserving scalar Reinhard proxy then
allows the model's complete RGB chromaticity to be rescaled onto that guarded
scene luminance without recolouring a model no-op. `Color Strength` blends from
stable renderer chroma at zero to the full model palette at one, fading only in
near-black pixels where normalized colour is numerically ambiguous. HDR
headroom and alpha remain renderer-owned. No temporal accumulator or midpoint
blend is involved; every frame is independently re-anchored.

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

## Model tuning parameters

`DLSSNR.Intensity` / `Style` / `LocalToneStrength` / `LocalStructureStrength` /
`SkinStructureStrength` / `UseAutoMask` / `Hint.Render.Preset` are **latched when
the feature is created**. Writing them only at evaluate does nothing. `Runtime`
sets them at create time; changing a slider therefore needs the feature rebuilt,
which today means toggling Neural Rendering off and on. A debounced in-place
rebuild (drain the interop queue, `ResetFeature()`, recreate) is a follow-up - it
must go through the same GPU-idle path `EnsureResources()` uses, never a bare
`release()`.
