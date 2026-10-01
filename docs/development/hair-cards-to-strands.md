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
`excludeUV`, `seed`. Hair Strands maps its style onto these in `ToSettings`.

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
- `stats`: triangles, flow-map share, and what happened to the card guides (traced, repeats
  dropped, rooted, continued, merged, bridged, dropped).

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
   width (clamped to 0.4 to 2 units). `seeding: auto` switches to area seeding when the median
   guide is under 1 unit.
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

## Tools

`tools/hair_cards_to_strands/`:

- `convert.cpp`: a command-line front end, `cards_to_strands <mesh.ctsm> <out.ctsr>
[key=value ...]` (keys as in `Settings`). Build:
  `g++ -std=c++20 -O2 -I src/Features/HairStrands/CardsToStrands tools/hair_cards_to_strands/convert.cpp src/Features/HairStrands/CardsToStrands/CardsToStrands.cpp -o cards_to_strands`.
- `ctsio.py`: writes meshes (`.ctsm`) and reads results (`.ctsr`); the formats are in its
  docstring.
- `synth.py`: the synthetic hairstyles.
- `check.py`: builds the converter, runs every case and fails on a missed limit. `--render DIR`
  draws each style from the side, back and top; `--compare EXE` runs another converter with the
  same command line beside it.

## Limits

- The scalp is fitted to the innermost hair. Big-volume hair with no scalp cap and no cards
  near the skin fits a scalp too far out, and roots sit on the inner hair shell. The actor's
  head mesh would give the real scalp; Hair Strands already reads it for collision.
- Root binding is greedy and local, not HairCS's global assignment: scalp coverage follows the
  cards that reach the scalp, so a style whose cap is sparse is sparse at the roots.
- Continuation needs the feeding hair's tip within 2.5 units of the tail's start. A tie modelled
  as a solid band between them, with the cap ending well above it, merges the tail onto the
  nearest lock instead.
- Not yet checked on real meshes. The synthetic styles stand in for KS Hairdo's and vanilla
  hair; see [Hair Strands, Verifying changes](hair-strands.md#verifying-changes).
