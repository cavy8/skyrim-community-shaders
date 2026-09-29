# Hair Strands

Converts hair card meshes into strands at runtime and draws them inside the hair's own
lighting pass, in place of the cards. Personal feature, added 2026-09-28 (Alpha).

The first in-game run (2026-09-28, `0-1-0`) showed three faults, fixed in `0-1-1`:

-   Brows, lashes, beards and hairlines were converted too. They carry the hair-tint
    material, and the old classification fell back to it for any head part. See
    [What converts](#what-converts).
-   Strands were drawn over the cards (the default mode was Hybrid). Strands now always
    replace the cards, and the cards come back only as the fallback: past the strand
    distance, over the budget, while generating, or when the actor fades.
-   Strands did not match the hair's shape. The generator filled the whole card geometry,
    but cards are mostly transparent: tapered tips, gaps between locks, and whole empty
    regions of beard and brow meshes. Strands now follow the texture's alpha. See
    [Conversion algorithm](#conversion-algorithm-strandgeneratorcpp).

The second run (`0-1-1` before the depth fix) drew the strands (the statistics counted them)
but showed the hair as a flat, sky-coloured card silhouette, the same look as a hair
permutation that is still compiling. Two causes:

-   Alpha-tested hair has a depth prepass (Utility shader), and its lighting pass tests depth
    for equality against it. No strand fragment off the exact card surface passes, so no
    strand was ever visible on alpha-tested hair. That was already true in the first run,
    where only the blended brows and beards showed strands.
-   The cards were hidden in the lighting pass only. Their prepass depth stayed, so the
    deferred composite shaded the card silhouette from an empty G-buffer.

The strands now draw with their own depth state, and the cards are hidden from the depth
prepass as well. See [Why it is built this way](#why-it-is-built-this-way).

The third run (`0-1-1`) showed five faults, fixed in `0-1-2`:

-   Some hair exploded into huge, long strands (up to 40,000 strands averaging 60-90 units).
    `MeshExtract` read positions as half floats unless `VF_FULLPREC` was set. SSE always
    stores float positions, and plain `BSTriShape` hair (flags `0x5b`) does not carry the
    flag, so every attribute of those meshes was read from the wrong bytes. Positions are
    now always floats, and a layout that disagrees with the descriptor's size fails the
    conversion.
-   Criss-crossing sheets of strands: the same cause (strands across garbage triangles).
-   Strands through heads. Clumping averaged point *k* of every strand in a clump, but point
    *k* sits at a different distance on strands of different length, and that average lies
    inside the skull. On KS Simonne, clumped points up to 5.5 units off the hair surface.
    The width-sized depth nudge also lifted strands on cards tucked under the scalp through
    the skin.
-   Distant shadows over the hair. The sun and shadow-light shadows come from a
    screen-space shadow mask built from the depth prepass, and with the cards gone from the
    prepass the mask at hair pixels held the shadow of whatever was behind the hair.
-   Thin, see-through hair. The pixel-width floor counted output pixels, not rendered
    ones, so with DLSS strands were narrower than a rendered pixel. The exploded hairs also
    took 40,000 strands each out of the per-frame budget, which thinned the rest.

The first `0-1-2` build, run the same day, showed vanilla Nord hair as ladders of short
strands over sky-coloured holes:

-   Holes: the prepass hook drew strand depth only while `inWorld` was set, but the depth
    prepass (`Main_RenderDepth`) runs before the world pass sets it. The cards kept their
    prepass depth while the lighting pass hid them, so the head behind failed its depth test
    and only strands just in front of the card surface drew. The instance itself now marks
    the world's hair.
-   Stubs (median strand 1.1 units). Vanilla's atlas lays one strip sideways (hair along U),
    and every strand on those cards ran across the strip. And vanilla hair is fully
    double-sided with front and back copies interleaved: keeping the first copy of each
    triangle left each side a checkerboard of isolated triangles, so every strand ended
    within one. KS Simonne lost length the same way at bands shared with another sheet's
    back (median 2.9 units). See [Conversion algorithm](#conversion-algorithm-strandgeneratorcpp).

The second `0-1-2` build fixed the hair but broke the world: terrain vanished, and other
objects dropped out or drew in the wrong order. With ReverseZ on, the depth prepass binds a
flipped depth test (`GREATER_EQUAL`). The strand draw read the pass's states back through
ReverseZ's read-back hooks, which return the unflipped ones, and then restored those raw.
That left `LESS_EQUAL` bound while the game and ReverseZ both still took the flipped state
as bound, so every later prepass draw tested depth backwards and wrote none. Terrain
Blending draws terrain from the prepass depth, so the terrain disappeared. The lighting
pass had escaped until then only because the hair's `EQUAL` test reads the same either
way. The states are now read with the passthrough on, as bound.

This is build-verified, and the generator fixes are checked on real meshes (see
[Verifying changes](#verifying-changes)). Work through
[Unverified assumptions](#unverified-assumptions) first.

## Pipeline

| Stage | Where | Thread | When |
| --- | --- | --- | --- |
| Classify: is this geometry hair? | `StrandRenderer::Classify` | render | first draw of a geometry |
| Resolve style (file → preset → editor override) | `StrandRenderer::ResolveStyle` | render | first draw, and after style files or settings change |
| Copy mesh (bind pose, weights, UVs) | `MeshExtract.cpp` | render | once per hair and style |
| Copy one mip of the diffuse texture to a staging texture, map it once the GPU is done | `BeginCoverageReadback`, `PollCoverageReadback` | render | once per hair and style, a frame or two before generation |
| Decode the texture's alpha (any format, BC included, via DirectXTex) | `DecodeCoverage` | worker | start of the generation job |
| Generate strands | `StrandGenerator.cpp` | worker (`std::async`, 2 at a time) | once per hair and style, shared by every actor |
| Upload asset | `StrandRenderer::BeginFrame` | render | when the job finishes |
| LOD, bone palette, skinning compute | `PrepareStrands` (`UpdateLod`, `Skin`) | render, in `SetupGeometry` | first pass of the hair each rendered frame (depth prepass or lighting) |
| Draw strand depth | `StrandRenderer::Draw` (depth only) | render, in the Utility `RestoreGeometry` | every Utility draw of the hair that writes depth (the depth prepass) |
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
-   **Hiding cards costs nothing.** For a card draw that strands replace, the shadow
    state's viewport moves to 1024×1024 at (30000, 30000), so every card fragment is clipped
    before the pixel shader. It works for opaque, alpha-tested and blended hair. A `discard`
    in `Lighting.hlsl` was rejected: it would have cost early-Z on every opaque permutation.
    The strands are drawn with the card's own viewport. It is read back from the one the game
    applied, so a dynamic resolution scale applied there is kept. If the hidden viewport never
    reaches the draw, the log says `Hiding the cards did not reach the draw` once.
-   **The depth prepass gets the strands' depth, not the cards'.** Alpha-tested hair writes
    its depth in a Utility shader prepass (`RenderDepth`) before the lighting pass. The
    screen-space shadow mask (sun and shadow lights) and other screen-space passes are
    built from that depth. A hook on `BSUtilityShader` slots 6 and 7 hides the cards in
    every Utility pass with the same viewport trick. Where the pass writes depth, it draws
    the strands with the strand VS and no pixel shader, and logs each such Utility
    descriptor once (`Strand depth drawn in Utility pass`). The lighting pass then draws the
    same depth again. Shadow-map passes (`RenderShadowmap`) and reflections keep the cards,
    so cards still cast the shadows. The prepass runs in `Main_RenderDepth`, before the world
    pass sets `inWorld`. Only an instance a world lighting pass created is touched, and that
    is what marks the world's hair. Skinning runs once
    per rendered frame (`State::frameCount`, counted at Present) in whichever of the hair's
    passes comes first, so the prepass and the lighting pass use the same pose whatever
    order the feature's per-frame hooks run in. The prepass uses the strand shaders of the
    lighting permutation that last drew the hair. If this frame's permutation is still
    compiling, the lighting pass shades with those too, because the cards cannot come back
    once their depth is gone.
-   **Strands bring their own depth state.** The hair's lighting pass tests depth with
    `EQUAL`. The strand draw uses the pass's own depth-stencil state with writes on and
    `EQUAL` widened to `LESS_EQUAL`, or `GREATER_EQUAL` when the projection is reversed
    (ReverseZ; detected from the camera projection's z row). The VS also moves each ribbon
    0.02 units nearer along its view ray, so strands lying on the hairline cap do not
    z-fight it. It is a fixed distance on purpose: a nudge the size of the ribbon (which
    grows with distance and LOD) lifted strands on cards tucked under the scalp through the
    skin. All state, and the card viewport, is read, set and restored with ReverseZ's hook
    passthrough on. Its read-back hooks return the unflipped states, and an unflipped state
    restored raw stays bound behind the game's cache.
-   **Procedural style in the vertex shader.** The generator makes the low-frequency shape
    (flow, clumps, volume) as 4–32 control points per strand. The VS adds curls, coils, waves
    and frizz analytically on a Catmull-Rom spline. That keeps memory small, makes those
    fields update live in the editor, and lets LOD drop curve detail with distance.

## What converts

`StrandRenderer::Classify` runs once per geometry. A shape becomes strands only if all of
these hold:

1. It belongs to an actor and uses the hair-tint material, the one the HAIR technique (and
   so Hair Specular and Hair Backlighting) treats as hair. Beads, ties and other solid parts
   of a hair mesh keep their own material and stay as they are.
2. It has an alpha property (test or blend). Hair cards always do.
3. It is a head part of type **Hair**, or it is not a head part at all (a wig worn as
   equipment). Brows, lashes, beards and eyes are hair-tinted too, but their types are
   Eyebrows, Misc or FacialHair, so they keep their cards.

The Misc extra parts of a Hair part are either its hairline or a second layer of the same
hair. Checked on 2026-09-28 against `Skyrim.esm` and `KS Hairdo's.esp`: every hairline is a
Misc extra part, and KS "HL" parts reuse the hair's mesh (identical vertex and triangle
counts). A Misc extra part is hidden while a strand shape of the same actor with the same
counts draws strands. Otherwise it keeps its cards: a hairline is a scalp cap that stays
under the strands, where it covers the gaps between them. Two Hair shapes of one actor with
identical counts are handled the same way: the first seen becomes strands, the other is
hidden.

## Conversion algorithm (`StrandGenerator.cpp`)

1. **Weld** positions (1/1000 unit). Vertices at one position join only if their normals
   face the same way (dot above 0), so the two sides of a double-sided card stay separate
   sheets while a card that curves round the head stays one. A triangle repeated on the same
   welded vertices (a back face sharing the front's vertices) is dropped. Back faces on
   vertices of their own are dropped a sheet at a time: a sheet more than half of whose area
   repeats earlier kept sheets goes, and any other sheet is kept whole, overlap included.
   Dropping copy by copy leaves an interleaved double-sided mesh (vanilla hair lists front
   and back alternately) as two checkerboards of isolated triangles. It also cuts holes
   where a sheet shares a band with another's back. Either way, strands stop at every hole.
2. **Flow** per triangle = ∂P/∂V from the UVs (or ±U/±V when set), projected into the
   triangle. With `flowAxis: auto`, each UV island (vertices sharing a position and a UV;
   one strip of the atlas) flows along U instead when it is more than 1.5× longer that way
   on the surface. Vanilla's atlas lays one strip sideways (V 0.74–0.88): 24% of the Nord
   hair's card area and 71% of `0_td18_hair_9`'s. Each connected piece is then flipped so its
   flow runs away from the skull centre (the `NPC Head` bone + 5 units up) and, on balance,
   downwards; a piece's U and V strips vote separately. Real hair UV V only loosely tracks
   root→tip (KS/Apachii: correlation with height about -0.55 to -0.7), hence the vote
   rather than a fixed sign.
3. **Trace** streamlines across welded edge adjacency, following barycentrically
   interpolated vertex flow. A strand stops at a boundary, at a fold (neighbour normals
   more than about 100° apart), where the flow reverses, or where it re-enters a triangle
   it already crossed (other than stepping straight back): flow circling a bun or a closed
   lock would otherwise wind round to the 200-unit cap.
4. **Seed.** Roots are placed along boundary edges the flow enters, `density` per unit of
   width across the flow (85% of the 40k strand cap). A fill pass adds whole streamlines
   through any triangle the roots under-visited. `seeding: auto` switches to area seeding
   (short strands scattered over the surface) when the median strand is under 1 unit.
5. **Trim to the painted hair.** With a coverage mask (the diffuse alpha, read back at no
   more than 512 texels across), each traced streamline keeps only the run from its first
   point whose alpha reaches `coverageThreshold` to the last one before a transparent gap of
   more than 0.5 units. Roots can therefore start downstream of a transparent card edge, and
   strands end where the painted lock ends. Seeds in transparent stretches are dropped, so
   strand count follows painted area. Fill seeding skips triangles with no painted texel on
   a 4-step barycentric probe grid. Without a mask (a texture with no alpha, a failed
   readback, or `coverageThreshold` 0), the whole card counts, as before.
6. **Resample** every strand to one point count per asset: the 95th-percentile length
   divided by `segmentLength`, clamped to 4–32 points. Then lift points off the surface
   (layer jitter plus `volume` towards the tip), clump, and shuffle. A clump is the strands
   of one connected piece whose roots share a grid cell. Each member is pulled (plus
   optional twist) towards the clump's longest strand at the same distance from the root,
   so the clump centre is a real strand on the hair surface. Members whose roots leave the
   cell more than 60° away from that strand (a parting, a crown whorl) are not pulled.
   Because of the shuffle, any prefix of the strand list is an even thinning, which is
   what LOD draws.

## LOD and budget

-   Per frame, `BeginFrame` ranks last frame's hair (the player first, then the nearest).
    It allows up to `MaxActors`, and scales strand counts to fit `MaxStrandsPerFrame`.
    Everything else keeps its cards.
-   Per hair: fraction = lerp(1, `MinStrandFraction`, smoothstep(`LodStart`, `LodEnd`, distance))
    × `DensityScale` × budget share. Width is scaled by 1/fraction (up to `MaxWidthScale`)
    so coverage stays about the same. Where that is still under `MinPixelWidth`, the
    fraction drops further, because the VS widens thin strands to the pixel floor anyway.
    The floor counts rendered pixels (the dynamic-resolution height, so an upscaler's
    lower render resolution is accounted for), not output pixels.
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
    `coverageThreshold`, `seed`, `excludeUV`) and render (`rootWidth`, `tipWidth`, `waveAmplitude`, `waveLength`,
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
gets more control points, up to 32. Dark hair over a light background shows gaps between
strands most: raise `density` or the root width for it.

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
    orientation with flipped V, double-sided cards with back faces on shared and on their
    own interleaved vertices, strips along U, weight normalisation, every preset, area
    seeding and UV exclusion. Check strand *length*, not just validity: an early
    double-sided test passed while making 1.5-unit stubs. Eight generations take 0.25 s.
-   Real meshes: a small Python NIF reader dumps a skinned shape (positions, UVs, normals,
    bone weights mapped through the partition bone lists, triangles, bone names and bind
    origins) for the same harness. Measure strand lengths against the mesh's bounding box
    (looping streamlines) and each point's distance to the nearest mesh vertex (clumping or
    lift pulling strands off the hair). On 2026-09-28, KS Simonne went from 6-16 strands at
    the 200-unit cap to none (max 33.4 on a 47-unit mesh). Its worst clumped point went
    from 3.2-5.5 units off the hair to 1.8-3.4. The child hair `0_td18_hair_9` (flags
    `0x5b`) gives 2,413 strands of median length 3.6, where the game had made 40,000
    averaging 58. With the per-strip axis and per-sheet back-face drop, median strand
    lengths are 12.6 on vanilla Nord hair (was 1.1), 13.9 on `0_td18_hair_9` (was 3.6) and
    22.4 on Simonne (was 2.9). Unclumped strand points stay within 2.3 units of a mesh
    vertex. To see why strands end, count `Trace`'s stop reasons in a scratch copy.

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
    the card draw. If it does not, the log warns once and cards show under the strands.
-   The hair's depth prepass is a Utility pass that writes depth, outside shadow maps and
    reflections. The log should name its descriptor once (`Strand depth drawn in Utility
    pass`, expected to include the `RenderDepth` bit 0x2000). If that line never appears, or card
    outlines still show as flat sky-coloured shapes, the prepass is reaching neither hook.
-   The strand VS draws the same depth in the Utility prepass as in the lighting pass: it
    reads only its own `b7`/`t0-t2` and `ViewProj` from `b12`, the game's single per-frame
    buffer bound for every shader. Holes in moving hair would point here.
-   A hair switches to strands in the prepass only after its lighting pass has drawn strands
    once (the prepass needs that permutation's shaders), so expect one frame of card outline
    when it first converts.
-   The diffuse texture from `BSLightingShaderMaterialBase::diffuseTexture->rendererTexture`
    is the one the card draw samples, and its alpha is coverage. A hair whose log line says
    `strands fill the whole cards` had no usable alpha.
-   The default `coverageThreshold` of 0.3 on a mip of at most 512 texels. Too high thins
    and shortens strands; too low brings back strands over transparent card areas.
-   The actor's head parts are listed in its NPC record (`headParts`, or RaceMenu's
    overlays). A hair not listed there keeps its cards.
-   The previous-frame convention (relative to `previousPosAdjust`) matches the game's
    skinned motion vectors. A mismatch would show as ghosting on moving hair with TAA or DLSS.

## Not done (candidates)

-   Strand shadow maps and self-shadowing beyond Hair Specular's, and deep opacity maps.
    Cards cast the shadows.
-   Strand simulation of its own. Strands inherit bone and SMP motion only.
-   Wigs have no model path in their key (no head part), so they match on shape name and
    vertex/triangle count.
-   Actor fade-out keeps the cards: strands have no alpha to fade with.
-   Beards and other facial hair keep their cards. They would need their own flow and
    density rules, since beard cards lie flat on the skin.
-   Model-space-normal hair permutations keep their cards.
