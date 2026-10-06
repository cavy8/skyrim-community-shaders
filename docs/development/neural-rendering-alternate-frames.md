# Neural Rendering alternate-frame verification

Alternate Frames evaluates the neural model at half the rendered frame rate. The
game still supplies fresh color on every frame. Incorrect temporal edits can make
motion look uneven even when presentation intervals stay at 16.67 ms.

The old backend multiplied the current one-frame motion by two on evaluations
after a skip. This assumes constant velocity at the same screen coordinate, which
fails for camera turns, acceleration, and third-person character animation. The
skipped-frame resolve also tested only luminance, allowing an old foreground edit
to survive on a newly exposed background of similar brightness.

The backend now captures raw motion, depth, and material category after each
successful alternate-mode frame. A following evaluation composes current motion
with the skipped frame's motion at the reprojected position, accounting for the
previous guide raster's jitter. Skipped resolves reject out-of-bounds history and
depth/category mismatches, and compare RGB instead of luminance alone. Missing
captures force evaluation; incomplete motion chains reset model history. Raw
model diagnostics and Frame Hold evaluate every frame.

The history uses one RGBA32F texture at guide resolution (about 31.6 MiB at
1920x1080), one inexpensive capture pass each frame, and one composition pass on
evaluations after a skip. Depth rejection is a conservative relative reciprocal
depth check, rather than a full reconstruction of moving geometry. Fast changes
in distance can reduce the edit on a skipped frame. Newly inferred edits still
update at half rate; this mode cannot reproduce every-frame inference exactly.

## Automated checks

From a Visual Studio x64 developer prompt at the repository root:

```bat
BuildDevFast.bat
cl /nologo /EHsc /std:c++17 tools/test-neural-temporal.cpp /Febuild/test-neural-temporal.exe /Fobuild/test-neural-temporal.obj /link d3d11.lib d3dcompiler.lib
build\test-neural-temporal.exe
```

The standalone test executes the production capture, composition, and decode
shaders on D3D11 WARP. It checks acceleration, a direction reversal, sampling the
previous field along the motion trajectory, guide jitter, depth/category changes,
offscreen and non-finite motion, equal-luminance color changes, and fresh-frame
passthrough of the enhancement. Both conventional and Reverse Z are tested.
An optional first argument selects another decode shader for regression comparison.

Also prepare shaders and validate the NeuralRendering shader directory according
to [the shader workflow](shader-workflow.md), including Reverse Z permutations.

## In-game comparison

1. Install the rebuilt DLL and all Neural Rendering shaders together. Clear the
   compiled shader cache if needed so the new transfer layout and passes are used.
2. Lock presentation to 60 FPS. In third person, run sideways past a high-contrast
   wall or foliage, rotate the camera around the player, then reverse direction.
   Compare Alternate Frames off/on with all other settings held fixed.
3. Watch the player's silhouette, hands, hair, and newly exposed background.
   Old edits should no longer trail onto newly exposed surfaces. Inspect motion
   while turning as well as running at a constant speed.
4. Repeat at native and reduced model resolution, before and after upscaling,
   Finished Image, and Separate Upscaling. Test Reverse Z on/off across restarts.
5. Change style or placement, toggle Alternate Frames, resize the model raster,
   and enter/leave Frame Hold. The first evaluation must be fresh, with no stale
   history flashes. Raw model output should update every frame.
6. Record presentation times and GPU cost. Compare visual motion independently
   of the FPS counter; the neural model itself still updates at 30 Hz at 60 FPS.

WARP checks validate transfer logic, not NVIDIA's opaque model history or the
in-game animation/render hook ordering. Those require this runtime comparison.
