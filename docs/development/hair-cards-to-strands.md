# Hair cards to strands

`src/Features/HairStrands/CardsToStrands/` turns a textured hair-card mesh into strand curves
that grow from the scalp. Hair Strands runs it in game (`StrandGenerator.cpp` adapts the game's
mesh and style to it). It depends on the C++ standard library only (its own `Vec2`/`Vec3`, no
Skyrim, DirectX or logging types), so standalone tools can build it as it is: the test harness in
`tools/hair_cards_to_strands/`, and a hair designer.

Hair Strands' own pipeline and the earlier stages (welding, flow, tracing) are described in
[Hair Strands](hair-strands.md#conversion-algorithm-strandgeneratorcpp). This page covers the
module as a whole and how strands reach the scalp.

## The problem

Up to `0-5-1` the generator seeded a strand on every upstream card edge the flow entered, and
along streamlines through whatever triangles that missed. A card's upstream edge is often not on
the scalp: an outer layer standing off the cap, an under-layer, a ponytail below its tie, a side
lock tucked behind the ear, the start of a lock past a transparent gap in the texture. Every one
of those grew strands from mid-air, pinned to the head bone. On the synthetic layered style
(below), 26% of strands started more than 2 units off the skull, 11% were stubs under 2 units,
and a detached piece of card grew 77 strands of its own. A ponytail's tail was a separate wisp
hanging under the head (28% floating).

## Prior work

- **HairCS** ([arXiv 2609.16465](https://arxiv.org/abs/2609.16465), "Reconstructing
  Strand-Based Hair from Hair Cards") is the closest match. Per its abstract: it extracts each
  card's flow orientation, root side and centreline as a guide; binds guides to root
  candidates Poisson-sampled on the scalp (30,000 of them) by integer programming; inflates
  each guide into a "wrapper" whose cross-section matches its share of the scalp; and fills
  every wrapper with a clump of strands. It names the same problems: card roots are not spread
  over the scalp, flat cards have to become a volume without voids or crowding, and partings
  and layers must survive. The full text could not be read from this environment, so the
  details here are our own.
- **Hair Tool** for Blender ([Convert Hair](https://joseconseco.github.io/HairTool_3_Documentation/convert_hair.html))
  extracts one curve per card from its UVs, which must run root to tip, with a flip switch.
  That is the old per-card approach: nothing ties a curve to the scalp.
- **Ornatrix** [Guides from Mesh](https://ephere.com/plugins/autodesk/maya/ornatrix/docs/1/Guides_from_Mesh.html)
  plants roots on a mesh and grows guides from them; [Hair from Guides](https://ephere.com/plugins/autodesk/maya/ornatrix/docs/2/Hair_from_guides.html)
  fills a volume of strands round the guides.
- **Guide interpolation.** Grooming tools make dense hair from guides by clump interpolation (each
  strand follows one guide through a transported frame) or prism interpolation (three guides,
  barycentric), as surveyed in [Real-Time Physically Guided Hair Interpolation](https://graphics.cs.utah.edu/research/projects/physically-guided-hair-interpolation/hairinterp-siggraph2024.pdf)
  (SIGGRAPH 2024). Unreal's groom ([reference](https://docs.unrealengine.com/4.27/en-US/WorkingWithContent/Hair/Reference))
  and TressFX simulate guides and carry the strands round them.
- Learned methods ([DiffHairCard, arXiv 2505.18805](https://arxiv.org/abs/2505.18805),
  [CGHair, arXiv 2604.03716](https://arxiv.org/abs/2604.03716), [Strands2Cards](https://dl.acm.org/doi/10.1145/3757377.3763864))
  go the other way, strands to cards, or reconstruct hair from images; none fits a
  deterministic, millisecond conversion.

This module follows HairCS's outline (guides from cards, bound to the scalp, a clump per guide)
with cheap geometric steps in place of its two optimisations. It has to run in tens of
milliseconds on a worker thread and give the same result every time.

## Inputs

`CardsToStrands::CardMesh`:

| Field                                    | Meaning                                                                    |
| ---------------------------------------- | -------------------------------------------------------------------------- |
| `positions`, `normals`, `uvs`, `indices` | the cards in bind pose; Skyrim units, +Z up                                |
| `boneIndices`, `boneWeights`             | four influences per vertex (optional; strands copy them)                   |
| `boneNames`, `boneBindPositions`         | the skull centre is `NPC Head` + 5 units up; else the mesh's top           |
| `coverage`                               | the diffuse's alpha (and luminance, for the painted-streak flow), any size |
| `flow`                                   | a flow map (RG = tip-to-root in texture space), optional                   |

`CardsToStrands::Settings`: `density` (strands per unit of card width), `clumpSize` (card width
per guide, so per clump), `clumpStrength`, `clumpTwist`, `volume`, `layerJitter`, `segmentLength`,
`lengthScale`, `tipVariation`, `coverageThreshold`, `seeding` (Auto, Scalp, Area), `flowAxis`,
`seed`, and what stays cards (see [Hair that is not loose](#hair-that-is-not-loose)):
`keepWoven`, `excludeUV`, `chainUV` and `triangleRegions`. Hair Strands maps its style onto these
in `ToSettings`.

`CardMesh::tangents` and `bitangents` (optional) are the tangent frame the cards' normal map is
read in, as Skyrim's Lighting vertex shader builds it; only the cards kept as cards use them.

## Outputs

`CardsToStrands::Result`:

- `points`: `StrandCount() × pointsPerStrand` points (position, normal, uv, four bones and
  weights, `t` along the strand), every strand the same point count.
- `strands`: length, a random value, the simulated guide it follows, the clump's random value,
  the card guide it came from (`cardGuide`) and whether it grows from the scalp. Shuffled, so
  any prefix is an even thinning; the first `guideCount` are the simulated guides.
- `guides`: the card guides left after dropping repeats, as bound (root to tip, the root on
  the scalp), what became of each (`GuideKind`: rooted, merged, bridged, continued, dropped,
  or free with area seeding) and how many strands grew round it. A tool can show or edit
  them.
- `scalp`: the fitted scalp (centre, sphere radius, a radius per direction). `Height(p)`
  and `Project(p)` work against it.
- `headBone`, `headCentre`, `headRadius`: the head collider.
- `triangleRegions`: what each mesh triangle became (`Region`: Strands, Cards, Chain).
- `chains`: the hanging braids, each a `ChainCurve` (joints in the bind pose, how many lie
  pinned on the head, the braid's thickness, the bone it hangs from, its first bone number).
- `ties`: where hair is gathered and tails grow from (a ponytail's tie, a braid's end).
- `cardVertices`, `cardIndices`: the triangles kept as cards, as a mesh to draw, with the
  chain triangles skinned to their chain's joints. Joints are bones `chainBoneBase` on.
- `stats`: triangles, flow-map share, what happened to the card guides (traced, repeats
  dropped, rooted, continued, merged, bridged, dropped, gathered, tied), woven pieces found,
  and the triangles kept as cards and on chains.

## Algorithm

1. **Card analysis** (unchanged from `0-5-1`, moved here): weld, adjacency, sheets, back-face
   drop, flow per triangle (flow map, painted streaks, or a UV axis), root/tip votes, coverage.
2. **Fit the scalp** (`FitScalp`). The painted hair is sampled (15 probes a triangle, by area).
   Directions from the skull centre are binned (24 azimuth × 12 elevation); below 0.2 under the
   horizon is ignored, because hair hangs free there. In each bin the innermost 10% of hair lies
   on the scalp (the cap's cards, or the cards nearest the skin). A sphere is fitted to those
   inner points by linear least squares, its centre pulled towards the skull centre (weight 0.1
   of the data, at most 4 units off it) and refitted once without bins more than 15% off it.
   Each direction's scalp radius is its inner hair, held within 0.8 to 1.15 of the sphere's
   radius, then smoothed twice over its neighbours. Too little hair over the head: a sphere
   through the innermost 10% of the hair.
3. **Card guides.** Streamlines traced along the cards from upstream edges and through
   triangles they missed, trimmed to the painted hair, as before, one per `clumpSize` of card
   width (clamped to 0.4 to 2 units). `seeding: auto` switches to area seeding when half the
   traced length lies in guides under 1 unit. That is weighted by length, not a median count:
   dense long hair traces far more short streamlines than long ones. Apachii hair 09 (33,000
   triangles) traces 7,288, 7,005 of them repeats or fragments, and its median guide is under
   1 unit; by count it came out as 35,000 strands 1.1 units long, by length as 8,020 strands
   of median length 27.8, all rooted on the scalp.
4. **Drop repeats** (`PruneRedundant`), longest first and within one sheet only (overlapping
   sheets are layers, each carrying its own hair). A guide goes if over 80% of it runs within
   half a spacing of a kept one, or if it is a fragment: shorter than 0.35 of its sheet's long
   hair (90th percentile) and over half of it within a spacing of a kept guide. Fragments are
   streamlines from a card's side edge, or the rest of a lock past a transparent gap.
5. **Bind to the scalp** (`BindToScalp`).
    - A guide starting within 1.25 units of the scalp is **rooted**. If it starts above it, a
      root on the scalp a little upstream is added, so hair leaves the scalp at a slant.
    - Any other guide is floating. Up to 8 passes, so that hair can continue hair that itself
      continues hair:
        - **Continued.** Bound hair whose tip ends within 2.5 units of where the floating guide
          starts, running the same way (cosine ≥ 0.3), carries on into it: each such guide's
          end is eased onto the floating guide's start and the floating guide's path appended.
          Every lock gathering into a ponytail's tie continues down the tail, rather than the
          tail hanging off one of them. The floating guide itself grows no strands.
        - **Merged.** Otherwise the guide continues the bound hair it starts on: the sample
          within 4 units with the lowest distance + 3 × (1 − cosine), running the same way
          (cosine ≥ 0.3) and not behind it. Its path becomes that hair up to there, eased over
          to its own start, then its own. An under-layer grows from the scalp under the layer
          above it.
    - What is left joins the scalp along a Hermite curve if it starts within 5 units of it
      (**bridged**) and is dropped otherwise: hair has to come from the head.
6. **Grow strands** (`BuildStrands`). Each bound guide is one clump: `density × spacing`
   strands, fewer (down to 35%) where its painted coverage is thin, 40,000 at most.
    - Roots: half at the guide's scalp root, spread across (and along) it over half a spacing
      (wider, up to 4×, when several guides share one root), and only over scalp that rooted
      hair covers, so none crosses a hairline or parting. The other half spread along the
      stretch the guide lies on the scalp, up to 0.6 of its length: hair combed back from the
      hairline grows from under the hair before it, not all from the hairline.
    - Shape: the guide's path plus an offset in its frame (tangent, the card normal away from
      the head, across). Across the card, strands are stratified over the spacing, so they keep
      their order from root to tip. The lift is layer jitter plus `volume` towards the tip.
      `clumpTwist` turns the offset round the tangent, and `clumpStrength` pulls it in towards
      the tip. The difference between the scalp root and the guide's start is blended out over
      the first 2 units (at most 30% of the strand).
    - UVs follow the offset across the card (through ∂P/∂U and ∂P/∂V, at most 0.25), so a clump
      shows the card's painted strands; bones, weights and normals come from the triangle
      under the guide. Strands end up to `tipVariation` short of the guide's tip.
7. **Shuffle, simulated guides, head collider**: as before (see
   [Hair Strands](hair-strands.md#conversion-algorithm-strandgeneratorcpp), steps 7 and 8).

Area seeding (buzz cuts, stubble) grows short strands only on triangles within 1.25 units of the
fitted scalp, each its own guide.

## Hair that is not loose

Strands suit loose hair. A braid traced as strands follows each lobe of the plait and falls
apart into a mess of short clumps, and the hair pulled tight into a ponytail's tie stands off
the head and swings loose. So the conversion finds the parts that are not loose hair and keeps
them as cards (`Region::Cards`); braids hanging free swing on a chain of rigid segments of
their own (`Region::Chain`); and loose hair below a tie or a braid's end grows from there.
`keepWoven` (on by default) turns this off; then, with no region chosen by hand, the result is
exactly that of `0-6-0`.

### Finding woven hair (`FindWoven`)

Braids in Skyrim meshes are geometry, not texture: KS, Apachii and BG3 braids use the same
strand texture as the loose hair, and only vanilla's atlas paints plaits. Measured on the test meshes,
each sheet (welded component) gets a shape from its painted part: its principal axes (spreads:
twice the standard deviation along each), which ways its surface faces round its long axis
(normals folded into six directions), how closed its cross-sections are (of twelve directions
round the axis in 1.5-unit slices, the share holding hair), and how its hair runs along it.

| Shape | Test | Is |
| --- | --- | --- |
| Tube | faces 5 of 6 ways round its length, cross-sections 75% closed, 1.5x longer than thick, hair running along it (0.6), at most 2.75 thick (4.5 if its texture shows a plait: streak coherence under 0.4) | a braid or dreadlock built as a tube |
| Twist | as a tube, but open (an arc) | a plait's twisted strand, or a curl |
| Ring | under 4 wide, flat, facing out round its axis, closed round it | a tie or band |
| Lobe | under 4 long, faces 3 of 6 ways | a plait's lobe, or a bun's piece |

A curl's ribbon spirals round an empty axis, so it is open (a twist) or too thick: ponytail
curls (Apachii 79) and a ponytail rolled into one cone of cards (Apachii Saram, pony 2: 2.9 to
4.0 thick) stay loose. Flat cards never face more than one or two ways, so a layered cut does not
read as a ring however its tiers curve (KS Tails: 33 flat pieces passed the ring test until it
checked which way they face).

Woven sheets in contact (0.75 units) form groups. A group is woven if it is slim, under 2 units
wide where you look at it (the median, over its triangles, of the second spread of the hair
within 3 units), and holds a tube, or at least four lobes of which 30% hang free. Lobes lying on
the head are as often a layered cut as a plait, and keeping a layered cut as cards would lose
all its strands, so only tubes make woven hair on the head. A wide group (a curtain of
dreadlocks, a mass of BG3 curls, measured 2.13 to 2.38) keeps only its long hanging tubes, each
on its own. A ring near the scalp (3 units) is a tie by itself. Small sheets touching a woven
group over 60% of their samples, with under half its area, join it (a plait's lobes built as
flat strips).

A piece is then split: what lies on the head (within 2 units of the scalp, and above the skull,
direction z from its centre above -0.6) stays with the head bone as cards, and each connected
hanging part becomes a chain if it is at least 3 units long past its pinned joints and its end
lies below its root (a bun on top does not swing). A hanging part that is not slim (braids
hanging side by side and touching) splits into a chain per sheet. Chains go to the biggest parts
first, 16 at most, 16 joints each.

**The chain** (`BuildChain`) follows the piece's centre line from its end nearest the scalp:
triangle centres linked within 1.5 units, Dijkstra from the lowest, binned by distance through
the piece in 1-unit steps, the area-weighted centre of each bin, smoothed twice, resampled at 2
units or more. Joints lying on the head (within 1 unit and above the skull) are pinned to the
parent bone (the dominant bone at the root). Its radius is the median distance of the piece's
triangles from the centre line. Card vertices on a chain blend the two joints of their nearest
segment by where they project on it.

### Gathered hair, ties and tails (`BindToWoven`, `FindTies`)

Woven triangles take no part in tracing, so loose hair stops at them and hair below a braid's
end starts there. Before binding:

- **Gathered.** A rooted card guide ending at woven hair on the head (a braid along the scalp,
  a tie, the top 2 units of a chain), or inside a hanging braid (within 1.5 radii + 0.5 of its
  centre line), is gathered into it: pulled tight to the head, it stays cards. So is one
  starting there and lying on the head all the way: on a tight cap the cards run either way
  (Apachii Navi's cap runs from the band to the forehead). A guide that merges onto gathered
  hair is gathered too (a layer under the cap).
- **Tied.** A floating guide starting at woven hair grows its strands from there: a braid's
  tuft below its end (only past 70% of a chain's length; strays along a braid bind as usual),
  or hair below a woven tie. One that runs back onto the head from there is gathered instead.
  Strands of a guide tied to a chain are skinned rigidly to its joints at their root, so they
  ride on the braid, and the simulation's targets follow them (`TargetSkin` counts chain
  weights at full guidance).
- **Ties without a band** (`FindTies`). A floating guide that rooted hair runs into (as
  continuation finds it) is a tail. Tails starting within 2.5 units of each other share a tie
  if at least four card guides gather into it from roots at least 4 units apart, converging (their
  roots twice as far apart as their tips), with every tail within 2.5 units of its centre, at
  least six tail card guides, and the tie within 3 units of the scalp. A layered cut's lower
  tier carrying on from the one above fails on convergence (its hair runs side by side) or on
  its few tails (the synthetic curly and coily presets found false ties with three and four). The feeders are
  gathered, and so is all hair lying on the head that runs to the tie (its tip or root within
  the tie's radius + 2.5, or its last segment pointing at it): a cap's front row stops short of
  the tie. The tails, and any other floating guide starting at the tie, are tied: their strands
  root round their own start (half a spacing across the card and off it).

Card guides dropped for starting far from the head keep their cards now (before, they vanished
with the rest of the cards).

### Regions of every triangle (`LabelTriangles`), and the kept cards (`BuildCards`)

Woven triangles take their piece's region. Every other converted triangle goes with the card
guide nearest it on its sheet (within 2 spacings, its own path before binding extended it):
gathered and dropped guides keep their cards. Triangles the conversion skipped take the region
of their copy on the same vertices, or of the kept sheet whose back face they are, or of the
nearest labelled triangle within 1.5 units. `cardVertices` copies each kept triangle's vertices
(per chain, so a vertex shared with a chain triangle is skinned twice), with the mesh's tangent
frame (or one from the UVs) and its bones, or two joints of its chain.

### Choosing regions by hand

`Settings::triangleRegions` sets any triangle's region (`RegionChoice`: Auto, Strands, Cards,
Chain); it wins over everything. `excludeUV` and `chainUV` rectangles set Cards and Chain by
the triangle's UV centre. Triangles chosen as Strands are never woven. Triangles chosen as Cards
or Chain form pieces of their own (connected sets); a Chain piece is one chain, pinned where it
lies on the head, and stays cards if it does not hang. A designer can show `triangleRegions`,
let the user paint `RegionChoice`s per triangle (or pick a piece and set all its triangles), and
convert again: triangle indices do not change with the other settings.

### Chains in motion (`ChainSimulator`)

The chain is simulated on the CPU, in the same module so a designer can preview it: points at
the joints, the pinned ones carried by the parent bone. Each free joint moves with its target
(where the parent bone alone carries it) plus a velocity of its own relative to it, Verlet-style:
gravity, `inertia` (0.6) of the target's acceleration taken against it, at most `maxInertia`
(1600 units/s²), and `damping` of it lost per 1/60 s step. Damping the velocity relative to the
head, not in the world, matters: a braid on a running actor otherwise meets a headwind of
`damping` x speed per step, about 1400 units/s² at a run against gravity's 400, and streams up
behind the head. Then `iterations` passes pull each segment towards its styled direction as the
segment before it carries it (TressFX's local shape constraint: half the pull on each end, the
pinned ones excepted); `iterations` passes keep each segment's length moving only the joint
further from the root (follow the leader) and push joints out of capsule colliders by the braid's
radius, but never further than its styled place lies, and never less than half of it. What the
constraints moved becomes velocity, less `dftlDamping` (0.9) of the length correction of the
joint below (DFTL, Müller et al. 2012), without which follow the leader adds energy and the
chain swings by itself. `Bones` gives each joint's skin-to-world transform: the bind pose turned
with its segment and carried to the joint as drawn, the parent bone as it is now plus the offset
simulated at the last two steps blended by the frame's position between them, so a chain follows
the head smoothly between steps.

`tools/hair_cards_to_strands/chain_check.cpp` checks it against a head at rest, running,
starting and stopping, spinning, shaking, running while bobbing, and turning (in 0.15 s, and in
one frame). A 36-unit braid sags 2 units under gravity and holds still; no motion lifts a joint
more than 11 units above its styled place (the 0.15 s half turn; under 1 for running, starting
and stopping, a spin and a snap turn); segments keep their length within 0.3%; joints stay out of
the head and neck; the chain comes to rest within two seconds of the head stopping. The `0-7-0`
simulator, which damped world velocity and pulled only the lower end of each segment, failed
all but rest: braids rose up to 50 units above their styled place (above the head), stretched
segments by up to 88% on a snap turn, and were still swinging two seconds after a run stopped.

## Results

`python tools/hair_cards_to_strands/check.py` on the synthetic styles in `synth.py` (a skull
ellipsoid of radii 6, 7.2 and 7.6 with layered cards on it), compared with the `0-5-1` generator:

| Style                                                                                                                 | Floating roots (before → now) | Stubs under 2 units | Median length |
| --------------------------------------------------------------------------------------------------------------------- | ----------------------------- | ------------------- | ------------- |
| layered long (cap, curtain, outer layer 1.8 off, floating under-layer, side locks, fringe, stray piece, texture gaps) | 26.5% → 0%                    | 11% → 0.7%          | 10.0 → 19.5   |
| ponytail (cap gathered into a tie, tail)                                                                              | 28% → 0%                      | 15% → 0.1%          | 9.5 → 30.7    |
| bob                                                                                                                   | 6.1% → 0%                     | 15% → 0.8%          | 8.0 → 8.6     |
| buzz (area seeding)                                                                                                   | 0% → 0%                       | short on purpose    | 0.5 → 0.5     |

The stray piece now grows no strands (77 before). Every case (four styles, the curly, coily and
locs presets, no texture, area seeding, wide clumps, another seed) roots at least 99.7% of
strands within 1 unit of the skull (97% with area seeding, whose roots lie on the cards up to
1.25 units out), puts at most 0.1% of points more than 0.3 units inside it, and keeps 95% of
points within 0.6 units of a card (0.85 with the coily preset's volume). Outside area seeding,
strands cover 88-100% of the painted cards. A conversion takes 40-180 ms. Area seeding the long
style (23,000 strands) takes 0.9 s, and a dense 14,000-triangle test 0.5 s.

With [Hair that is not loose](#hair-that-is-not-loose) (`0-7-0`), every synthetic case converts
strand for strand as before, with `keepWoven` on or off: none has woven hair, and only the
layered style's stray piece (24 triangles) is now kept as cards rather than dropped. The
ponytail without its texture finds its tie: the cap (43% of the triangles) stays cards and all
570 strands grow from the tie, rooted within 0.62 units of its radius. With the texture the
cap's painted hair stops over 2.5 units short of the tail, so the tail carries on from the cap as
before.

On real meshes, exported from the owner's mods with the designer's `hair_convert ... ctsm=`
(51 meshes: KS Hairdo's, Apachii, BG3 ports, Vanilla Hair Remake), rendered region by region:

| Hair | Kept as cards | On chains | What was found |
| --- | --- | --- | --- |
| Loose: Apachii 01, 09, 53, 58, 95, Eclipse pony, KS Tails, Swallow Tail, vanilla remake 01, 07, 15, 18, 20 | 0-2% | 0 | nothing (KS Tails' 33 flat tier pieces once read as rings) |
| Apachii 03 "Long Braids" | 28% | 33%, 2 chains | the cap gathered into two hanging braids, their tufts tied to the chain ends |
| KS TombRaider | 18% | 26%, 1 chain | the French braid on the head stays cards, the braid below it swings, its tail grows from its end |
| KS Abracadabra braid, Apachii Viking long braid | 86-90% | 10-14% | cap and bun gathered, the braid a chain; no strands |
| Apachii Navi braids long, Navi 2 braids | 57-63% | 26-32% | cap gathered into the band, the braids chains, the beaded side braids too |
| Apachii 79 (high ponytail), Saram ponytail, BG3 foxtail | 24-68% | 0-7% | the cap gathered into the tie; the curly tail grows from it |
| Vanilla remake 05, 09, 11, 12 (ponytails) | 20-33% | 0 | a tie of 15-26 tails, 23-34 card guides gathered into it |
| Vanilla remake 02, 03, 04, 06, 08, 13, 14, 16 (small braids) | 15-55% | 9-21%, 1-7 chains | the vanilla atlas's plait-textured braids |
| KS Dreadlocks, Amara dreads | 35-64% | 19-21%, 7-16 chains | a chain per dreadlock, 16 at most; the rest cards |
| Apachii 64 (curly bun), BG3 Arabel, Laezel, Minthara | 36-68% | 0 | buns and pulled-back hair gathered; their loose locks strands |

Misses: vanilla 19's braids (a crown of braids wider than 2 units where it meets the hanging
ones), 10's braided crown and 17's braided buns stay strands, as before; vanilla 21's dreadlocks stay cards without
chains (each too short past the head to swing); BG3 Karlach's mane keeps 2 short chains and some
ties among its curls.

## Tools

`tools/hair_cards_to_strands/`:

- `convert.cpp`: a command-line front end, `cards_to_strands <mesh.ctsm> <out.ctsr>
[key=value ...]` (keys as in `Settings`, with `keepWoven=0|1` and `excludeUV` / `chainUV`
rectangles as `minU,minV,maxU,maxV`). Build:
  `g++ -std=c++20 -O2 -I src/Features/HairStrands/CardsToStrands tools/hair_cards_to_strands/convert.cpp src/Features/HairStrands/CardsToStrands/CardsToStrands.cpp -o cards_to_strands`.
- `ctsio.py`: writes meshes (`.ctsm`) and reads results (`.ctsr`); the formats are in its
  docstring.
- `synth.py`: the synthetic hairstyles.
- `check.py`: builds the converter, runs every case and fails on a missed limit: loose styles
  must keep (almost) nothing as cards, and the untextured ponytail must find its tie and grow its
  tail from it. `--render DIR` draws each style from the side, back and top; `--compare EXE` runs
  another converter with the same command line beside it.

## Limits

- The scalp is fitted to the innermost hair. Big-volume hair with no scalp cap and no cards
  near the skin fits a scalp too far out, and roots sit on the inner hair shell. The actor's
  head mesh would give the real scalp; Hair Strands already reads it for collision.
- Root binding is greedy and local, not HairCS's global assignment: scalp coverage follows the
  cards that reach the scalp, so a style whose cap is sparse is sparse at the roots.
- Continuation needs the feeding hair's tip within 2.5 units of the tail's start. A tie modelled
  as a solid band between them roots the tail on the band when the band is found as woven; with
  **Keep Braids and Ties as Cards** off, or a band that does not read as a ring, the tail merges
  onto the nearest lock instead.
- Finding woven hair is a set of shape tests tuned on 51 real meshes (see Results), not a
  learned or global method: braids lying on the head made of lobes stay strands, a crown of
  braids meeting hanging ones can read too wide, and a mass of small rolled clumps (BG3 hair) can
  give a few short chains. `triangleRegions`, `excludeUV` and `chainUV` correct it by hand.
- A tie needs at least six tail card guides and its hair's painted tips within 2.5 units of
  the tail. A cap whose texture fades out well before the tie is not gathered.
- Chains are one per hanging part, 16 at most: a curtain of dreadlocks beyond that keeps its
  cards still. A chain swings rigidly per segment: a braid does not twist about its own length.
- The conversion has run on real meshes only through the command line; the game draws the
  kept cards and chains, untested in game so far; see [Hair Strands, Verifying changes](hair-strands.md#verifying-changes).
