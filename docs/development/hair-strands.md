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

`0-2-0` (2026-09-28) adds strand physics: guide strands simulated on the GPU, which every
other strand follows. It works on SMP and non-SMP hair alike, and SMP bone motion only
guides it. See [Physics](#physics-strandsimcshlsl). The solver was checked against a Python
port, not yet in game.

The first `0-2-0` run (Vanilla Hair Remake SMP with Sassy SnW's retexture) showed two faults:

-   Tips flew about unless stiffness was 1 (fixed in `0-2-1`). Followers turned their offset
    from the guide by the guide's bend, so the offset acted as a lever. That hair's strands
    average 3.8 units over 14 points, so a guide bends a lot per unit of offset. In the
    NumPy port at idle, a follower 2.5 units beside such a guide strayed 7× as far from its
    target as the guide and stretched 35%. A follower twice the guide's length strayed 12×
    as far. Followers now take the guide's displacement from its target. See
    [Physics](#physics-strandsimcshlsl).
-   Black spots at strand tips and dashes inside the hair (fixed in `0-2-2`). Strands took
    the texture colour without its alpha, and 86% of that retexture's transparent texels
    are black. Of the other textures checked, KS Hairdos' fill them with hair colour, and an
    Apachii one is dark there too. Tips taper into transparency, and strands cross
    transparent gaps between painted locks. Strands now have their own colour texture with
    those texels filled. See [Why it is built this way](#why-it-is-built-this-way).

The second run (`0-2-2`) showed:

-   Tips still twitched, on every hairstyle, with SMP or without (fixed in `0-2-3`). Dynamic
    follow-the-leader damping fed each length correction back into the velocity of the
    point before it. Under a steady load (any head tilt, or wind) those corrections never
    stop. Together with the local shape constraint, which turns each segment by its
    parent's current direction, that made the strand flutter. In the NumPy port, with the
    head held still and tilted, 69 of 224 cases (4 lock shapes, the 5 presets and 2 slider
    extremes, 8 tilts) never came to rest and moved up to 2 units a frame. A 3.8-unit lock
    shook at a 5° tilt. Without that damping none did, at 30, 60, 144 and 240 fps. Velocity
    relative to the target now loses 0.2 per 1/60 s instead. See
    [Physics](#physics-strandsimcshlsl).
-   Dirt and Blood's overlays drew in the shape of the hidden cards (fixed in `0-2-4`). Its
    spells play effect shaders (11 `EFSH` records in its plugin). An effect shader on an
    actor sets `effectData` on each shape's shader property, and the engine draws that
    shape again with `BSEffectShader`. Only the Lighting and Utility passes were hooked.
    Effect passes over hair that draws strands are now hidden. See
    [Why it is built this way](#why-it-is-built-this-way).

The third run (`0-2-4`) had calmer hair at rest, but the smallest movement sent strands flying,
"like a sine wave" (fixed in `0-2-5`). The solver check passed, because it only simulated one
20-unit lock. A NumPy port of a whole head of scalp locks (256 guides, 14 points each, median
3 units long, like Vanilla Hair Remake's) showed two faults:

-   Short hair swung as far as long hair. Stiffness fell from `rootStiffness` to
    `tipStiffness` along each strand's own length, but a load (air drag while walking, the
    head's own acceleration, wind, gravity as the head tilts) moves points about the same
    distance on any strand. After one step aside every tip ended about 1 unit off target,
    whatever its length. Locks under 2 units swung 56% of their length (median) and 2-4-unit
    locks 31%. Walking off and stopping, 76% and 41%. The bend piled up in the last few points,
    so the tips hooked over (neighbouring segments near the tip 22-37° apart, 2-3° at rest).
    Stiffness now falls over 20 units of strand, or the whole strand if it is longer. A short
    lock has the stiffness of the same length of long hair near its root, and strands of 20
    units or more keep the stiffness they had.
-   Tips flicked back and forth for about a second after every swing, on hair of any length.
    The length and local shape constraints move only each segment's far point, so a swing's
    corrections ran on down the strand as a wave that reached the tip and came back: after
    one step aside, the 20-unit lock's tip turned back 12 times. Without the local shape
    constraint the flicking was 20× smaller. Moving both ends of each segment stopped it too,
    but then a heavy steady load had no resting shape: on a head on its side (anyone lying
    down), locks with short segments jittered by up to 0.5 units a frame. The constraints are
    unchanged; instead the bending motion is damped (see [Physics](#physics-strandsimcshlsl)).
    The flicking is now 50× smaller. Only velocity is damped, so under any load hair comes to
    rest in the shape it would without the damping.

With both, one step aside swings those locks 7% and 3% of their length (medians), and walking
off and stopping 12% and 6%.

After the fourth run (`0-2-5`) the owner found the defaults too springy and asked for damping
0.4 and root stiffness 0.4 (`0-2-6`). `damping` was air drag on world velocity. At 0.4 a run
blew a 20-unit lock 76% of its length off target, and a fringe over the forehead 102%, back
into the head (NumPy port, 350 units/s). `damping` now takes out swings: velocity relative
to the target, in place of the fixed `RelativeDamping` of 0.2. Air drag is a fixed 0.06. See
[Physics](#physics-strandsimcshlsl). Styles saved before `0-2-6` hold an air drag value in
`damping` (0.04-0.18): re-save them or their swings settle more slowly than the presets'.

The same run showed hair going into the head while walking, and the front hair flying about
(fixed in `0-2-7`). The head sphere is fitted inside the hair, so it lies well inside the
forehead, face and back of the head. In the NumPy port a fringe pressed back by the walk went
0.26-1.64 units into an ellipsoid head before the sphere stopped it, and with world drag at
0.4 it went 2.5 units in and jittered. Only guides collided, and followers take their guide's
displacement, so strands nearer the scalp than their guide went in further. Collision now uses
the actor's own head mesh (see [Physics](#physics-strandsimcshlsl)), for guides and for every
strand point: the fringe stays out of the head at 0.00 units.

KS Hairdos looked semi-transparent from some directions (fixed in `0-2-8`). Each KS hair is
two copies of one mesh: the Hair part, alpha **blended** (`NiAlphaProperty` 0x12ED, test 40),
and a Misc "Hl" part, alpha **tested** only (0x12EE, test 200). The Hl copy was hidden as a
layer, and the strands drew in the blended pass. Blended geometry is drawn forward, after the
deferred passes, and has no depth prepass, so the prepass held whatever lay behind the hair:
the `0-1-2` "distant shadows over the hair" fault, which the strand prepass depth had fixed
only for alpha-tested hair. The Hl copy's own passes now draw the strands (see
[Why it is built this way](#why-it-is-built-this-way)), so KS hair takes the same path as
vanilla hair.

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
| Decode the texture's alpha (any format, BC included, via DirectXTex) and fill its colour for strands | `DecodeCoverage` | worker | start of the generation job |
| Generate strands, pick guide strands, fit the head collider | `StrandGenerator.cpp` | worker (`std::async`, 2 at a time) | once per hair and style, shared by every actor |
| Upload asset (strands, colour texture) | `StrandRenderer::BeginFrame` | render | when the job finishes |
| Read the actor's head mesh into the head field | `BuildHeadField` | render, in `SetupGeometry` | once per actor and hair asset, with physics and collision on |
| LOD, bone palette, colliders, simulation and skinning compute | `PrepareStrands` (`UpdateLod`, `Skin`, `PrepareSimulation`) | render, in `SetupGeometry` | first pass of the hair each rendered frame (depth prepass or lighting) |
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
    topology, VS `b7`/`t0-t2`, PS `t0` (lighting draws) and the rasterizer state are swapped,
    and all of them are put back exactly.
-   **Strands have their own colour texture.** A strand has no alpha, but its texture
    coordinates cross transparent texels: gaps between painted locks, and the tapered tips.
    Their colour is whatever the artist left there, black in many hair textures. The
    generation job builds a copy of the read-back mip (at most 512 texels across) in which
    every texel is filled from the painted hair around it. It weights the colour by alpha
    (smoothstep 0.1–0.6) and fills by push-pull: the weighted colour is averaged down a
    pyramid, then each level's missing weight is filled from the level above. Painted texels
    keep their colour exactly, and each filled level is that mip. The texture keeps the
    source's colour space, and the lighting draw binds it at PS `t0` in place of the card
    texture. On Sassy SnW's `hairlong.dds`, transparent texels went from luminance 0.09 to
    0.41 (painted hair: 0.39). Without a readback (no alpha), strands use the card texture.
-   **Skin from the bone palette, not the card vertices.** Each strand point gets the
    barycentric blend of its triangle's bone weights (top four, unorm8). A compute shader
    applies `boneWorld × skinToBone`, the same transform the game skins the cards with.
    Positions are camera relative (`posAdjust`); previous positions use last frame's palette
    and `previousPosAdjust`. With physics on, the same pass makes each strand follow its
    simulated guide (see [Physics](#physics-strandsimcshlsl)).
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
-   **Blended hair with an alpha-tested layer draws its strands in the layer's passes.** When
    a hidden layer (see [What converts](#what-converts)) is alpha-tested and its twin is
    blended (KS Hairdos' "Hl" parts; `DrawsTwin`), the layer's depth prepass draws the
    strands' depth and its deferred lighting pass draws the strands, with a strand variant of
    the layer's own permutation. The blended pass then only hides its cards. The twin is
    skinned with its own geometry and skin instance, read from the layer's pass only once that
    geometry is found under the same actor's face node (`LiveTwinSkin`), so it is alive. Until
    the layer's strand permutation has compiled, the blended pass draws the strands as before,
    over the prepass depth. Blended hair without an alpha-tested layer is still drawn forward
    with no prepass depth.
-   **Effect shaders over the hair are hidden with the cards.** A magic effect membrane or
    an overlay such as Dirt and Blood's draws the hair's own geometry again with
    `BSEffectShader`, so it traces the cards. A hook on `BSEffectShader` slots 6 and 7
    hides that pass with the same viewport trick when the hair (for a layer, its twin) drew
    strands this frame or the last. Reflections keep the cards, and so their effects. The
    effect does not show on the strands at all (see [Not done](#not-done-candidates)).
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
counts draws strands; if the part is alpha-tested and that hair blended (every KS hair), the
part's passes draw the strands. Otherwise it keeps its cards: a hairline is a scalp cap that stays
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
7. **Guides.** The first strands of the shuffled list are the simulated guides: one per 8
   strands, 64 to 4,096. Each other strand follows the guide nearest its root that runs
   most like it: the smallest sum of squared distances at its root, middle and tip, each
   compared with the guide's point at the same distance from the root. A shorter guide
   loses on the tip, and a guide from another lock that only shares the root area loses on
   the middle and tip. `StrandInfo.guide` holds the choice; a guide names itself.
8. **Head collider.** A sphere round the skull centre (the `NPC Head` bone + 5 units up)
   with the radius at which only 2% of strand points lie inside (× 0.95, clamped to 2–10
   units).

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
-   Physics fades out over the last quarter of `PhysicsDistance` (default 400). Every guide
    is simulated while a hair is: guides are cheap next to the drawn strands.

## Physics (`StrandSim.cs.hlsl`)

Only guide strands are simulated, one thread each. `StrandSkin.cs.hlsl` then moves every
strand point with its guide at the same distance from the root: the point moves off its own
target by as much as the guide has moved off the guide's target there. The previous position
is built the same way from the guide's previous state, so motion vectors carry the simulated
motion. Past its guide's tip, a longer strand takes the tip's displacement. The guide's
rotation there (shortest arc from the target's tangent to the simulated one) turns only the
normal. Turning the offset from the guide too (the `0-2-0` follow) made the offset a lever
that amplified every bend of a short guide (see the `0-2-1` notes at the top).

-   **Targets, and how SMP guides.** A guide point chases its target: the point skinned by
    the head bone alone (the styled shape, rigid on the head), blended towards its full
    skinning by the global **SMP Guidance** (default 0.35). Non-SMP hair is skinned to the
    head, so both are the same. SMP hair adds its physics bones: at 0 the strands ignore
    them, at 1 they chase the SMP pose. Hair without a head bone always uses its full
    skinning.
-   **Inertia.** Each frame, last frame's state first moves with `1 − inertia` of what the
    point's target skinning did since (the rigid motion of that skinning, so it rotates
    too). At 0 hair moves rigidly with the head, or with the SMP bones at full guidance;
    at 1 it keeps all its world-space inertia.
-   **Steps.** The frame runs in steps of at most 1/60 s (frames are capped at 1/30 s, so 1
    or 2 steps). Targets move from last frame's pose to this frame's across the steps. A
    step integrates (Verlet: velocity, gravity, wind), pins the root to its target, then
    runs 3 iterations of: a global shape constraint (towards the target, see below), a local
    shape constraint (each segment keeps its rest direction relative to its parent segment,
    turned by the parent's shortest-arc rotation; the first segment keeps its rest
    direction), and follow-the-leader length constraints with collision. Velocities are the
    step's move with bending motion damped (below), clamped at 3,000 units/s. Dynamic FTL
    damping (Müller et al. 2012) is left out: under a steady load it made strands flutter (see
    the `0-2-3` notes at the top).
-   **Bending motion is damped.** The length and local shape constraints move only each
    segment's far point, so corrections run on down the strand, and a swing sent a wave to
    the tip and back (see the `0-2-5` notes at the top). After each step, relative to its
    target, each point is pulled `BendDamping` (0.8) of the way per 1/60 s towards the
    velocity it would have if its segment turned with the one before it, root to tip. A
    lock swinging as a whole keeps its speed, and only velocity changes, so hair at rest
    under any load stays at rest. Making the local shape constraint move both ends stopped
    the wave as well, but under a heavy steady load the strand then never settled.
-   **Stiffness by distance from the root.** A load (air drag, the head's acceleration, wind,
    gravity as the head tilts) moves a point about the same distance on any strand, so how
    far a point may swing has to depend on how far it is from the root, not on how far along
    its own strand. The global shape stiffness falls from `rootStiffness` to `tipStiffness`
    over the first 20 units (`FreeLength`, about 28 cm), or over the whole strand if it is
    longer, as before `0-2-5`. With the Straight preset (0.4 → 0.03) a 4-unit lock ends at
    0.33 and a 10-unit lock at 0.22.
-   **Same motion at any frame rate.** Stiffness and damping are authored per 1/60 s. A
    shorter step scales stiffness as a compliance (XPBD: `h² / (h² + α)`, with α fitted so
    the authored value holds at 1/60 s). Position projections also damp the motion relative
    to the target by `1 − s` per step, which shorter steps lose; that is given back on the
    velocity relative to the target.
-   **Damping and air drag.** A style's `damping` (default 0.4) is the share of velocity
    relative to the target lost per 1/60 s (`SwingDamping`): it settles swings, only ever
    takes energy out, and does not slow hair that moves with the head. Air drag (`kAirDrag`,
    0.06 per 1/60 s) applies to world velocity, and is what makes hair trail while running,
    so it stays small and is not a style field. Until `0-2-6` `damping` was the air drag and
    swings lost a fixed 0.2. In the NumPy port (`tools/hair_strands_sim_check.py`), a
    20-unit lock 0.3 s into a sprint start lagged 8.5, 5.0 and 3.5 units at 30, 60 and 144 fps
    with plain per-frame scaling. With this scheme and the `0-2-6` defaults it lags 3.8 units
    once running at 30, 60, 144 and 240 fps alike, and settles within 3 s at every rate. A
    fast 70° turn peaks 0.5 units off target as it starts. Shorter steps with the plain
    scaling kept oscillating.
-   **Gravity is preloaded.** The styled shape is how hair hangs with the head upright, so
    only the change as the head tilts acts: `g × (down − R_head × down)`, with `g` = 687
    units/s² × `gravity`. Without that, hair would sag below its style at rest. The
    restoring force after a swing comes from the shape constraints.
-   **Wind** is the weather's (`Sky::windSpeed`, `windAngle`; none indoors), up to 600
    units/s² at full wind speed × **Wind Strength** × `windResponse`. It pushes across the
    strand, stronger towards the tip, with slow gusts out of phase per strand.
-   **Collision.** The head, and capsules found up the head bone's own skeleton:
    neck (neck → head, radius 3), chest (spine 2 → neck, 5.5), back (spine 1 → spine 2,
    6.5), shoulders (clavicle → upper arm, 3.5) and upper arms (upper arm → forearm, 3).
    Radii scale with the head bone's world scale. A point is pushed out to at most its own
    target's depth inside the collider, and never left deeper than half the radius, so the
    styled shape itself never collides.
-   **The head field.** The head is the actor's own head mesh (its Face head part: FaceGen
    and RaceMenu morphs included), read once per actor and hair (`BuildHeadField`). It is
    stored as a radial height field: a 64 × 64 octahedral map of directions from the hair's
    skull centre (about 3° a texel), each holding the distance to the outermost head surface.
    The mesh is brought into the hair's skin space through both meshes' bind poses on the
    head bone, so the field rides the head bone rigidly; vertices less than half on the head
    bone (the neck) are left out and the neck capsule covers them. Points 0.2 units apart over
    every head triangle are splatted into it, each texel keeping the outermost; texels with
    four or more filled neighbours are filled from them three times (eye sockets, the mouth),
    and the neck opening stays empty (no collision). Radii are capped at 1.5× the median, so
    ears, muzzles and horns cannot throw passing hair out to their tips. A point is pushed out
    along its direction from the centre, never deeper than its target lies below the surface
    at the target's own direction. Guides collide in the simulation, and every strand point is
    kept out once more after following its guide (`StrandSkin.cs.hlsl`, previous positions
    against last frame's head). On a synthetic head with eye holes (a mirror of the build and
    lookup), the field is within +0.13 units of the true surface everywhere (median +0.04).
    Without a head mesh skinned to the head bone, when it covers under half the directions,
    or when its median radius is not within 0.7-2× the head sphere's (the meshes do not share
    that centre), the head sphere is used as before; the reason is logged (info for the player,
    debug for others).
-   **Restarts.** A guide restarts from its targets when it is first simulated, after more
    than 2 frames unsimulated, when its asset changes, and (per strand, on the GPU) when
    its root moves more than 40 units in a frame (teleports, loads). Non-finite state falls
    back to the target.
-   **Paused:** no steps; the state is only carried with the bones.
-   **Not simulated:** style `simulate` off, area-seeded (short) hair, or past the physics
    distance. Such hair is plain skinning, as before `0-2-0`.

State per instance: `HairStrands::GuideState`, 96 bytes per guide point (a 2,500-guide,
20-point hair is 4.8 MB). Positions are camera relative, stored against the camera of the
simulation that wrote them and shifted by the camera's move each frame.

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
    `coverageThreshold`, `seed`, `excludeUV`), render (`rootWidth`, `tipWidth`, `waveAmplitude`, `waveLength`,
    `curlRadius`, `curlLength`, `curlStart`, `frizz`, `flyaways`) and motion (`simulate`,
    `rootStiffness`, `tipStiffness`, `bendStiffness`, `damping`, `gravity`, `inertia`,
    `windResponse`; stiffness and damping per 1/60 s; `damping` takes out motion relative to
    the head, not air drag; `tipStiffness` is reached 20 units from the root, so shorter
    strands stay stiffer). Tooltips in the editor explain each field.
    Units are Skyrim units, about 1.4 cm. Motion fields apply live.
-   In-game editor: select a hair in view, edit it, and the change applies to every actor
    wearing it. Render fields apply live; generation fields apply when the slider is
    released. **Save** writes the fully resolved style, matched on that exact head part,
    model and shape, to `UserStyles.json`.

## Hair types

| Preset | What it changes |
| --- | --- |
| Straight | defaults: light clumping, little frizz; free-swinging tips (stiffness 0.4 → 0.03, bend 0.35, damping 0.4, inertia 0.85) |
| Wavy | per-lock sine waves (period 4); a little stiffer (tip 0.04, bend 0.45) |
| Curly | helical curls (radius 0.35, period 1.6), strong clumping, so locks spiral together as ringlets; springy (stiffness 0.45 → 0.08, bend 0.7, damping 0.3) |
| Coily | tight coils from the root (radius 0.18, period 0.45), little clumping (a cloud rather than ringlets), high volume and frizz, denser and thicker strands so the scalp does not show; holds its shape (stiffness 0.55 → 0.35, bend 0.85, damping 0.5, gravity 0.5, inertia 0.5) |
| Locs | clump pull 0.95 with twist: strands collapse into twisted ropes (locs, braids, twists); heavy (stiffness 0.35 → 0.02, damping 0.25, gravity 1.2, inertia 0.9) |

Short hair (buzz cuts, fades, fuzz) is covered by area seeding, not a preset. Long hair just
gets more control points, up to 32. Dark hair over a light background shows gaps between
strands most: raise `density` or the root width for it.

## Shared-file hunks

Both are in `Lighting.hlsl`, both are additive, and both are gated on `HAIR_STRANDS`. Only
`StrandLighting.hlsl` defines that, so card permutations compile unchanged.

1. After the non-landscape base colour and normal sample: strands take the colour at `t0`
   (their own colour texture, see above; `HAIR_STRANDS_COLOR_MIP_BIAS` is 0) with alpha 1
   and a flat normal.
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
    signature must match the strand PS input signature. Compile `StrandSkin.cs.hlsl` and
    `StrandSim.cs.hlsl` as `cs_5_0` with `-I package/Shaders -I "features/Hair Strands/Shaders"`.
    fxc rejects partial writes to `Guides[]` fields inside the sim's reset branch (`X4532`),
    so those writes stay unconditional. HLSL `for (uint i ...)` leaks `i` into the function
    scope, so the sim declares its index once.
-   Solver: `python tools/hair_strands_sim_check.py` (`--verbose` for the time series) runs
    a NumPy port of `StrandSim.cs.hlsl` on a hanging lock: still head, sprint start and
    stop, fast 70° turn, 60° bow and a shoulder capsule, at 30 to 240 fps. It fails if the
    lock leaves its target while still, stretches, lags differently across frame rates or
    does not settle. 1.5- and 3.8-unit scalp locks and the hanging lock, held still on a
    tilted head (up to a head on its side), must come to rest (under 0.005 units a frame
    after 4 s). The two scalp locks must stay within 10% of their length of target through
    one step aside and a 15° turn. Once the head stops, no lock's tip may turn back more
    than twice (60 and 144 fps, frame times jittered by 10%). It also runs followers beside
    and longer than a 3.8-unit guide through idle sway and snap turns, and fails if one
    strays further than its guide or changes length by more than 10%. The single lock missed
    the `0-2-4` faults, so also run changes on a whole head of short locks before tuning.
    Port solver and follow changes to it first (a few minutes to run), then tune.
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
-   Physics (`0-2-0`): the tuning is from the NumPy port, not seen in game. Check first that
    still hair sits exactly where it did without physics (a visible offset means the
    follow pass is off), then tune the presets. Since `0-2-5` hair under 20 units keeps its
    shape far better, and a 20-unit lock no longer swings back after a fast turn: it peaks
    as the turn starts (0.5 units with the `0-2-6` defaults; 1.55 in `0-2-3`, 2.58 in
    `0-2-0`). Under a load a 20-unit lock rests 3.1 units off target (a 60° bow; 2.84
    before `0-2-6` lowered `rootStiffness`). More inertia, less stiffness or less damping
    gives more swing; short hair needs a lower `rootStiffness` as well.
-   The strand colour texture binds at PS `t0`, which is `TexColorSampler` in every Lighting
    permutation hair uses. Strands in a flat colour or another texture would point here.
-   The hair's skin instance lists `NPC Head [Head]` (non-SMP and most SMP hair) and that
    bone's parents are `NPC Neck [Neck]`, `NPC Spine2 [Spn2]` and `NPC Spine1 [Spn1]`, with
    the clavicles and arms under spine 2. Without them there is only the head sphere.
-   KS hair (`0-2-8`): the log should name a Utility descriptor for strand depth drawn in the
    Hl part's prepass, and RenderDoc should show `Hair Strands` inside the deferred pass, not
    the blended one. KS hair still see-through after that would point at strand coverage
    (density × width per layer of cards: at the defaults about 0.7 at the root and 0.2 at the
    tip), not at the pass.
-   The body collider radii (neck 3 to back 6.5) are guesses meant to sit inside any body.
    Hair floating off the shoulders means they are too large; hair through them, too small.
-   The head field (`0-2-7`): the player's log should say `head collider from head mesh …
    (N% of directions)`, with N around 85-90. The Face head part hangs under the face node by
    its editor ID, its CPU vertex data is kept (as the hair's), and its skin lists
    `NPC Head [Head]`. Hair pushed off the head by a constant gap, or into it, would mean the
    bind-pose chain (hair skin → head bone → head mesh skin) is off.
-   `RE::GetSecondsSinceLastFrame()` is real frame time, and `UI::GameIsPaused()` covers
    menus. Slow-motion kill cameras may play hair at full speed.

## Not done (candidates)

-   Strand shadow maps and self-shadowing beyond Hair Specular's, and deep opacity maps.
    Cards cast the shadows.
-   Hair-hair collision, and colliders fitted to the actual body mesh (breasts, armour,
    weapons on the back). Below the head, colliders come from bones with fixed radii. The
    body is skinned to many bones, so a rigid field like the head's would not fit it.
-   Wind from anything but the weather (spells, dragons, player speed beyond air drag).
-   Wigs have no model path in their key (no head part), so they match on shape name and
    vertex/triangle count.
-   Actor fade-out keeps the cards: strands have no alpha to fade with.
-   Beards and other facial hair keep their cards. They would need their own flow and
    density rules, since beard cards lie flat on the skin.
-   Model-space-normal hair permutations keep their cards.
-   Effect shaders on strand hair (magic effect membranes, Dirt and Blood) are hidden, not
    drawn on the strands. That needs a strand variant of `Effect.hlsl`, as
    `StrandLighting.hlsl` is of `Lighting.hlsl`, with its own membrane permutations.
