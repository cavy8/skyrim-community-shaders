# Hair Strands

Converts hair card meshes into strands at runtime and draws them inside the hair's own
lighting pass. Personal feature, added 2026-09-28 (Alpha, `0-1-0`). As of that date it is
build- and shader-compile-verified and its converter is unit-tested on synthetic cards; it has
**not** been run in game yet. The first in-game session should work through
[Unverified assumptions](#unverified-assumptions).

## Pipeline

| Stage | Where | Thread | When |
| --- | --- | --- | --- |
| Classify: is this geometry hair? | `StrandRenderer::Classify` | render | first draw of a geometry |
| Resolve style (file → preset → editor override) | `StrandRenderer::ResolveStyle` | render | first draw, and after style files or settings change |
| Copy mesh (bind pose, weights, UVs) | `MeshExtract.cpp` | render | once per hair and style |
| Generate strands | `StrandGenerator.cpp` | worker (`std::async`, 2 at a time) | once per hair and style, shared by every actor |
| Upload asset | `StrandRenderer::BeginFrame` | render | when the job finishes |
| LOD, bone palette, skinning compute | `UpdateLod`, `Skin` | render, in `SetupGeometry` | first lighting draw of the hair each frame |
| Draw ribbons | `StrandRenderer::Draw` | render, in `RestoreGeometry` | every main-view lighting draw of the hair |

Everything lives in `src/Features/HairStrands.{h,cpp}` (feature, settings, UI) and
`src/Features/HairStrands/` (the `Strands` namespace). Shaders are in
`features/Hair Strands/Shaders/HairStrands/`.

## Why it is built this way

-   **Draw inside the hair's pass, with the hair's own pixel shader.** Hooks on
    `BSLightingShader` vtable slots 6 (`SetupGeometry`) and 7 (`RestoreGeometry`) bracket
    the card draw. Just before `RestoreGeometry`, every texture, sampler, constant buffer,
    render target and blend state for the hair is still bound. `StrandLighting.hlsl` is
    compiled with the *same defines* as the hair's permutation (from
    `ShaderCache::GetDefinesString(shader, modifiedPixelDescriptor)`), so its pixel shader is
    `Lighting.hlsl`'s. Deferred output, motion vectors, Hair Specular, Hair Backlighting and
    Light Limit Fix all apply to strands with no per-feature work. Only VS/PS, input layout,
    topology, VS `b7`/`t0-t2` and the rasterizer state are swapped, and all of them are put
    back exactly.
-   **Skin from the bone palette, not the card vertices.** Each strand point gets the
    barycentric blend of its triangle's bone weights (top four, unorm8). A compute shader
    applies `boneWorld × skinToBone`, the same transform the game skins the cards with. SMP
    and other physics move bones, so strands follow them for free. Positions are camera
    relative (`posAdjust`); previous positions use last frame's palette and `previousPosAdjust`.
-   **Game objects are only read inside the two hooks.** At that point the geometry, skin
    instance and bone nodes are guaranteed alive. Instances are keyed by `BSGeometry*` but
    never dereferenced elsewhere, and re-validated by skin instance and vertex count because
    addresses get reused.
-   **Hiding cards costs nothing.** Replace mode sets the shadow state's viewport to 1×1
    at (30000, 30000) for the card draw, so every card fragment is clipped before the pixel
    shader. It works for opaque, alpha-tested and blended hair. A `discard` in
    `Lighting.hlsl` was rejected: it would have cost early-Z on every opaque permutation.
    Cards still cast the shadows, because the shadow passes use the Utility shader, which is
    not hooked.
-   **Procedural style in the vertex shader.** The generator makes the low-frequency shape
    (flow, clumps, volume) as 4–32 control points per strand. The VS adds curls, coils, waves
    and frizz analytically on a Catmull-Rom spline. That keeps memory small, makes those
    fields update live in the editor, and lets LOD drop curve detail with distance.

## Conversion algorithm (`StrandGenerator.cpp`)

1. **Weld** positions (1/1000 unit), with a normal-direction bucket so the two sides of a
   double-sided card stay separate. Drop exact duplicate back faces.
2. **Flow** per triangle = ∂P/∂V from the UVs (or ±U/±V when set), projected into the
   triangle. With `flowAxis: auto`, each connected piece is flipped so its flow runs away
   from the skull centre (the `NPC Head` bone + 5 units up) and, on balance, downwards.
   Real hair UV V only loosely tracks root→tip (KS/Apachii: correlation with height about
   -0.55 to -0.7), hence the per-piece vote rather than a fixed sign.
3. **Trace** streamlines across welded edge adjacency, following barycentrically
   interpolated vertex flow. A strand stops at a boundary, at a fold (neighbour normals
   more than about 100° apart), or where the flow reverses.
4. **Seed.** Roots are placed along boundary edges the flow enters, `density` per unit of
   width across the flow (85% of the 40k strand cap). A fill pass adds whole streamlines
   through any triangle the roots under-visited. `seeding: auto` switches to area seeding
   (short strands scattered over the surface) when the median strand is under 1 unit.
5. **Resample** every strand to one point count per asset: the 95th-percentile length
   divided by `segmentLength`, clamped to 4–32 points. Then lift points off the surface
   (layer jitter plus `volume` towards the tip), clump by root grid cell within a
   connected piece (pull plus optional twist), and shuffle. Because of the shuffle, any
   prefix of the strand list is an even thinning, which is what LOD draws.

## LOD and budget

-   Per frame, `BeginFrame` ranks last frame's hair (the player first, then the nearest).
    It allows up to `MaxActors`, and scales strand counts to fit `MaxStrandsPerFrame`.
    Everything else keeps its cards.
-   Per hair: fraction = lerp(1, `MinStrandFraction`, smoothstep(`LodStart`, `LodEnd`, distance))
    × `DensityScale` × budget share. Width is scaled by 1/fraction (up to `MaxWidthScale`)
    so coverage stays about the same. Where that is still under `MinPixelWidth`, the
    fraction drops further, because the VS widens thin strands to the pixel floor anyway.
    Past `LodEnd` (with 5% hysteresis) the cards come back.
-   Curve detail (render points per control segment) comes from the curl or wave period
    up close and falls to 1 at `LodEnd`, capped by `MaxSubdivisions`.
-   The compute pass skins only the drawn prefix. Cost scales with drawn strands × points.

## Authoring styles

The loader reads `Data/SKSE/Plugins/CommunityShaders/HairStrands/*.json` in name order,
then `UserStyles.json` (the in-game editor's file) last. The most specific matching entry
wins, and later entries win ties. Match fields are optional and case-insensitive:
`headPart` (editor ID), `model` (relative to `Meshes`) and `shape` (node name). An entry
with no `match` applies to all hair.

```json
{
    "version": 1,
    "styles": [
        {
            "match": { "headPart": "HairFemaleNord01" },
            "preset": "curly",
            "mode": "replace",
            "curlRadius": 0.3,
            "excludeUV": [[0.0, 0.0, 0.25, 0.1]]
        },
        { "match": { "model": "actors\\character\\hair\\mybeads.nif" }, "enabled": false }
    ]
}
```

-   A style resolves as the preset's defaults, then every field the entry sets. Every value
    is clamped (`Strands::Sanitize`). Malformed files and entries are logged and skipped.
-   The global **Convert** setting chooses between *All hair (automatic)* and *Authored
    styles only*. With the second, hair with no entry keeps its cards. `"enabled": false`
    excludes a hair in either mode.
-   `preset: auto` (the default) guesses from the head part, model and shape names:
    `afro`/`coil`/`kink`/`4c` → coily, `locs`/`dread`/`braid`/`twist`/`cornrow` → locs,
    `curl`/`ringlet` → curly, `wave`/`wavy` → wavy, anything else → straight.
-   Fields: generation (`seeding`, `flowAxis`, `density`, `segmentLength`, `lengthScale`,
    `volume`, `layerJitter`, `clumpStrength`, `clumpSize`, `clumpTwist`, `shortLength`,
    `seed`, `excludeUV`) and render (`rootWidth`, `tipWidth`, `waveAmplitude`, `waveLength`,
    `curlRadius`, `curlLength`, `curlStart`, `frizz`, `flyaways`). Tooltips in the editor
    explain each field. Units are Skyrim units, about 1.4 cm.
-   In-game editor: select a hair in view, edit it, and the change applies to every actor
    wearing it. Render fields apply live; generation fields apply when the slider is
    released. **Save** writes the fully resolved style, matched on that exact head part,
    model and shape, to `UserStyles.json`.

## Hair types

| Preset | What it changes |
| --- | --- |
| Straight | defaults: light clumping, little frizz |
| Wavy | per-lock sine waves (period 4) |
| Curly | helical curls (radius 0.35, period 1.6), strong clumping, so locks spiral together as ringlets |
| Coily | tight coils from the root (radius 0.18, period 0.45), little clumping (a cloud rather than ringlets), high volume and frizz, denser and thicker strands so the scalp does not show |
| Locs | clump pull 0.95 with twist: strands collapse into twisted ropes (locs, braids, twists) |

Short hair (buzz cuts, fades, fuzz) is covered by area seeding, not a preset. Long hair just
gets more control points, up to 32. For dark hair, keep the cards (Hybrid): thin strands over
a light background show the gaps between them most.

## Shared-file hunks

Both are in `Lighting.hlsl`, both are additive, and both are gated on `HAIR_STRANDS`. Only
`StrandLighting.hlsl` defines that, so card permutations compile unchanged.

1. After the non-landscape base colour and normal sample: strands take the card colour
   (mip bias +1) with alpha 1 and a flat normal.
2. After the double-sided TBN flip: strands restore the unflipped TBN, because a
   camera-facing ribbon has no back side.

`StrandLighting.hlsl` also `#undef`s `DO_ALPHA_TEST`, which only gates discards in
`Lighting.hlsl`, and supplies its own VS, compiling `Lighting.hlsl` with `VSHADER` undefined.
Registration is the usual `Feature.cpp` / `Globals` append. There is no `FeatureData` or
permutation bit.

## Verifying changes

-   Build: `./BuildDevFast.bat`.
-   Shaders: `StrandLighting.hlsl` is not in `.github/configs/shader-validation.yaml`, so
    compile it by hand with fxc as VS and PS. Use `-D HAIR -D DO_ALPHA_TEST` plus the
    Lighting feature defines, with and without `DEFERRED`/`SKINNED`. The strand VS output
    signature must match the strand PS input signature.
-   Converter: `StrandGenerator.cpp` only needs `float3` and friends plus `logger`. It
    builds on its own with a small shim (SimpleMath, a `logger` stub, `RE::BSGeometry`
    declared) and synthetic cards. That is how the 2026-09-28 checks ran: root/tip
    orientation with flipped V, double-sided dedupe, weight normalisation, every preset,
    area seeding and UV exclusion. Eight generations take 0.25 s.

## Unverified assumptions

Check these first in game:

-   Each draw happens between slots 6 and 7 with its state still bound (FrameAnnotations
    assumes the same), and `RestoreGeometry` still has the pass's pixel shader constants bound.
-   `partitions[i].buffData->rawVertexData` is kept on the CPU after load. RaceMenu's body
    morphs rely on this. `triList` holds global indices in SSE; local ones are mapped
    through `vertexMap` when detected.
-   The skinning convention is `boneWorldTransforms[b] × skinToBone(b)`, with no extra
    root-parent transform. Strands offset from the cards would point here.
-   Setting `viewPort` + `DIRTY_VIEWPORT` in the shadow state reaches `RSSetViewports` on
    the card draw.
-   The previous-frame convention (relative to `previousPosAdjust`) matches the game's
    skinned motion vectors. A mismatch would show as ghosting on moving hair with TAA or DLSS.

## Not done (candidates)

-   Strand shadow maps and self-shadowing beyond Hair Specular's, and deep opacity maps.
    Cards cast the shadows.
-   Strand simulation of its own. Strands inherit bone and SMP motion only.
-   Wigs have no model path in their key (no head part), so they match on shape name and
    vertex/triangle count.
-   Actor fade-out keeps the cards: strands have no alpha to fade with.
-   Model-space-normal hair permutations keep their cards.
