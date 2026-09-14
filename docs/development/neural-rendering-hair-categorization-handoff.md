# Neural Rendering hair categorization — handoff

Update 2026-09-13 (third follow-up): categories no longer share bits with vertex
AO. `Masks2` is `R16G16_UNORM`, with upstream's AO in R and the category in G.
Upstream's AO, blending and effect writes are restored; see
[Third follow-up](#third-follow-up-categories-separated-from-ao). Its in-game
result is unverified.

Parts of the [second follow-up](#second-follow-up-decal-hair-was-erased) still
apply: the whole-frame capture scope, the head-part changes and the TruePBR grass
category. Its blend-state and `Effect.hlsl` changes are superseded.

Everything before the second follow-up is historical. Its premise, that hair mods
author hair pieces with non-hair-tint shaders, did not hold for the NPC that was
actually inspected.

Original status as of 2026-09-13: fix implemented and committed (`Initial hair
categorization improvements`, `d135e72`) on the `Personal` branch. Compiles
and links; **not yet verified in-game**. This doc is the handoff for whoever
picks this up next (including a future Claude session) — read it before
touching `NeuralRenderingCategories`, `Upscaling::BSLightingShader_SetupNeuralCategory`,
or the `Masks2` write path in `Lighting.hlsl`.

## Symptom

With Neural Rendering restricted to the Hair category, a hairline showed three
visually distinct bands instead of one: the roots read as Skin, a middle band
read as Everything Else despite being 100% opaque hair, and only the top
section read as Hair. The category debug view (`Show Material Categories` in
the Neural Rendering settings) later showed the same failure on hair pieces
generally — most notably a whole braid drawn on top of a shirt reading as
something other than Hair, regardless of what was behind it. This was initially
interpreted as ruling out blending. That conclusion was too strong: additive or
multiplicative blending can corrupt a packed category even at full opacity;
see the follow-up below.

Two independent root causes were found and fixed, in that order. Both were
real; fixing the first did not fix the symptom on its own (confirmed by the
user testing in-game between fixes).

## Root cause 1 — forward-pass draws never wrote Masks2 at all

`Masks2` (R16_UNORM, render target 7) packs vertex AO and the Neural Rendering
material category together. It's written by `Lighting.hlsl`, but only the
`DEFERRED` permutation's `PS_OUTPUT` struct declared it. Alpha-blended
lighting geometry — hair strands and hairline scalps chief among them — is
sorted and drawn *after* the deferred opaque pass and its composite, through
the non-`DEFERRED` forward permutation, with the deferred render targets
unbound. That permutation had no `Masks2` output slot, so those draws
contributed nothing: whatever category the opaque geometry beneath them had
wound up in the snapshot every Neural Rendering evaluation reads. Roots over
the face read Skin; the mid-section over background or mismatched geometry
read Everything Else; only the alpha-tested (non-blended) core of the hair
mesh was drawn in the deferred pass and read correctly as Hair.

### Fix: three-stage Masks2 capture pipeline

- [`Lighting.hlsl:320`](../../package/Shaders/Lighting.hlsl#L320) — the
  forward `PS_OUTPUT` struct now also declares `float4 Masks2: SV_Target7`.
- [`Lighting.hlsl:3362-3422`](../../package/Shaders/Lighting.hlsl#L3362) — the
  category-classification block (previously nested only inside
  `#if defined(DEFERRED)`) now runs unconditionally, and both permutations
  write `Masks2` with the same stochastic 0/1 coverage dither the normals use
  (`masks2Coverage`), so a fractional-alpha draw takes one surface's exact
  packed value instead of lerping two discrete category ids together.
- [`Upscaling.cpp:2026`](../../src/Features/Upscaling.cpp#L2026) —
  `CaptureNeuralRenderingCategories()` (existing, pre-decal snapshot) now also
  clears `neuralRenderingForwardCaptureActive` at the top of a new frame.
- [`Upscaling.cpp:2069`](../../src/Features/Upscaling.cpp#L2069) — new
  `RestoreNeuralRenderingCategories()`, called from
  [`Deferred::EndDeferred`](../../src/Deferred.cpp#L438) right after the
  deferred composite (which reads the decal-blended AO from the live
  `Masks2`) has run. Copies the pre-decal snapshot back into the live
  `Masks2` target and arms `neuralRenderingForwardCaptureActive`.
- [`Upscaling.cpp:2098`](../../src/Features/Upscaling.cpp#L2098) — new
  `BSBatchRenderer_RenderPassImmediately` hook (installed in `PostPostLoad`
  at the engine's per-render-pass draw entry point, the same mechanism
  `TerrainBlending`/`InteriorSun`/`LightLimitFix` already use). While the
  forward capture is armed, for eligible passes only (main world view,
  Lighting shader type, not reflections, slot-0 target matches what
  `Deferred::EndDeferred` restored), it rebinds `Masks2` to slot 7 around the
  individual draw call, then unbinds it again.
- [`Upscaling.cpp:2085`](../../src/Features/Upscaling.cpp#L2085) — new
  `FinishNeuralRenderingCategoryCapture()`, called at the top of
  `Main_PostProcessing` (`Upscaling.cpp:2901`), after all world/first-person
  geometry is done. Re-snapshots `Masks2` (now containing both opaque and
  forward categories) and disarms the forward capture.

AO is untouched by this: the composite still reads the live `Masks2` (with
its decal-blended AO) before the restore runs, and the restore only replaces
it afterward for the category snapshot's purposes.

## Root cause 2 — category classification itself was wrong for hair

Independent of the pass-ordering bug above, the classification logic had two
gaps that a correct forward write would not fix:

1. **Hair was recognized only by shader technique.** The `HAIR` `#define`
   only compiles for material authored with the hair-tint shader type. Hair
   mods routinely author hairlines, braids, and loose strands with the
   Default or Skin Tint type instead. Those pieces compiled as Equipment (if
   flagged humanoid), Skin (skin-tint permutation), or Everything Else —
   matching exactly what the debug view showed.
2. **The humanoid flag was only set during the deferred pass.** Before this
   fix, `BSLightingShader_SetupNeuralCategory` gated its whole body on
   `deferred->deferredPass`. Once forward draws started writing categories
   (root cause 1's fix), they ran with the humanoid flag always cleared,
   which routed skinned forward pieces into the creature-body Skin branch and
   rigid ones into Everything Else.

### Fix: runtime `IsHair` flag, from two independent signals

New `ExtraShaderDescriptors::IsHair` flag
([`State.h`](../../src/State.h), bit 12; mirrored in
[`Permutation.hlsli`](../../package/Shaders/Common/Permutation.hlsli)), set
per-pass by
[`BSLightingShader_SetupNeuralCategory`](../../src/Features/Upscaling.cpp#L1986)
(now updated to also run whenever `neuralRenderingForwardCaptureActive`, not
only during the deferred pass) from either of:

- **Shader authoring** — [`IsHairTintShader`](../../src/Features/Upscaling.cpp#L1950):
  the pass's material is hair tint, or its shader property carries the hair
  soft-lighting flag. This is the same material/flag pair
  `CharacterRainSurfaces::IsCharacterRainSurfaceCompatible` uses to exclude
  hair. It needs no actor lookup, so it also catches **wigs worn as
  equipment** — geometry with no head part to match, authored with hair
  shading but attached like armor.
- **Head part ownership** — [`IsHairHeadPartGeometry`](../../src/Features/Upscaling.cpp#L1965):
  walks up from the drawn geometry to its ancestor sitting directly under the
  actor's skinned face node (`Actor::GetFaceNodeSkinned()`), then matches
  that ancestor's node name against the NPC's hair/facial-hair head parts —
  including their `extraParts`, which is how a hairline attaches to a hair
  head part. This catches hairlines/braids/strands authored with a
  non-hair-tint shader type.

[`Lighting.hlsl:3408`](../../package/Shaders/Lighting.hlsl#L3408) — after the
existing technique/flag classification chain, `IsHair` overrides the result
in every permutation except `HAIR` itself (which is already correct).

## Files touched

| File | What changed |
|---|---|
| `package/Shaders/Lighting.hlsl` | Forward `PS_OUTPUT` gains `Masks2`; category block runs unconditionally; new `IsHair` override branch |
| `src/Deferred.cpp` | `EndDeferred()` calls `RestoreNeuralRenderingCategories()` after the composite |
| `src/Features/Upscaling.h` / `.cpp` | New capture-pipeline methods, new `BSBatchRenderer_RenderPassImmediately` hook, `BSLightingShader_SetupNeuralCategory` rewritten with the two hair signals |
| `src/State.h`, `package/Shaders/Common/Permutation.hlsli` | New `IsHair` bit (12), kept in lockstep per this repo's convention (see `porting-from-open-shaders` internal note — new flags append sequentially after `AdditiveLighting`, `IsEye` stays pinned to the reserved high bit) |
| `docs/development/neural-rendering.md` | Design doc updated: forward-pass capture pipeline, and the `IsHumanoidActor`/`IsHair` runtime-resolved categories |

Note: the committed diff also includes pre-existing uncommitted work from
before this investigation started — `DecodeColorCS.hlsl`,
`NeuralRenderingCategories.hlsli`, `NeuralRendering.h/.cpp`, `Backend.h/.cpp`,
and `en.json` changes related to the category debug view and per-category
color handling. That work was not authored or reviewed as part of this
investigation; it predates it and was already in the working tree.

## Verification performed

- fxc compiles clean for: deferred default, deferred `FACEGEN_RGB_TINT`,
  forward default, forward `HAIR` permutations of `Lighting.hlsl` (all with
  `-D PSHADER`, plus `SKINNED`/`VC` as applicable).
- `clang-format --dry-run` reports no changes needed on the touched C++
  files.
- `BuildDevFast.bat` full incremental build succeeds and links
  `CommunityShaders.dll` (checked after each round of changes).

## Not verified — do this next

- **No in-game or RenderDoc testing of this exact fix.** The user's earlier
  in-game test only ruled out the original (uncommitted, since-replaced)
  stochastic-blend-only attempt; the forward-capture pipeline and the
  `IsHair` flag have not been observed running.
- Turn on `Show Material Categories` in the Neural Rendering settings and
  check: a hairline against a face, a braid against clothing, and ideally a
  wig if one is available to test with.
- **Multi-shape hair NIFs**: `IsHairHeadPartGeometry` assumes every shape in
  a head part's NIF sits as a direct descendant reachable by walking parents
  up to the child of the face node bearing the head part's editor-ID name.
  If a hair NIF nests shapes under an intermediate node with a different
  name that never gets renamed to the head part's editor ID, or a shape
  isn't skinned under the face node at all, this walk fails silently (falls
  through to the technique-based classification). If a braid or strand still
  misclassifies after this fix, that's the first place to check — a quick
  log of `partRoot->name` versus the NPC's head part editor IDs would
  confirm or rule this out immediately.
- **First-person geometry**: the `BSBatchRenderer_RenderPassImmediately`
  hook's eligibility check requires the slot-0 render target to match what
  `Deferred::EndDeferred` restored; this should include first-person forward
  draws, but hasn't been specifically confirmed on a first-person hair/wig
  case (helmet hair clipping, etc.).
- Overlay head parts (`npc->HasOverlays()` branch in `IsHairHeadPartGeometry`)
  are checked before the base head parts; this hasn't been tested against an
  actor actually wearing a head-part overlay (e.g. some hair-overlay mods).

## Follow-up investigation

The deployed Personal `Lighting.hlsl` and DLL matched the prior source shader
and `build/ALL/Release/CommunityShaders.dll` by SHA-256. The regression is not
explained by stale files in that mod folder, although a higher-priority mod
override would still require checking the active virtual filesystem.

Two gaps were found in the capture implementation:

- The capture hook intercepted only the `RenderBatches` call site. Light Limit
  Fix already intercepts three immediate-pass dispatch sites, so the category
  hook now follows that same template pattern and SE-only third-site guard.
- The forward target inherited the material's colour blend rule. Binary alpha
  does not protect packed values from additive or multiplicative operations.
  `Deferred::ResetBlendStates(true)` now selects lazily cached forward blend
  variants made by `src/Features/Upscaling/CategoryBlend.h`. RT7 uses
  source-alpha/inverse-source-alpha blending, or an opaque overwrite when RT0
  is opaque. RT0-6 retain their effective original settings. At capture finish,
  the original blend table is restored. Deferred/decal blending is unchanged.

`tools/test-neural-category-blending.cpp` is a standalone D3D11 WARP regression
test using the production blend helper and HLSL packing code. Its baseline
reproduces additive Hair (2) over Equipment (6) decoding as Everything Else (0).
The patched test passes 1,764 cases covering both coverage endpoints, all seven
source/destination categories, three AO pairs, opaque/blended draws, and
independent/shared native states with additive, multiplicative, or standard
alpha colour blending. It also checks that RT0-6 descriptors are preserved.

Verification in this follow-up: the Dev-Fast DLL compiled and linked; shader
preparation through `cmake --build build/ALL --target prepare_shaders` succeeded;
fxc compiled deferred default, deferred skin-tint, forward default, and forward
hair Lighting pixel shaders. The initial Dev-Fast reconfigure attempted a
blocked PowerShell download; configuring that existing build with
`VCPKG_MANIFEST_INSTALL=OFF` allowed it to use its installed dependencies.
The test DLL is `build/Dev-Fast/CommunityShaders.dll`; it has not been deployed
to the Personal mod folder.

This reproduces a concrete corruption mechanism, **not the precise draw in the
user's screenshot**. No current frame capture or actor geometry inspection was
available in this session. If the screenshot persists after testing this DLL,
inspect pixel history on the black strand: confirm RT7 is bound, inspect its
blend state and shader output, then inspect `IsHair` and the geometry/head-part
ownership. A failed head-part lookup remains possible; it was not changed here.

Manual verification: with Show Material Categories enabled, revisit Anoriath
at the same angle and inspect the strand over his shirt and the bangs over his
forehead, then move the camera to distinguish temporal coverage from stable
black patches. Also check opaque hair, clothing, an alpha hairline over skin,
and Neural Rendering disabled. Only real transparency should expose the
underlying surface category. The screenshot's exact failure is not yet verified
as resolved in-game.

## Second follow-up: decal hair was erased

The user reported black strands still appeared with the blend-state patch, and
that a top-down view rules out transparency as the explanation. Inspecting the
actual assets instead of assuming how hair is authored changed the diagnosis.

**Evidence** (read-only, from the user's MO2 Default profile). Anoriath's FaceGen
head (`mods/NPC Output/meshes/.../FaceGeom/Skyrim.esm/00013B97.nif`) and his head
parts (`NPC.esp` override, CoDB/CotG resources):

| Shape | Head part type | Shader | Flags and alpha |
|---|---|---|---|
| `000CotG_AnoriathHair` | Hair | HairTint | alpha test 77 |
| `000CotG_AnoriathHLExtra02` (braids) | Misc, extra of hair | HairTint | alpha test 130 |
| `000CotG_AnoriathHL` (hairline) | Misc, extra of hair | HairTint | Decal + DynamicDecal, alpha blend |
| `000CoDB_HumanBeard02` | FacialHair | HairTint | Decal + DynamicDecal, alpha blend |
| `000CoDB_BrowsMaleHumanoid09` | Eyebrows | HairTint | Decal + DynamicDecal, alpha blend |
| `000CoDB_EyeLashesTintedM` | Misc, extra of eyes | HairTint | alpha blend |
| `000CotG_AnoriathHLExtra03` (earrings) | Misc, extra of hair | EnvMap | alpha test |
| `000CotG_AnoriathScars`, `...LeftScar` | Scar, Misc | SkinTint | Decal + DynamicDecal, alpha blend |

Every hair piece is hair tint, so the `HAIR` technique already classified it.
The FaceGen shapes are direct children of `BSFaceGenNiNodeSkinned`, each named by
its head part editor ID, so the head-part walk resolved as well. Vanilla
`HairLine*` head parts in `Skyrim.esm` are Misc-type extra parts of their hair.

**Root cause.** The hairline, beard and brows are decals with alpha blending. They
draw in the engine's blended-decals pass inside the deferred pass, not through
the forward path the capture hook covers. `CaptureNeuralRenderingCategories`
snapshotted `Masks2` just before that pass, and `RestoreNeuralRenderingCategories`
copied the snapshot back after the composite. That erased every decal's category
and exposed whatever was beneath (face, clothing, or an Everything Else pixel).
The original "roots read as Skin" symptom is this. Separately, the extra-parts
rule tagged the environment-mapped earrings as Hair.

**Fix (working tree).**

- The capture spans the whole frame: `BeginNeuralRenderingCategoryCapture` in
  `Deferred::StartDeferred`, `FinishNeuralRenderingCategoryCapture` at the start of
  post-processing. There is no pre-decal snapshot and no restore.
- Deferred blend states get the same category-safe RT7 variant as forward ones
  (`MakeCategoryBlendDesc`, renamed from `MakeForwardBlendDesc`), so decals write
  their category where they cover.
- `Effect.hlsl` writes zero `Masks2` alpha while `ExtraFlags::NeuralCategoryCapture`
  (bit 13) is set. With decals no longer undone, an effect's fractional alpha would
  otherwise lerp categories. Frames without the capture keep upstream behaviour.
- `RunGrass.hlsl`'s TruePBR grass path packs Foliage. It wrote no category, which
  reads as Everything Else and plausibly explains the black ground in the
  top-down screenshot.
- The head-part hair signal skips accessory materials (environment map, glow
  map, parallax, multilayer parallax, eye) and matches each ancestor up to the
  face node's child.
- With Show Material Categories on, `LogNeuralCategoryDraw` logs each actor
  geometry once per stage: `deferred`, `forward`, or `forward, Masks2 unbound`.

**Not explained by static analysis.** Nothing found writes Everything Else onto
Anoriath's braids over his shirt: the erased-decal path exposes the surface
beneath, which there is Equipment. If black persists with this build, enable the
debug view, reproduce, and read the `Neural category draw` lines for that actor
in `CommunityShaders.log`:

- A strand listed only as `forward, Masks2 unbound` means an unhooked dispatch
  site.
- `hair false` on a hair shape means classification.
- No line at all for the shape means a non-lighting writer.

**Verification.** `BuildDevFast.bat` built and linked
`build/Dev-Fast/CommunityShaders.dll` (not deployed). fxc compiled `Effect.hlsl`
(deferred, deferred `MULTBLEND`, forward), `RunGrass.hlsl` (`GRASS_LIGHTING` with
and without `TRUE_PBR`), and `Lighting.hlsl` (deferred, forward, deferred
`HAIR`/`SKINNED`) pixel shaders. The WARP test,
`tools/test-neural-category-blending.cpp`, was rebuilt against the renamed helper
and passed 1,764 checks, with the baseline still reproducing the additive
corruption. No in-game or RenderDoc verification.

## Third follow-up: categories separated from AO

The user asked for no change to upstream functionality. The capture still changed
it in three ways:

- categories took the low three bits of vertex AO (since `54b528be5`);
- the deferred pass blended AO with binary coverage instead of the material alpha
  (`d135e72` and the second follow-up);
- `Effect.hlsl` dropped its `Masks2` alpha while capturing.

A texture of its own was ruled out. The deferred pass binds all eight D3D11 render
targets, so a separate target needs every lighting draw issued a second time.

**Design (working tree).**

- `Masks2` is created as `R16G16_UNORM`.
  - R is upstream's vertex AO, written with upstream's values, alpha and
    blending. The composite still reads `.x`.
  - G holds the category (`NeuralRenderingCategories::Encode`: id / 255, exact in
    16-bit unorm). `DecodeColorCS` reads G from the snapshot.
- The deferred blend table enables G only for unblended entries
  (`GetDeferredMasks2WriteMask`). Opaque and alpha-tested geometry writes both
  channels in one draw, and blended draws blend AO exactly as upstream.
- `Upscaling::RenderDeferredPass` redraws blended deferred lighting passes into G
  alone. For the redraw:
  - the redraw blend table masks every other target and channel;
  - a variant of the first draw's depth-stencil state tests inclusively and
    writes neither depth nor stencil;
  - `ExtraFlags::NeuralCategoryRedraw` (bit 13, replacing `NeuralCategoryCapture`)
    makes `Lighting.hlsl` output binary coverage as `Masks2`'s alpha.

  A pass is redrawn only when its lighting draw ran, which
  `BSLightingShader_SetupNeuralCategory` records, and its bound blend state
  blends RT0.
- Forward draws write G only (`MakeForwardCategoryBlendDesc`).
- `Effect.hlsl` is back to upstream. Grass writes unpacked AO in R.
- `TerrainBlending::RenderTerrainBlendingPasses` routes its blended terrain loop
  through `RenderDeferredPass`. Upscaling's hook is installed after Terrain
  Blending's at the shared call site, so the passes Terrain Blending defers and
  draws later never reach it. Without this, near terrain would lose Landscape.

**Cost.** Only while the capture is armed:

- one extra lighting draw per blended deferred pass (decals, plus near terrain
  with Terrain Blending on);
- a 364-entry blend table swap around each redraw;
- an `OMGetBlendState` per deferred dispatch.

Separately, `Masks2` grows by one 16-bit channel whether or not Neural Rendering
runs.

**Risks to check in-game.**

- The redraw binds its depth-stencil state directly, as Terrain Blending does. If
  the engine rebinds its own state during the redraw, and that state writes depth
  with a strict test, the redraw fails and the pass keeps the category beneath.
- Blended decals must dispatch through the three hooked call sites. With Show
  Material Categories on, each redrawn shape logs a `deferred, category redraw`
  line. A hairline, brow or beard with only a `deferred` line got no category.
- The hooks further down the chain run a second time for a redrawn pass.
  - Terrain Blending cannot queue a pass twice: a pass it defers never drew, so it
    is never redrawn.
  - Light Limit Fix's `CheckParticleLights` and Interior Sun's cull-mode override
    were not audited beyond their thunks.

**Verification.**

- `BuildDevFast.bat` built and linked `build/Dev-Fast/CommunityShaders.dll` (not
  deployed).
- fxc compiled these pixel shaders, plus `DecodeColorCS.hlsl`:
  - `Lighting.hlsl`: deferred, deferred `HAIR`/`SKINNED`, deferred
    `FACEGEN_RGB_TINT`, forward, forward `HAIR`;
  - `RunGrass.hlsl`: `GRASS_LIGHTING` with and without `TRUE_PBR`, and basic;
  - `Effect.hlsl`: deferred, deferred `MULTBLEND`, forward.
- `Effect.hlsl` has no diff against `HEAD`.
- The rewritten WARP test passed 6,615 checks:
  - the deferred table's AO and RT0 match an upstream `R16_UNORM` `Masks2` bit for
    bit, across opaque, alpha, premultiplied, additive and multiplicative
    blending;
  - the category is never blended;
  - the redraw and forward states change only G.

  Its baseline still reproduces additive Hair over Equipment storing 8.
- clang-format reports nothing on changed lines.
- No in-game or RenderDoc verification.

## Design doc

`docs/development/neural-rendering.md` has been kept in sync with all of the
above — read it for the full category model (Skin/Hair/Eyes/Foliage/
Landscape/Equipment/Everything Else), the `Masks2` channel layout, and the
debug-color view. This handoff doc is meant to be deleted or folded into that
file once the fix is confirmed working in-game; it exists for now because the
design doc describes the intended, steady-state architecture and shouldn't
carry "not yet verified" caveats.
