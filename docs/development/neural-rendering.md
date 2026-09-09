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

Runs in `Upscaling::PerformUpscaling()` after the colour upscale and before
`UpscaleDepth()`, on the display-resolution upscaled frame, before RCAS
sharpening / copy-back. Depth and motion therefore remain the exact
render-resolution guides used for the colour upscale. The backend tracks the
colour region and guide region separately (`FrameInputs::guideWidth/guideHeight`).

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
clamps the ratio to `0.5..2.0`, and applies that one scalar to the untouched scene
colour. The original hue, HDR headroom, and alpha remain renderer-owned, while
the model contributes bounded local shading/detail. No temporal accumulator or
midpoint blend is involved; every frame is independently re-anchored.

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
