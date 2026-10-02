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

`0-3-0` (2026-09-29) takes the flow from the hair's flow map where it has one, and elsewhere
from the part of the texture each card samples. Until then each connected piece of the mesh
was turned to run away from the head and downwards, which says little for hair lying on the
scalp. Vanilla hair has flow maps (Vanilla Hair Flowmaps, the ones Hair Specular reads).
Measured against them, the old flow ran backwards on 14% (female hair 06), 19% (13) and 35%
(14) of the card area, all on the crown and top of the head. Without the maps it now runs
backwards on 0%, 1% and 12%; the 12% is hair 14's braids wrapped sideways round the head.
With the maps installed it follows them. The top of KS TombRaider, combed back into its
braid, grew from the braid (16% of its area) and now grows from the hairline. See
[Conversion algorithm](#conversion-algorithm-strandgeneratorcpp).

The owner then found hair highly resistant to leaving its styled shape, even at SMP Guidance
0: jumping off a cliff, long hair lifted only at the ends, and motion settings pushed far
enough to free it made it fly everywhere. `0-3-0` replaces the solver with a port of TressFX
4.1's simulation, as a known-working baseline to tune from. Real-time conversion, SMP
guidance, weather wind, the head field, the body colliders and the follow scheme stay. The
`0-2-5` requirements still hold: short scalp locks keep their shape through a step aside, and
a swing dies down once the head stops, as a pendulum's does, instead of running up and down
the strand. See [Physics](#physics-strandsimcshlsl). `0-3-0` was checked with DXC (both
compute shaders), the constant buffer layout against DXC's reflection, a clang syntax check of
the style code, and the NumPy port. Merged with the flow change, it builds with MSVC, and fxc
compiles both compute shaders (with the same buffer layouts as `Strands::SkinCB` and
`GuidePoint`) and the strand Lighting permutations. Neither half has been run in game.

`0-4-0` (2026-09-30) lays hair over the character's own body and what it wears. Below the head,
hair used to collide with bone capsules of fixed radii, kept well inside any body. Each capsule
shrank to wherever the styled hair already lay, so hair never collided with its own styled
shape. Armour was never in the colliders, and hair went through it. Now eight colliders (neck,
three spine segments, clavicles, upper arms) are fitted to the worn meshes: body, armour, the
head mesh's neck. They are rebuilt on a worker whenever what the actor wears changes. Hair
styled into armour (or into a body larger than the one it was made on) lies on it. Hair styled
on the bare body stays as styled. See [Physics](#physics-strandsimcshlsl). In the NumPy port the
colliders lie within 0.35 units of the true surface. A lock styled through the shoulder lies
over it and comes to rest. Hair stays out of the body (0.3 units deep at most, drawn) while
breathing, walking, running and turning at 30 to 144 fps. TressFX's capsule response stops a
point dead. Against a moving body, that left hair to be shoved again every step, and it shook.
The body colliders use their own response (below), and the worst shake is 0.08 units.

`0-5-0` (2026-10-01) collides hair with everything the character visibly wears. The `0-4-0`
colliders were eight radial maps, rigid on the neck, spine, clavicle and upper arm bones.
Nothing covered the hips, forearms or legs, so hair down to the waist went through them. A joint
away from its bind pose collided where that part sat in the bind pose. Each direction held one
surface, and shields, weapons and quivers (rigid meshes) were not seen at all. Now the worn
meshes (body, armour and clothes, and the rigid ones hanging on the body) make one decimated
collision mesh, built on a worker when what is worn changes. Every frame the GPU skins it and
builds a narrow-band signed distance field round the hair's reach, as TressFX 4.1 does with its
collision mesh, and the guides and the drawn strands collide with that. See
[Physics](#physics-strandsimcshlsl). The NumPy port builds the field as the shaders do, from
three closed synthetic bodies: bare, in a cuirass 2.5 units off the body, and with a 2-unit
shield on the back as well. Within 1.5 units outside, the field is off by −0.38 to +0.18 units
(1st to 99th percentile). At most 8 of 3,000 points outside read inside, and 3 to 48 of 1,000
points up to 6 units deep do not. Hair styled through the shoulder comes to rest over it, hair on
the back rests as styled, and the same hair in the cuirass rests on the cuirass. Breathing,
walking, running and turning at 30, 60 and 144 fps leave drawn hair 0.00 units deep on the bare
body. A 45-unit lock to the waist, in the cuirass with the shield, goes at most 0.23 deep, and
neither shakes more than without the body. On the way the port found three faults in the
response, now fixed (*Contact*, *Thin parts* and *Steps between frames* below). VSP's move on top
of the surface's drove hair on a running body into it (3.4 units deep at 60 fps, 8.3 at 30). A
hair tip swinging into the shield went through it. At 30 fps the steps between frames missed the
body at a sprint. The shaders compile with DXC (all five entry points), the constant buffers
match DXC's reflection, and the C++ passes a clang syntax check against CommonLib. It has not been
built with MSVC, fxc has not compiled the new kernels, and none of it has run in game.

`0-5-1` (2026-09-30) fixes the first in-game run of `0-5-0` in iron armour. Short hair snapped
onto the cuirass's collar, and collision made hair shake. The cause was the inside band. A
thickness ray that found no face to leave through counted as 15 units of solid. Behind every
open edge (a collar's rim, a hem, a pauldron's edge) and every sheet, the field then called up
to 7.5 units of open air "inside". The iron collar's rim faces down, so the air above it, where
nape hair hangs, read up to 7.5 units deep, and hair there was thrown out across the collar. A
NumPy port of the collision mesh and field, run on the real iron cuirass and body with vanilla
`hair01`'s strands, shows it. 1,826 strand points read inside, and 31 of 52 strands near the
field ended over 1 unit off their style (up to 8.8). Breathing, 29 of 52 shook, by up to 10.5
units. Now a ray counts only if it leaves through a face, however far, before it leaves the
mesh's box. A triangle none of whose rays counts, or a sheet, has no inside at all: the field is
the distance either side of it. In the same port no strand point reads inside, hair rests
exactly as it does with no body, and
nothing shakes. Points inside the armour's thick plates read inside as before (183/594 pauldron
points, 80 of 170 cuirass points against 82). Also: the split length now grows with the merge
cell. At a fixed 2.5 units the splits undid the coarser merges, so a busy outfit stayed over the
65,536-triangle budget, and whole meshes were left out. The log showed it as `6.33-unit cells`,
one retry more than was used. Triangles on vertices with no bone weight (they skin to the
camera) are left out. It builds with MSVC, fxc compiles the kernels, and the sim check passes.
It has not run in game.

`0-6-0` (2026-10-01) grows every strand from the scalp. Strands used to start on whatever card
edge the flow entered: outer layers standing off the cap, under-layers, a ponytail below its tie
and the start of a lock past a transparent gap all grew strands from mid-air, pinned to the head
(on a synthetic layered style 26% of strands, on a ponytail 28%). The conversion now lives in an
engine-agnostic module, `CardsToStrands/`, shared with standalone tools. It fits a scalp to the
innermost hair, traces one guide per `clumpSize` of card width and drops guides that repeat a
longer one. Guides starting on the scalp grow from it, and guides starting off it continue the
rooted hair they start on or that ends where they start. Each guide then grows a clump of
strands rooted round it on the scalp. See [Hair cards to strands](hair-cards-to-strands.md). On
the synthetic styles no strand roots more than 2 units off the skull, stubs fall from 11-15% to
under 1%, and median strand length doubles on long hair. `Clump Size` is now the card width
each clump (one card guide) stands for. The module and the game's adapter build with g++ and clang, and the adapter's output matches
the command-line converter's exactly. It has not been built with MSVC or run in game.

Earlier builds are build-verified, and the generator fixes are checked on real meshes (see
[Verifying changes](#verifying-changes)). Work through
[Unverified assumptions](#unverified-assumptions) first.

## Pipeline

| Stage | Where | Thread | When |
| --- | --- | --- | --- |
| Classify: is this geometry hair? | `StrandRenderer::Classify` | render | first draw of a geometry |
| Resolve style (file → preset → editor override) | `StrandRenderer::ResolveStyle` | render | first draw, and after style files or settings change |
| Copy mesh (bind pose, weights, UVs) | `MeshExtract.cpp` | render | once per hair and style |
| Copy one mip of the diffuse texture, and of the flow map if there is one, to staging textures, map them once the GPU is done | `BeginCoverageReadback`, `BeginFlowReadback`, `PollCoverageReadback` | render | once per hair and style, a frame or two before generation |
| Decode the texture's alpha and luminance (any format, BC included, via DirectXTex) and fill its colour for strands; decode the flow map | `DecodeCoverage`, `DecodeFlow` | worker | start of the generation job |
| Generate strands (fit the scalp, bind card guides to it, grow a clump per guide), pick guide strands, fit the head collider | `StrandGenerator.cpp` → `CardsToStrands/` | worker (`std::async`, 2 at a time) | once per hair and style, shared by every actor |
| Upload asset (strands, colour texture) | `StrandRenderer::BeginFrame` | render | when the job finishes |
| Read the actor's head mesh into the head field | `BuildHeadField` | render, in `SetupGeometry` | once per actor and hair asset, with physics and collision on |
| Look at what the actor wears; copy the colliding triangles of what changed | `UpdateBodyField`, `FindBodyMeshes`, `PrepareBodyField` | render, in `SetupGeometry` | every 15 frames; a copy when the worn meshes change |
| Build the body colliders' maps | `BuildBodyField` | worker (`std::async`, one per hair) | when the worn meshes change |
| LOD, bone palette, colliders (`GatherColliders`, `GatherBodyColliders`), simulation and skinning compute | `PrepareStrands` (`UpdateLod`, `Skin`, `PrepareSimulation`) | render, in `SetupGeometry` | first pass of the hair each rendered frame (depth prepass or lighting) |
| Draw strand depth | `StrandRenderer::Draw` (depth only) | render, in the Utility `RestoreGeometry` | every Utility draw of the hair that writes depth (the depth prepass) |
| Draw ribbons | `StrandRenderer::Draw` | render, in `RestoreGeometry` | every main-view lighting draw of the hair |

Everything lives in `src/Features/HairStrands.{h,cpp}` (feature, settings, UI) and
`src/Features/HairStrands/` (the `Strands` namespace; the body colliders are `BodyField.{h,cpp}`).
Shaders are in `features/Hair Strands/Shaders/HairStrands/`.

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
-   **Kept cards are drawn by the strands, not by the game.** Hiding is all or nothing per
    draw, so the triangles a hair keeps as cards (braids, ties, gathered hair) are drawn again
    right after the strands, in the same hooks, from the asset's own copy of them
    (`CardVertex`, an index buffer): `HairStrands/CardLighting.hlsl`, compiled with the hair's
    permutation defines like `StrandLighting.hlsl` but without `HAIR_STRANDS`, so its pixel
    shader is `Lighting.hlsl`'s as the game uses it, with the card texture, alpha test and
    normal map the pass bound. Its vertex shader skins the cards with the strands' bone palette
    (the chain joints included) and the mesh's own tangent frame (`MeshExtract` reads it as the
    Lighting VS does: row one from the position's, normal's and tangent's fourth components, row
    two from the tangent). UVs use the material's offset and scale from the draw's constant
    buffer, because the prepass's Utility shader binds another `b1`. The depth prepass draws
    them with `CardDepth.hlsl`, which alpha-tests the texture the Utility pass bound at `t0`
    against that pass's threshold (`AlphaTestRef.x` in its `b2`), as `Utility.hlsl` does: depth
    written through transparent card parts would cut holes in whatever lies behind. A Utility
    pass without `ALPHA_TEST` draws them with no pixel shader. The lighting pass widens the equal test as the
    strands do but keeps the pass's depth writes (off for blended hair) and its rasterizer and
    blend state, so the cards cull and blend as authored. A hair kept wholly as cards (a braid
    on its own) has no strands and draws only its cards. Shadow maps still use the game's cards,
    so a swinging braid's shadow stays where it was styled.
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

Within a converted hair, since `0-7-0`, only loose hair becomes strands. Braids, twists, ties,
buns and the hair pulled tight into them keep their cards, and braids hanging free swing on
chains of their own; loose hair below a ponytail's tie or a braid's end grows strands from there
(see [Hair cards to strands, Hair that is not loose](hair-cards-to-strands.md#hair-that-is-not-loose)).
The strands draw those kept cards themselves (see [Why it is built this way](#why-it-is-built-this-way)).
The style's **Keep Braids and Ties as Cards** turns this off, and its UV rectangles keep parts
as cards or as swinging cards by hand.

## Conversion algorithm (`StrandGenerator.cpp`)

`StrandGenerator.cpp` copies the mesh and style into `CardsToStrands` (engine-agnostic, in
`src/Features/HairStrands/CardsToStrands/`) and packs its result for the GPU. Steps 1 to 3, 7 and
8 are as below; since `0-6-0`, steps 4 to 6 are replaced by fitting a scalp, binding card guides
to it and growing a clump of strands per guide, described in
[Hair cards to strands](hair-cards-to-strands.md). Steps 4 to 6 below describe `0-5-1` and
earlier, kept for the history of the fixes they mention.

1. **Weld** positions (1/1000 unit). Vertices at one position join only if their normals
   face the same way (dot above 0), so the two sides of a double-sided card stay separate
   sheets while a card that curves round the head stays one. A triangle repeated on the same
   welded vertices (a back face sharing the front's vertices) is dropped. Back faces on
   vertices of their own are dropped a sheet at a time: a sheet more than half of whose area
   repeats earlier kept sheets goes, and any other sheet is kept whole, overlap included.
   Dropping copy by copy leaves an interleaved double-sided mesh (vanilla hair lists front
   and back alternately) as two checkerboards of isolated triangles. It also cuts holes
   where a sheet shares a band with another's back. Either way, strands stop at every hole.
2. **Flow** per triangle, root to tip, projected into the triangle. With `flowAxis` V, -V, U
   or -U it is that texture axis (∂P/∂V or ∂P/∂U from the UVs). With `auto`:
   -   **Flow map.** A material with the back-lighting flag and a back-lighting texture (slot
       7) over 32×32 has a flow map, as Hair Specular reads it (`Lighting.hlsl`): RG × 2 − 1
       is the direction from tip to root in texture space, and black texels have none. Where
       its mean over a triangle (15 probes) is at least 0.3 long, it is mapped through the
       triangle's ∂P/∂U and ∂P/∂V onto the surface. PGPatcher's "Add Hair Flow Map" puts
       Vanilla Hair Flowmaps' maps there; they cover 98-100% of vanilla hair. With them,
       vanilla hair hanging below the head runs down on 90-100% of its area, which confirms
       the tip-to-root sense.
   -   **Axis from the texture.** Elsewhere each UV island (vertices sharing a position and a
       UV; one card's strip of the atlas) runs the way the strands are painted in the texels
       it samples. That is the structure tensor of the diffuse's alpha-weighted luminance,
       smoothed over about 1/85 of the texture and summed over the island: painted strands
       are streaks, so brightness changes fastest across them. On every atlas checked the
       streaks agree strongly (coherence 0.6-0.9). Below 0.2, or without a readback, the
       island takes U when it is more than 1.5× longer that way on the surface, else V.
       Vanilla's atlas lays one strip sideways (V 0.74–0.88): 24% of the Nord hair's card
       area and 71% of `0_td18_hair_9`'s. The shape rule sent strands across wide, short
       cards, such as KS Tails' layered tiers (8 units wide, 5 long).
   -   **Root and tip.** Islands joined by welded seams form pieces that turn together:
       across a seam hair continues, or runs beside its neighbour, the same way (or away
       from a parting). A seam where the flow mostly crosses, or keeps changing sign, joins
       nothing. Hair runs away from the skull centre (the `NPC Head` bone + 5 units up) and,
       on balance, downwards, but only hair hanging free shows that. Free means 1.3 to 1.8
       scalp radii from the skull centre, the scalp radius being the distance of the
       innermost 10% of the hair's area. On the scalp and the nape hair runs either way:
       down from the crown, back from the hairline, or up into a tie. A piece partly on the
       flow map follows the map. A piece whose free-hanging part clearly votes one way (a
       mean of 0.3 over the whole piece) keeps that way. Every other piece follows the
       decided pieces that sample the same texels, in 64×64 cells of the texture, because
       the same texels show the same painted strands. With nobody to follow, its own weak
       vote stands. Only decided pieces vote in the cells: Apachii hair 79 maps its scalp
       block either way up, and weak votes from such cards are noise. A decided piece keeps
       its way whatever the texels say, because some cards map their strip upside down
       (one long card of KS TifaLong).
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

Since `0-3-0` the guide solver is a port of TressFX 4.1's simulation
([GPUOpen-Effects/TressFX](https://github.com/GPUOpen-Effects/TressFX) at `ba0bdac`,
`src/Shaders/TressFXSimulation.hlsl`, MIT; AMD's notice is kept in the shader). Only guide
strands are simulated, one thread each. TressFX dispatches each kernel over every vertex or
strand of the asset; here one thread runs a whole strand, and each kernel is a loop with the
same result.

### A step, as in TressFX

Every step runs TressFX's simulation pass on a strand, in TressFX's order and with its maths:

1. **IntegrationAndGlobalShapeConstraints.** Verlet integration:
   `x + exp(-damping x 60 h) (x - x_prev) + g h^2`, with gravity straight down. The first two
   points of a strand are pinned to their targets (TressFX's assets give them inverse mass 0).
   Points whose index is below `globalConstraintsRange x points` are then pulled towards their
   targets by `globalConstraintStiffness` (further on short strands, below). The rest of the
   strand is not.
2. **CalculateStrandLevelData, VelocityShockPropagation.** The rotation (shortest arc between
   the root segment's old and new directions) and translation that took the root segment from
   the last step to this one move every other point, current and previous positions alike, by
   `vspCoeff` (less on short strands, below). That carries that share of the head's motion
   rigidly without adding velocity. When the second point's pseudo-acceleration
   `|p - 2 p' + p''|` passes `vspAccelThreshold` (a snap or a landing), the share is 1.
3. **LocalShapeConstraints**, `localConstraintsIterations` times. From the root, each segment's
   rest vector is turned by the shortest arc from its parent segment's rest direction to its
   current one. The next point moves towards the end of that vector by
   `0.5 x min(localConstraintStiffness, 0.95)`, and the point before moves back by as much.
   Pinned points stay put.
4. **LengthConstriantsWindAndCollision.** Wind (below), then `lengthConstraintsIterations`
   passes of distance constraints, even pairs then odd pairs as TressFX's threads do them.
   Then TressFX's capsule collision: a point inside is put on the surface, keeps 0.4 of its
   move along the capsule, and stops. The body colliders (not TressFX, below) come with the
   capsules. Last, the position delta clamp:
   `delta x clampPositionDelta^2 / |delta|^2` past `clampPositionDelta`, TressFX's formula.
5. **Signed distance field collision** (`CollideHairVerticesWithSdf`), which TressFX runs
   after the simulation: here with the head field (below). A point inside is put back on the
   surface and stops. TressFX does every point but the first two, followers included; the
   followers are done in `StrandSkin.cs.hlsl`.

### Around the step (not TressFX)

-   **Fixed steps.** TressFX runs one pass per frame of whatever length (its sample clamps
    frames at 0.05 s), so its motion changes with frame rate. Here `StrandRenderer::BeginFrame`
    keeps a clock shared by all hair: 1/60 s steps (TressFX's samples ran at 60 Hz), frames
    clamped at 0.05 s, at most 4 steps a frame, none while paused. The targets move from last
    frame's pose to this frame's across the frame, and each step takes them at the time it
    ends. Strands are drawn at this frame's targets plus their offset from the target,
    interpolated between the last two steps. The dynamics show one step late (16.7 ms), the
    roots never. In the NumPy port the sprint and turn agree within 5% at 30, 60, 144 and 240
    fps and the fall within 9% (at 30 fps), and the sprint within 0.02 units with frame times
    jittering by 30%.
-   **Targets, and how SMP guides.** A target is the point skinned by the head bone alone (the
    styled shape, rigid on the head), blended towards its full skinning by the global **SMP
    Guidance** (default 0.35). The local shape constraints and rest lengths come from these
    targets. TressFX skins each strand rigidly by its root's bones, which is Guidance 0. Non-SMP
    hair is skinned to the head, so both are the same; hair without a head bone always uses its
    full skinning.
-   **Short strands.** TressFX's constraints act per point, whatever a strand's length, so a
    load moves points about the same distance on any strand. A converted hairstyle mixes
    1-unit scalp strands with 40-unit locks. Without what follows, at the defaults, a 3.8-unit
    scalp lock swung 60% of its length off target on one step aside (15 units in 0.5 s), and
    a 1.5-unit one 114%, with its root segments folded into kinks. On a strand shorter than
    10 units (`ShortStrandLength`), the global range reaches as far as on a 10-unit strand
    (`globalConstraintsRange` x 10 units from the root, so a strand shorter than that is held
    all along), and VSP is scaled by its length over 10 units (below the acceleration
    threshold; above it VSP is 1 on any strand). VSP moves the strand by the root's motion
    over the step after the global pull has put the held part on its targets, so it carries
    that part past them: on a moving head, TressFX's held points run ahead of their targets
    by about a third of each step's motion (at the default `vspCoeff`), which a short
    strand's short segments cannot absorb. Now the 3.8-unit lock stays within 4% of its
    length of target and the 1.5-unit lock within 10%. Strands of 10 units or more are
    TressFX's.
-   **Wind** is the weather's (`Sky::windSpeed`, `windAngle`; none indoors), made as TressFX's
    `SetWind` makes it. The magnitude, 125 x wind speed x **Wind Strength** x `windResponse`,
    swells and fades with `sin^2(step x 0.01) + 0.5`. Four vectors 40 degrees off the wind's
    direction form a cone, and each strand mixes them by `(guide % 20) / 20`. The force on a
    point is `-((v x w) x v)` for its segment `v`, from the third point to the one before the
    tip. TressFX takes `v` at its current length. Here it is at its rest length: a stretched
    strand caught more wind the longer it got, and in the NumPy port loose settings blew up.
    TressFX's `asin` turn from its X axis to the wind is only right within 90 degrees of X.
    Weather wind is level, so the turn is a plain turn about Z.
-   **Stretch cap.** A few Jacobi passes cannot stop a long strand that is still moving when
    the head stops. On a hard landing a 40-unit lock stretched to 1.69 times its length; 20
    passes still left it at 1.47. After the length constraints no segment is left longer than
    1.2 times its rest length, measured from the root. That holds landings and sprint stops to
    about 1.16 and barely changes the motion.
-   **Collision.** The body's distance field (below), built every frame from what the actor
    wears. Until an actor's collision mesh is built, or when none can be, TressFX's capsule
    response on capsules found up the head bone's own skeleton: neck (neck → head, radius 3),
    chest (spine 2 → neck, 5.5), back (spine 1 → spine 2, 6.5), shoulders (clavicle → upper
    arm, 3.5) and upper arms (upper arm → forearm, 3). With no head field, the head is a sphere
    under the same response. Radii scale with the head bone's world scale. Per point, each
    capsule shrinks to the depth the point's target already lies at (never below half its
    radius), so the styled shape itself never collides.
-   **The body's distance field** (`BodySdf.cpp`, `BodySdf.cs.hlsl`, `HairStrandsSkin::CollideBody`).
    TressFX 4.1 collides hair with a signed distance field of its collision mesh, built every
    frame on the GPU: the mesh skinned, then per triangle the distance to every grid cell near
    it, the nearest kept with an atomic minimum. This is the same, from what the actor wears.
    -   *Sources.* Every shown, lit geometry under the actor's 3D: skinned (body, armour, clothes,
        cloaks) and rigid (shields, weapons and quivers on their nodes). Head parts are left out
        (the hair among them), and hair-tinted geometry (wigs); the head mesh is added back for
        its neck. Triangles wholly on the head bone or bones under it (the face, helmets, hoods)
        are the head field's, and so are rigid meshes under the head bone.
    -   *Collision mesh.* Built on a worker when what is worn changes, from one copy of each
        mesh's vertices (or their GPU buffer read back). Vertices within 1.25-unit cells are
        merged, never across their main bone or the octant they face, so a plate's two sides and
        parts on different bones stay apart. Of two faces closer than 0.6 units back to back (a
        thin plate, or a surface with its back faces modelled) the one facing away from the
        body's long axis stays: across such a pair the field would flip sign from cell to cell,
        and a plate thinner than a cell slips between them. A two-sided material is one sheet,
        turned to face out as most of its area does. Triangles with edges longer than two merge
        cells (2.5 units at first) are split. Each triangle gets an inside band (below).
        Triangles on a vertex with no bone weight are left out: the GPU would skin it to the
        camera. Past 65,536 triangles the vertices are merged 1.5 times further apart, up to
        three times. The split length grows with the merge cell; at a fixed length the splits
        brought the triangles back. Still over the budget, the meshes last in the list lose
        their triangles, and the log says how many.
    -   *Field.* For every simulated actor, every frame, three dispatches: the collision mesh
        skinned with its sources' bones this frame and last; each triangle splatted into the
        cells within 2 cells of it, and into the cells up to its inside band straight behind it
        (within 45°); then per cell, from its nearest triangle, the signed distance, the outward
        normal and the surface's move over the frame. The grid is in the actor's own axes,
        1.25 units a cell at its scale, snapped so cells stay fixed on the actor. It covers what
        the hair can reach (its furthest strand point from the skull centre, stretched 20%, plus
        2 units) where the body is. Every hair of an actor shares its field.
    -   *Sign.* From the nearest triangle's vertex normals at the closest point, not its face
        normal: the mesh is decimated, and its small triangles turn every way. A cell more than
        75° from that normal is inside. One nearly level with the surface (beside an open edge: a
        collar, a hem) counts as outside, so hair passing an edge is not pulled round it. A
        triangle with nothing solid behind it (below) has no inside: every cell it is nearest to
        is outside, on whichever side, and hair there is kept off it on its own side.
    -   *Inside band.* How deep the field reaches behind a surface: half how thick the mesh is
        there (the shortest of five rays, straight in and four tilted 40°, to the face each
        leaves through, however far), at least a cell, at most 7.5 units. Hair styled into
        thick armour (a cuirass standing off the body, big pauldrons) lies deep inside it and
        must still be found. A band reaching past the middle of a limb would take a point just
        past its far side, out of that side's outside band, for the inside, and a band across a
        wide cone of directions would do the same past a shoulder: hence half the thickness, and
        45°. A ray that leaves the mesh's box through no face is open (past a rim, a hem, an
        edge) and does not count. Rays stop at the box's edge. If none of the five closes, or the triangle is a sheet (one face of a thin pair,
        a two-sided material), nothing is solid behind it, and its band is 0. Before `0-5-1` an
        open ray counted as 15 units of solid, and the air above the iron cuirass's
        downward-facing collar rim read 7.5 units inside it.
    -   *Collision.* As TressFX's: a point nearer the surface than its clearance (or inside) is
        put back along the normal. The clearance is how far its target lies, between 0.15 and
        0.35 units, so a styled shape resting on the body rests as styled, and one styled into
        armour lies on the armour, however deep its target lies. Limiting that by the target's
        depth, so that hair styled deep into odd armour stays in it, was tried in `0-5-1` and
        dropped: legitimate targets lie as deep as the field reaches. A lock styled through the
        shoulder lies 4.4 units in and 5.8 while the head nods, and the head turned into a
        shoulder carries hair 4 in. A flat 3-unit cap left those 0.3 to 1.6 units inside the
        shoulder; a fade from 5 to 7.5 units, 0.41. A point more than 2.5 units deeper than its
        target (or the surface) is left alone: the
        field is more often wrong there (past a thin part, beyond its near side's reach) than
        the point is that deep, and pushing it out would throw it through the part.
    -   *Steps between frames.* The field is the body at the frame's end. A step part-way through
        the frame takes the point ahead by the surface's move over the rest of the frame,
        collides it there, and takes it back. The actor's root carries the point first (its move
        from last frame's pose to this frame's), and the surface's own move is read where that
        puts it: at a sprint the body moves 5 units in half a frame at 30 fps, and read at the
        point itself, out of the field's reach, the surface's move was missing, and so was the
        collision of every step but the frame's last. TressFX builds its field at each pass's
        pose; the `0-4-0` colliders rode their bones' poses at each step for the same reason (a
        collider a step ahead of its targets shook the hair).
    -   *Contact.* A pushed point moves on with the surface. It takes the surface's own move
        over the step, keeps 0.4 of its slide along the surface (TressFX's capsule friction), and
        loses its motion into or off it. VSP moves every point with the root each step (0.4 of
        the root's move at the defaults), its previous position too, so the velocity a point is
        left with is the surface's move less VSP's. Without that, hair on the back of a running
        body was driven into it by VSP's share every step (2 units at 300 units/s) and crept along
        it towards the shoulder; when the run stopped it went 3.4 units in (8.3 at 30 fps), past
        the 2.5 units a point is trusted to be deep, and through the shoulder. With TressFX's stop,
        hair on a moving body was shoved again every step: in the NumPy port of the `0-4-0`
        colliders it shook about seven times as often, by up to 1.3 units against 0.3. (Shaking:
        three or more frames running whose accelerations each reverse the last. Counted over
        breathing, walking, running and turning at 30, 60 and 144 fps.)
    -   *Thin parts.* A point that goes past the middle of a thin part in one step (a hair tip
        swinging into a 2-unit shield at 2 units a step, an arm sweeping through hair) reads the
        far side's surface, and the push would put it out through the part. So when a point reads
        inside, the field is also read where it began the step, carried with the surface: if the
        normal there faces the other way, the point goes back there, on that side. Only inside: in
        a crease (an arm against the side) the nearest surface changes sides with nothing gone
        through.
    -   *Followers.* Every drawn point but a strand's first two is kept off the field once more
        in `StrandSkin.cs.hlsl`, as far as its own target lies; its previous position against
        last frame's surface (this frame's, taken back by its move).
    -   *Rebuilds.* Every frame the actor's 3D is walked for what it wears: each mesh's geometry,
        skin, counts and a hash of eight vertex positions (body morphs move them). A change held
        for 10 frames builds the collision mesh again (the first at once); meanwhile the old one
        is used, its meshes no longer worn skinned to nothing far away.
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
    at the target's own direction. Guides collide with it after each step, on the head's pose
    at that step, as TressFX's signed distance field collision; every strand point is kept out
    once more after following its guide (`StrandSkin.cs.hlsl`, previous positions against last
    frame's head). On a synthetic head with eye holes (a mirror of the build and lookup), the
    field is within +0.13 units of the true surface everywhere (median +0.04). Without a head
    mesh skinned to the head bone, when it covers under half the directions, or when its
    median radius is not within 0.7-2× the head sphere's (the meshes do not share that
    centre), the head sphere is used as before; the reason is logged (info for the player,
    debug for others).
-   **Clamp.** Only the movable points: rewriting a pinned point's history would skew the next
    step's VSP.
-   **Restarts.** A guide restarts from its targets when it is first simulated, after more than
    2 frames unsimulated, when its asset changes, and (per strand, on the GPU) when its root
    moves further in a frame than 40 units or 4,000 units/s x frame time (teleports, loads).
    Non-finite state falls back to the target.
-   **Not simulated:** style `simulate` off, area-seeded (short) hair, or past the physics
    distance (the motion fades out over its last quarter). Such hair is plain skinning.

### Chains (hanging braids)

A braid hanging free is kept as cards and skinned to a chain of joints
(`CardsToStrands::ChainCurve`, at most 16 joints, 16 chains per hair), simulated on the CPU by
`CardsToStrands::ChainSimulator` in `StrandRenderer::SimulateChains`: on the strands' clock (the
same fixed 1/60 s steps), each step with the parent bone (the head) where it is at the step's
end between last frame's pose and this frame's. Joints lying on the head are pinned to it; the
others move with the head plus a velocity of their own relative to it: falling (`chainGravity`,
400 units/s²), thrown by 60% of the head's acceleration (at most 1600 units/s², so a snap turn
does not fling the braid) and damped (`chainDamping`, 0.08) relative to the head, so a walk or a
run carries the braid along instead of blowing it up behind the head. They are pulled towards the
styled shape (`chainStiffness`, 0.2 per step, on both ends of a segment as TressFX's local shape
constraint), kept at their segment lengths from the root down (DFTL), and pushed out of the head
sphere and the body's bone capsules by the braid's thickness (never further than the styled
braid lies, never less than half). The joints
become bones appended to the palette after the skin instance's, so the same compute and vertex
shaders skin the braid's cards and any strands growing from its end: those strands' points are
skinned to the chain, and `TargetSkin` takes their full skinning at any guidance, so the strand
simulation's targets ride the braid. State is camera-relative, shifted when the camera moves,
and restarts with the asset, after a gap of more than two frames, or when the root jumps further
than a strand's teleport distance. Physics off or out of its distance: the braid hangs as
styled. The body field (GPU) is not used: braids collide with capsules only.

### Followers (`StrandSkin.cs.hlsl`)

TressFX's follow hairs sit at their guide's position plus their root offset from it
(`UpdateFollowHairVertices`), scaled by `1 + tipSeparation x vertex / vertices`. Here every
strand is a converted strand of its own. A point sits at its own target plus its guide's drawn
offset from the guide's target, at the same distance from the root. That is the guide's
position plus the strand's rest offset from it, turned with the skinning. `tipSeparation`
scales that offset as TressFX does; 0 keeps each strand's own shape. The previous position
takes the guide's previous drawn offset, so motion vectors carry the simulated motion. Past its
guide's tip, a longer strand takes the tip's offset. The guide's rotation there (shortest arc
from the target's tangent to the drawn one) turns only the normal. Turning the offset from the
guide too (the `0-2-0` follow) made the offset a lever that amplified every bend of a short
guide (see the `0-2-1` notes at the top). After following, every point is kept out of the head
field, and all but the first two off the body's distance field.

### Settings

The per-style motion settings are TressFX's `TressFXSimulationSettings`, under its names, per
1/60 s step, with lengths in units:

| Setting | Straight | TressFX 4.1 sample (Ratboy mohawk, metres) |
| --- | --- | --- |
| `vspCoeff` | 0.4 | 0.758 |
| `vspAccelThreshold` | 1.208 units/step² | 1.208 |
| `localConstraintStiffness` | 0.908 | 0.908 |
| `localConstraintsIterations` | 3 | 3 |
| `globalConstraintStiffness` | 0.408 | 0.408 |
| `globalConstraintsRange` | 0.4 | 0.308 |
| `lengthConstraintsIterations` | 10 | 3 |
| `damping` (`dampingCoeff` in style files) | 0.068 | 0.068 |
| `gravityMagnitude` | 100 units/s² | 0.09 |
| `tipSeparation` | 0 | 0 |
| `clampPositionDelta` | 20 units/step | 20 (set in code) |

Plus `simulate` and `windResponse`, as before. The differences from the sample, all from the
NumPy port:

-   **`vspCoeff`.** 0.758 carries three quarters of every move of the head rigidly. A 40-unit
    lock then lifted only 4, 9 and 17 degrees 0.5, 1 and 1.5 s into a fall, as rigid as the
    hair the owner reported. At 0.4 it streams up (11, 111 and 150 degrees) and trails when
    running.
-   **Gravity and length passes.** TressFX's sample assets are in metres, so its gravity is
    about 1% of Earth's. Its constraints hold a shape against light gravity only: at Earth's
    gravity a 40-unit, 32-point lock rests 13% long with 3 length passes (22% without the
    stretch cap) and still 7% with 20. The stretch grows with gravity per step against segment
    length, which the even/odd passes leave on the even segments. At 100 units/s² (about
    1.4 m/s²; Earth's is about 687) with 10 passes, hair falls a little as the head tilts and
    rests about 1.6% long (a 3.8-unit, 14-point lock 0.7%: the global pull holds it all
    along). Passes are cheap: only guides take them.
-   **`globalConstraintsRange`.** With 0.308 a 40-unit lock streamed nearly level behind a
    300 units/s run, its tip 29 units off target. 0.4 keeps it at 22.
-   **`damping`** is the sample's 0.068. At 0.05 hair trails less when running (16 units for
    that lock) but swings longer: after a step aside a 20-unit lock's swings shrink to 0.42 of
    the one before, against about a third at 0.068, and it rises later in a fall (49 degrees
    1 s in, against 111).
-   **`vspAccelThreshold` and `clampPositionDelta`** keep TressFX's numbers, in units. In the
    metre-scale sample both are effectively off. In units, 1.208 per step² (about 62 m/s²)
    passes running (about 14 m/s² at the roots) and turning, and catches snap turns (82 to 216
    m/s²) and landings, so strands do not stretch on them. 20 units per step is 1,200 units/s.

State per instance: `HairStrands::GuideState`, 128 bytes per guide point (a 2,500-guide,
20-point hair is 6.4 MB). Per actor with body collision: its collision mesh (32 bytes a vertex,
16 a triangle: about 1.5 MB at the 65,536-triangle budget) and bone palette. Shared by every
actor: the skinned collision vertices (48 bytes each) and the field (20 bytes a cell, at most
512K cells: 10 MB), grown to the largest asked for. Positions are relative to the camera of the
simulation that wrote them and shifted by the camera's move each frame; offsets are camera
independent.

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
    `coverageThreshold`, `seed`, `excludeUV`, `keepWoven`, `chainUV`), render (`rootWidth`,
    `tipWidth`, `waveAmplitude`, `waveLength`, `curlRadius`, `curlLength`, `curlStart`, `frizz`,
    `flyaways`) and motion (`chainStiffness`, `chainDamping` and `chainGravity` for hanging
    braids, and `simulate`,
    TressFX's `vspCoeff`, `vspAccelThreshold`, `localConstraintStiffness`,
    `localConstraintsIterations`, `globalConstraintStiffness`, `globalConstraintsRange`,
    `lengthConstraintsIterations`, `dampingCoeff` (TressFX's `damping`), `gravityMagnitude`,
    `tipSeparation` and `clampPositionDelta`, per 1/60 s step, and `windResponse`; see
    [Settings](#settings)). Tooltips in the editor explain each field. Units are Skyrim units,
    about 1.4 cm. Motion fields apply live. The motion fields before `0-3-0` (`rootStiffness`,
    `tipStiffness`, `bendStiffness`, `damping`, `gravity`, `inertia`) are no longer read: saved
    styles take their hair type's TressFX settings. TressFX's damping is saved as
    `dampingCoeff` because the old `damping` (0.4: of the velocity relative to the head, per
    1/60 s) would read as very heavy air drag.
-   In-game editor: select a hair in view, edit it, and the change applies to every actor
    wearing it. Render fields apply live; generation fields apply when the slider is
    released. **Save** writes the fully resolved style, matched on that exact head part,
    model and shape, to `UserStyles.json`.

## Hair types

| Preset | What it changes |
| --- | --- |
| Straight | defaults: light clumping, little frizz; TressFX settings as in [Settings](#settings) (VSP 0.4, local 0.908 x 3, global 0.408 over 0.4, length x 10, damping 0.068, gravity 100) |
| Wavy | per-lock sine waves (period 4); keeps its shape a little more firmly (local 0.93, damping 0.075) |
| Curly | helical curls (radius 0.35, period 1.6), strong clumping, so locks spiral together as ringlets; springy (VSP 0.5, local 0.95 x 4, global 0.45 over 0.5, damping 0.08, gravity 75) |
| Coily | tight coils from the root (radius 0.18, period 0.45), little clumping (a cloud rather than ringlets), high volume and frizz, denser and thicker strands so the scalp does not show; holds its shape (VSP 0.7, local 0.95 x 4, global 0.6 over 0.8, damping 0.15, gravity 50, wind 0.4) |
| Locs | clump pull 0.95 with twist: strands collapse into twisted ropes (locs, braids, twists); heavy (VSP 0.3, local 0.85, global over 0.3, length x 12, gravity 150, wind 0.6) |

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
    signature must match the strand PS input signature. Compile `CardLighting.hlsl` the same
    way (VS and PS, without `HAIR_STRANDS`), and `CardDepth.hlsl` as `ps_5_0`; its input is the
    first two outputs of the card VS. Compile `StrandSkin.cs.hlsl`,
    `StrandSim.cs.hlsl` and `BodySdf.cs.hlsl` (entry points `SkinVertices`, `Splat` and
    `Finalize`) as `cs_5_0` with `-I package/Shaders -I "features/Hair Strands/Shaders"`.
    `triangle` is a reserved word too.
    fxc rejects partial writes to `Guides[]` fields inside a branch (`X4532`), so the sim
    writes each guide point whole, once. HLSL `for (uint i ...)` leaks `i` into the function
    scope, so the sim declares its indices once. `point` is a reserved word. fxc does not
    short-circuit `&&`: guard a call with side effects with `if`. Off Windows, DXC for Linux
    checks the compute shaders' syntax (`dxc -T cs_6_0 -HV 2018 -E main` with the same
    includes), and `-Fc` lists the `SkinCB` offsets to compare with `Strands::SkinCB`. It
    does not replace fxc.
-   Solver: `python tools/hair_strands_sim_check.py` (a few minutes on 4 cores) runs a NumPy
    port of `StrandSim.cs.hlsl` and `StrandSkin.cs.hlsl` on the clock `BeginFrame` keeps, for
    every preset. Locks of 1.5 to 40 units hang from a head that stays still, falls off a
    cliff, sprints and stops, turns, bows, steps aside, tilts (up to a head on its side) and
    turns into a shoulder capsule and the head sphere, at 30 to 240 fps and with jittered
    frame times. It fails if hair at rest sags or stretches past a limit or does not come to
    rest (the `0-2-2` solver fluttered on a tilted head). It builds the body's distance field as
    `BodySdf.cpp` and `BodySdf.cs.hlsl` do (inside bands from the five thickness rays, the splat
    and its cone, the sign, the filtering) from three synthetic bodies, each one closed skin
    (marching tetrahedra over overlapping parts, about 1.25 units between vertices, as the
    collision mesh): a bare body; a cuirass standing 2.5 units off the torso with no body under
    it, as Skyrim armour replaces the body; and the cuirass with a shield on the back, 4 units off
    it and 2 thick. A field fails if, within 1.5 units outside, it is off by more than 0.45 units
    (1st and 99th percentiles), if more than 1% of points up to 3 units outside read inside, or if
    more than 6% of points up to 6 units inside do not. It also fails if hair styled through the
    shoulder does not come to rest over it, if hair styled on the back does not rest as styled,
    or if the same hair in the cuirass (2.5 units inside it) does not come to rest on it. A fourth
    body has an open, single-sided collar standing off the back of the neck, its own source mesh:
    a band flaring out as it rises and a lip facing down at the top, as the iron cuirass's is
    after the sheet rule. A short lock hanging 1.5 to 7 units above the lip must read outside
    everywhere, rest as it does with no body (within 0.1 units) and not shake while the body
    breathes. Under `0-5-0`'s rules it read 6.3 units inside and ended 6.0 units off its style.
    Turning or tipping the head into the shoulder must leave hair out of it and at rest. Breathing,
    walking, running and turning at 30, 60 and 144 fps must leave drawn hair no more than 0.35
    units deep, on the bare body and, for a 45-unit lock down to the waist, in the cuirass with
    the shield, and it must not shake: three or more frames running whose accelerations each
    reverse the last, by more than 0.35 units. The bodies move rigidly there, so the field is
    built once in the body's frame and carried along, as the GPU's grid on the actor's axes is;
    the surface's motion comes from the body's pose this frame and last, and the body's pose is
    the root's that carries hair through the steps between frames. It also fails if motion differs
    across frame rates, if long straight, wavy or locs hair does not stream up in a fall, or
    if any motion does not settle. 1.5- and 3.8-unit scalp locks must stay within 10% of
    their length of target through one step aside and a 15° turn. Once the head stops, a
    lock's swing must die down as a pendulum's does: the tip turns back no sooner than 0.25 s
    after it last did (the `0-2-4` wave turned it back 12 times in 1.5 s), each swing of 0.05
    units or more is at most 0.6 of the one before, and the lock is still after 3.5 s. A fringe
    must stay out of an ellipsoid head field walking and sprinting. Wind, extreme settings
    (every slider at either end) and followers beside and longer than a 3.8-unit guide are
    checked too. A follower may not stray further than its guide, nor change length by more
    than 10% (15% in snap turns). Port solver and follow changes to it first, then tune.
-   Converter: `python tools/hair_cards_to_strands/check.py` (needs numpy and scipy; a few
    seconds) builds the `CardsToStrands` module with the host compiler and converts synthetic
    hairstyles: layered long hair, a ponytail, a bob and a buzz cut, with presets, no texture,
    area seeding, wide clumps and another seed. It fails if under 97% of strands root on the
    skull, over 1% float more than 2 units off it, over 5% are stubs, over 1% of points sink
    into it, 5% of points stray over 2 units from the cards, under 85% of the painted cards are
    covered, or the detached stray piece grows strands. `--render DIR` draws each style;
    `--compare EXE` runs another build beside it. See
    [Hair cards to strands](hair-cards-to-strands.md#tools). Before `0-6-0` the generator
    built on its own with a small shim (SimpleMath, a `logger` stub, `RE::BSGeometry`
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
-   Flow (`0-3-0`): the same reader also dumps every texture slot and the back-lighting flag;
    feed the harness the diffuse at its readback size (alpha, and luminance × alpha) and the
    flow map's RG. Score each triangle's flow two ways. Vanilla hair has flow maps: run it
    without them and compare. Hair more than 4 units below the skull centre must run down.
    Then look at the triangles whose flow changed, over a textured render of the mesh: a
    card's own shape and its texture can disagree, and the arrows are easy to misread, so
    measure the flow's direction over a region (mean y for front-to-back) before judging.
    Checked on 2026-09-29 against 11 KS and Apachii long hairs, ponytails and braids and 4
    vanilla hairs (numbers above); hanging hair runs down on 90-100% of its area on all of them
    but Apachii hair 79 (71%: its tail curls sideways).

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
-   Physics (`0-3-0`): the TressFX port and its defaults are from the NumPy port, not seen
    in game. With `gravityMagnitude` 0 still hair sits exactly where it did without physics
    (a visible offset means the follow pass is off); with gravity, it sags slightly. Then jump
    off something high: long hair should stream up by the end of a long fall. Lower
    `vspCoeff` gives more swing and lag; higher is more rigid (TressFX's sample: 0.758).
    Raising gravity beyond a few hundred stretches strands unless `lengthConstraintsIterations`
    rises with it. Short scalp strands should keep their shape while long hair swings: short
    hair that flops or kinks would point at `ShortStrandLength` (10 units) in the shader.
-   Wind: TressFX's force grows with a segment's length squared, so the 125 at full weather
    wind is tuned for segments of about 1 unit. Hair converted with long segments feels
    more wind.
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
-   Body collision (`0-5-0`): the player's log should say `body collision from N meshes: V
    vertices, T triangles (… cells)` a moment after loading and after every change of what is
    worn (debug level for other actors). Cells over 1.25 units mean the outfit was over the
    triangle budget; `K over the budget left out` means it still was, and meshes are missing. `no body collision (…)` names why the bone capsules are
    used instead, and `body collision skips …` (debug) a mesh that could not be read. The
    statistics show `Body collision: N characters, T triangles, F fields a frame`, and RenderDoc a
    `Hair Strands Body Field` event (three dispatches) before each simulated actor's hair.
    Assumed: worn meshes keep their CPU vertex data (as the hair's and head's do) or their GPU
    vertex buffer has the same layout (read back otherwise); rigid meshes' `rendererData` holds
    their vertices and 16-bit triangle list, on the CPU or the GPU; armour, shields and weapons
    hang under `Get3D(false)`, their skin instances' bones the actor's skeleton nodes; the root
    node moves and turns with the actor (the grid's axes, and what carries hair through the steps
    between frames) and its world scale is the actor's. Hair lying off
    the body by a constant gap, or into it, points at the skinning (bone world × skin-to-bone, as
    the hair's), or at a mesh's vertex normals facing in (the sign). A mesh whose normals face
    in, or thin plates thicker than 0.6 units modelled one-sided, push hair to the wrong side.
    Cost: aim for well under 0.3 ms of GPU time per simulated actor; check the event in a
    capture. The splat is one thread per triangle over up to a few hundred cells.
-   The bone capsules' radii (neck 3 to back 6.5), used only until an actor's collision mesh is
    built, or when none can be, are guesses meant to sit inside any body. Hair floating off the
    shoulders means they are too large; hair through them, too small.
-   The head field (`0-2-7`): the player's log should say `head collider from head mesh …
    (N% of directions)`, with N around 85-90. The Face head part hangs under the face node by
    its editor ID, its CPU vertex data is kept (as the hair's), and its skin lists
    `NPC Head [Head]`. Hair pushed off the head by a constant gap, or into it, would mean the
    bind-pose chain (hair skin → head bone → head mesh skin) is off.
-   Flow maps (`0-3-0`): a vanilla hair patched by PGPatcher should log `flow map on N% of
    the hair` with N near 100 (98-100 in the harness), and a hair without one no such
    suffix. Strands growing from the tips on such hair would mean the map's sense is not
    tip to root. The map is decoded as stored, so a map in an sRGB format would read
    differently from Hair Specular, which samples it linearised.
-   `RE::GetSecondsSinceLastFrame()` is real frame time, and `UI::GameIsPaused()` covers
    menus. Slow-motion kill cameras may play hair at full speed.

## Not done (candidates)

-   Hair gathered into a tie, and braids. Since `0-7-0` gathered hair, ties and braids keep
    their cards and hanging braids swing on chains (see [What converts](#what-converts)); a
    ponytail's tail grows from its tie. Untested in game. Apachii hair 79's nape hair, which runs
    down from its tie, is kept as cards with the tie's band. Misses on the real meshes and their
    causes are in [Hair cards to strands, Limits](hair-cards-to-strands.md#limits).
-   Long loose hair built from separate segments. `0-6-0` continues a segment from the hair
    ending where it starts, or merges it onto the hair it starts on; untested in game. Joining
    strands to "a card just ahead that runs the same way" does not work: 93-100% of ordinary
    strand tips have one within 0.75 units (a longer neighbouring layer).
-   Braids twisting about their own length, chains colliding with the body field (they use the
    bone capsules), and swinging braids' shadows (cast by the game's cards where they were
    styled).
-   Strand shadow maps and self-shadowing beyond Hair Specular's, and deep opacity maps.
    Cards cast the shadows.
-   Hair-hair collision.
-   The body field reaches 2.5 units in front of a surface and up to 7.5 behind it. Hair whose
    styled place lies deeper inside armour than that, or that gets deeper than 2.5 units below
    its target in one step, is not pushed out. Just inside that reach a target is pushed all
    the way out; just past it, not at all.
-   A surface with nothing solid behind it (an open shell, a sheet, a two-sided material) has
    no inside: hair that crosses it in one step stays on the far side. Hair styled through such
    a surface is not lifted onto it. A one-sided cuirass is solid only where one of the five
    rays meets its far side; near its neck, arm and waist openings all five can escape, and it
    has no inside there.
-   Layers worn over each other: the field takes the nearest surface's side. Between a cloak
    and the cuirass under it, a point nearer the cuirass is outside: hair styled on the body
    there stays under the cloak where the gap is wider than its clearance.
-   Which way a sheet faces is a guess (away from the body's long axis, by area). A cape whose
    triangles mostly face the body puts hair under it.
-   A sharp concave crease (an arm pressed to the side) can read inside just outside it: the
    sign from interpolated vertex normals is least sure there.
-   The rebuild waits for what is worn to hold still for 10 frames, and a body morph that leaves
    the eight hashed vertices of each mesh where they were goes unseen until the next change.
-   Wind from anything but the weather (spells, dragons, player speed beyond air drag).
-   Loading TressFX's own `.tfx` hair files (and `.tfxbone` skinning) as an alternative to
    converting cards. The simulation already is TressFX's. A `.tfx` file holds guide strands of
    a fixed point count (float4 positions, `w` the inverse mass, the first two points 0), and
    a `.tfxbone` file holds bone names and four bone weights per strand. Positions would need
    TressFX's up axis and scale turned into Skyrim's, bones mapped by name to the skin
    instance's, and follow strands generated (`GenerateFollowHairs`, where `tipSeparation`
    matters).
-   The bone capsules (the fallback) still use TressFX's stop response and this frame's pose
    for every step. Both made hair shake against the `0-4-0` body colliders before they were
    changed.
-   Wigs have no model path in their key (no head part), so they match on shape name and
    vertex/triangle count.
-   Actor fade-out keeps the cards: strands have no alpha to fade with.
-   Beards and other facial hair keep their cards. They would need their own flow and
    density rules, since beard cards lie flat on the skin.
-   Model-space-normal hair permutations keep their cards.
-   Effect shaders on strand hair (magic effect membranes, Dirt and Blood) are hidden, not
    drawn on the strands. That needs a strand variant of `Effect.hlsl`, as
    `StrandLighting.hlsl` is of `Lighting.hlsl`, with its own membrane permutations.
