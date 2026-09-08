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

### After Upscaling (`neuralRenderingPlacement == 1`, default)

Runs in `Upscaling::PerformUpscaling()` after `Upscale()` + `UpscaleDepth()`, on
the display-resolution upscaled frame, before RCAS sharpening / copy-back. The
colour input is display resolution; **depth and motion vectors are still the
game's render-resolution targets**, so each resource carries its own NGX subrect
and the backend tracks the colour region and the guide region separately
(`FrameInputs::guideWidth/guideHeight`).

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

## The one genuinely distinct third mode: Neural Rendering as the upscaler

Feature 18 exposes `DLSSNR.Upscaling` / `DLSSNR.Scale` / `DLSSNR.ScalingRatio` and
belongs to the same family as Ray Reconstruction, which replaces DLSS SR when
active - so it can plausibly produce a display-resolution frame from a
render-resolution input in one pass. That, not a repositioned post-process, is
what "inside the upscaler" (OptiScaler's `DualFeature`) actually means.

Integration recipe, if it is validated on hardware:

1. Add placement value `2` = "Instead of upscaler".
2. In `Upscaling::Upscale()` for `kDLSS` + placement 2, **skip `streamline.Upscale`**.
3. Call NR with colour input = render-resolution `main.texture` (render-resolution
   colour subrect), guides = render resolution, **output extents = display
   resolution**.
4. In `Runtime::Execute` for this path, create the feature with
   `DLSSNR.Scale` / `DLSSNR.ScalingRatio` = display/render, `DLSSNR.Upscaling = 1`,
   `Output*` / `OutputSubrect*` = display, `ColorSubrect*` = render.
5. `neuralRenderingTexture` (display resolution) becomes the upscaled frame:
   set `neuralRenderingResultValid = true` so `ApplySharpening()` consumes it.
6. Drop the DLSS-only reactive / transparency-mask inputs (feature 18 does not
   consume them); keep the motion-vector copy.

**Not wired today because it cannot be verified here.** If feature 18 does not
produce an acceptable *upscale* (as opposed to 1:1 detail synthesis), placement 2
is a broken render path with no way to catch it without an NVIDIA GPU running a
compatible `nvngx_dlssnr.dll`. It must be A/B'd against DLSS SR on real hardware
before it ships.

## Colour domain

`ColorTransfer.hlsli` maps the linear open-ended HDR scene colour into the
display-referred (tone-mapped + sRGB) domain the model was trained on, and its
exact inverse on the way out, so a model that returns its input unchanged leaves
the frame untouched. The current operator is a per-channel Reinhard curve plus the
sRGB transfer function. NVIDIA/RenoDX's own DLSS-5 composition uses a soft-knee
proxy and a measured white point instead of a plain curve, and re-anchors the
model's answer as a luminance *ratio* against a kept copy of the original rather
than replacing the colour; that is the proven-steady form and the place to look if
highlights still shimmer.

## Model tuning parameters

`DLSSNR.Intensity` / `Style` / `LocalToneStrength` / `LocalStructureStrength` /
`SkinStructureStrength` / `UseAutoMask` / `Hint.Render.Preset` are **latched when
the feature is created**. Writing them only at evaluate does nothing. `Runtime`
sets them at create time; changing a slider therefore needs the feature rebuilt,
which today means toggling Neural Rendering off and on. A debounced in-place
rebuild (drain the interop queue, `ResetFeature()`, recreate) is a follow-up - it
must go through the same GPU-idle path `EnsureResources()` uses, never a bare
`release()`.
