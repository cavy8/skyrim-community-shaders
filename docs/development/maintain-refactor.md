# Bottle-Centric Maintainability Refactor

> **Temporary working plan.** This file drives a one-off refactor of the `Personal` branch of
> `cavy8/skyrim-community-shaders`. When the work is done, its lasting content becomes
> `docs/development/maintainability.md` (see §8.1) and this file is deleted.

## Purpose

Make Personal as maintainable as possible.

> **Bottle-Compendium (`InTheBottle/Bottled-Shaders`) becomes the maintenance baseline for shared
> functionality. The repository explicitly records where features and components came from (Open
> Shaders, Jiayev, mainline Community Shaders, other forks, or Personal) and keeps that separate
> from which repository Personal is synchronized against.**

Keep these questions separate. They have different answers:

1. Where did this code originally come from? (**provenance**)
2. Which implementation should maintainers compare against now? (**maintenance baseline / policy**)
3. What does Personal intentionally keep that differs from that baseline? (**local components**)
4. Does a feature combine work from more than one source? (**composite**)

A feature living in Personal does **not** mean it was created in Personal. A file differing from
Bottle does **not** mean the difference is Personal-original. A feature that started in Open can
still be maintained against Bottle. Code that is absent from Bottle or diverges from its source is
**not** Personal-original for that reason alone.

---

# 1. Decisions Already Made

The owner decided these on 2026-09-25. Do not re-open them.

| # | Decision | Consequence |
|---|---|---|
| D1 | Bottle upstream is **`https://github.com/InTheBottle/Bottled-Shaders`**, branch `Bottle-Compendium`. | The local `bottle` remote still points at the old `InTheBottle/skyrim-community-shaders` URL. Update it in Phase 0. |
| D2 | **Upscaling matches Bottle, plus a minimal Neural Rendering seam.** | `Upscaling.cpp/h`, `src/Features/Upscaling/**` and `features/Upscaling/**` are reset to Bottle, **including Bottle's FSR4 runtime upscaler** (`src/Features/Upscaling/FidelityFX/RuntimeUpscaler.cpp`, `9ea5d7e64a`). This **reverses** the earlier "FSR4 deferred" decision in the port tracker. The only permitted divergence is a small set of one-line calls into the Neural Rendering feature (§5.6). |
| D3 | **Character Rain is removed** for Bottle parity. | Wetness Effects becomes `bottle-exact`. Bottle itself reverted character wetness in `3afd2b6277`. Use that commit as the removal map (§6). Provenance is kept as a record of removed work. |
| D4 | **The Dialogue DOF targeting fix is kept as a local Personal component.** It is not sent upstream. | Post Processing is `bottle-plus-components` with exactly one Personal component (§7). |
| D5 | **Neural Rendering becomes its own top-level feature**, owned by Personal. | §5. |

---

# 2. Current Facts (verified 2026-09-25; re-verify before acting)

The source code is the authority. These facts were established from source and history. If
current source disagrees with any of them, the source wins; update this section.

## 2.1 Remotes

| Remote | URL | Branch used | Notes |
|---|---|---|---|
| `bottle` | `InTheBottle/skyrim-community-shaders` → **update to `InTheBottle/Bottled-Shaders`** | `Bottle-Compendium` | Head at audit: `dac6803377` |
| `open-shaders` | `alandtse/open-shaders` | `dev` | |
| `jiayev` | `jiayev/skyrim-community-shaders` | `compendium-clean` | |
| `ytzy` | `YtzyFvra/skyrim-community-shaders` | `feature/dlssnr-vr` | DLSS-NR fork, watch-only |
| `optiscaler-dlssnr` | `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass` | `main` | NR design reference (Separate Upscaling) |
| `dlssnr-cost-scaler` | `xenmods/DLSSNR-Cost-Scaler` | `main` | NR design reference (proxy raster) |
| `community-shaders` | `cavy8/skyrim-community-shaders` | — | This is **your own fork**, not mainline |
| *(none)* | `community-shaders/skyrim-community-shaders` | `dev` | Mainline has **no remote**. Add one (`mainline`) in Phase 0 if the audit needs it. |

## 2.2 Features by presence (`src/Features/<Name>.cpp`)

| Feature | Bottle | Open | Jiayev |
|---|---|---|---|
| ReverseZ, FootstepParticles | yes | — | — |
| ProceduralSun, FoliageLighting | yes | yes | — |
| Wind | — | yes | — |
| PseudoSunBounce | — | — | yes |
| CharacterRainSurfaces | — (reverted, `3afd2b6277`) | yes | — |

Being present somewhere is not provenance. It only tells you where to look.

## 2.3 Neural Rendering footprint

- Already split out: `src/Features/Upscaling/NeuralRendering.{cpp,h}` and
  `src/Features/Upscaling/NeuralRendering/{Backend,Runtime,D3D12Interop}.{cpp,h}` (~3000 lines).
- Still inside Upscaling: **14 `Upscaling::*NeuralRendering*` methods** in `Upscaling.cpp`
  (283 NR references). These include:
  `DrawNeuralRenderingSettings`, `BSLightingShader_SetupNeuralCategory`,
  `Capture/Restore/FinishNeuralRenderingCategoryCapture`, `MakeNeuralRenderingOptions`,
  `Capture/MakeNeuralRenderingDisplayTransform`, `EvaluateNeuralRenderingFinishedImage`,
  `CaptureNeuralRenderingFinishedImageGuides`, `EnsureNeuralRenderingFinishedImageTexture`,
  `ApplyNeuralRenderingFinishedImage`, `RequestNeuralRenderingComparisonCapture`,
  `ServiceNeuralRenderingComparison`. `Upscaling.h` also holds the NR state members
  (`neuralRenderingTexture`, `materialCategoriesSnapshot`, Finished Image textures and snapshots,
  display capture, compare state machine, `static inline NeuralRendering neuralRendering`).
- **Inline NR code inside Upscaling's own functions:**
  - `Upscale()`, DLSS branch (~L1885–1935): Before Upscaling replaces `dlssInput`. Separate
    Upscaling calls `PrepareSeparateUpscaling`, which reads Upscaling's internal
    `motionVectorCopyTexture`.
  - `PerformUpscaling()` (~L2574–2627): After Upscaling evaluates on `sharpenerTexture`. Separate
    resolves via `ResolveSeparateUpscaling`. `CaptureNeuralRenderingFinishedImageGuides()` must run
    **before** `UpscaleDepth()`.
  - `ApplySharpening()` (~L2810): sharpens `neuralRenderingTexture` instead of `sharpenerTexture`
    when the NR result is valid.
  - `pendingDLSSReset`, consumed in `Upscale()`, also resets NR temporal history on loading
    screens.
- **Outside Upscaling:**
  - `src/Hooks.cpp` ~L371–395 (tonemap: display-transform capture, Finished Image apply);
  - `src/Deferred.cpp` ~L438, ~L691 (Masks2 category capture/restore);
  - `src/State.h` (`ExtraShaderDescriptors::IsHumanoidActor`, `IsHair`);
  - `src/Menu.cpp` (41 refs) and `src/Menu.h` L180–183, L472–475 (four NR hotkeys: toggle, compare,
    scale up/down);
  - `src/Menu/SettingsTabRenderer.*`;
  - `src/Features/ScreenshotFeature.*` (`NeuralRenderingComparisonPath`);
  - `package/Shaders/Common/NeuralRenderingCategories.hlsli`, `Permutation.hlsli`, `Lighting.hlsl`
    (12 refs), `RunGrass.hlsl` (3 refs).
- NR shaders live in `features/Upscaling/Shaders/Upscaling/NeuralRendering/`. The paths are
  hardcoded as `Data\Shaders\Upscaling\NeuralRendering\*.hlsl` in `Backend.cpp` ~L90–94.
- Settings: roughly 35 `neuralRendering*` fields in `Upscaling::Settings` (Upscaling.h ~L71–104).
  Default placement is `3` (Finished Image).
- i18n: **81 keys** under `feature.upscaling.neural_rendering_*`.
- The runtime is the user-supplied `nvngx_dlssnr.dll` (NGX feature 18), loaded by
  `NeuralRendering/Runtime.cpp`.

## 2.4 Upscaling divergence from Bottle (non-NR)

28 files differ (+5921/−2475). Beyond NR, the differences include:
- Bottle's FSR4 `FidelityFX/RuntimeUpscaler.cpp` (1845 lines), which Personal lacks;
- `FidelityFX.cpp/h`;
- the FidelityFX DLLs and license;
- `DX12SwapChain.*`, `Streamline.*`, `EncodeTexturesCS.hlsl`, `Upscaling.ini`.

## 2.5 Character Rain footprint in Personal

- `src/Features/CharacterRainSurfaces.{cpp,h}`
- `features/Wetness Effects/Shaders/WetnessEffects/CharacterRain{Lighting,Spots}.hlsli`
- `src/Features/WetnessEffects.{cpp,h}`
- `src/State.h` (`IsCharacterRainSurface`, `IsHeldWeapon`)
- `package/Shaders/Common/{Permutation,SharedData}.hlsli`
- `package/Shaders/Lighting.hlsl` (`CHARACTER_RAIN_SURFACE` block ~L2285)
- `en.json` (and translations)

Bottle's revert `3afd2b6277` also touched `GrassCollision.cpp` and `src/Utils/ActorUtils.{cpp,h}`.
Check whether Personal carries those hunks.

## 2.6 Dialogue DOF fix

`src/Features/PostProcessing/DoF.cpp` `DoF::GetInDialogue()`. Bottle already implements dialogue
targeting (`speaker || lastSpeaker`). Personal's fix (`a7e142722c`, "fix(dof): clear dialogue
target-focus as soon as menu exits") adds the `menuOpen &&` gate so focus does not linger on
`lastSpeaker` after the menu closes. It is a one-hunk change on top of Bottle.

## 2.7 `ExtraShaderDescriptors` bit layout diverges from Bottle

| Bit | Bottle | Personal |
|---|---|---|
| 7 | `IsEye` | `NoSnow` |
| 8 | `NoSnow` | `NoFoliageTint` |
| 9 | `NoFoliageTint` | `IsHumanoidActor` (NR) |
| 10 | — | `IsCharacterRainSurface` (removed in Phase 3) |
| 11 | — | `IsHeldWeapon` (Character Rain only, removed in Phase 3) |
| 12 | — | `IsHair` (NR) |
| 13 | — | `TreeBend` (Wind/Open) |
| 31 | — | `IsEye` |

Personal moved `IsEye` to bit 31 on purpose (in `f18e9543`) so that upstream flags could be
appended. That turned out backwards: every Bottle sync of `State.h`/`Permutation.hlsli` now
conflicts. See §9.3 for the fix.

---

# 3. Data Model

## 3.1 Provenance vs policy

```text
provenance != maintenance policy
```

Keep them in **two separate files**:
- `feature-provenance.yaml` answers "where did this come from?";
- `maintenance-policy.yaml` answers "what do we compare against and keep synchronized?".

Neither file may contain the other's fields.

## 3.2 Upstream registry — `docs/development/upstreams.yaml`

This is the single source for repositories and the pinned baseline. There is no separate
`.bottle-base.json`. The pinned `sha` here *is* the baseline, and tooling reads it from here.

```yaml
upstreams:
  bottle:
    repository: InTheBottle/Bottled-Shaders
    remote: bottle
    branch: Bottle-Compendium
    sha: <EXACT_BOTTLE_SHA>        # pinned maintenance baseline; bump only as a deliberate sync
    role: primary-maintenance-baseline
  mainline:
    repository: community-shaders/skyrim-community-shaders
    remote: mainline               # add this remote; `community-shaders` is cavy8's own fork
    branch: dev
    role: base-project
  open:
    repository: alandtse/open-shaders
    remote: open-shaders
    branch: dev
    role: feature-source
  jiayev:
    repository: jiayev/skyrim-community-shaders
    remote: jiayev
    branch: compendium-clean
    role: feature-source
  ytzy:
    repository: YtzyFvra/skyrim-community-shaders
    remote: ytzy
    branch: feature/dlssnr-vr
    role: nr-watch
  optiscaler_dlssnr:
    repository: wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass
    remote: optiscaler-dlssnr
    branch: main
    role: nr-design-reference
  dlssnr_cost_scaler:
    repository: xenmods/DLSSNR-Cost-Scaler
    remote: dlssnr-cost-scaler
    branch: main
    role: nr-design-reference
```

Pinning the SHA identifies the baseline. It says nothing about authorship.

## 3.3 Provenance — `docs/development/feature-provenance.yaml`

Use **one schema everywhere**. Source values are upstream keys from `upstreams.yaml`, or `personal`.

```yaml
features:
  <FeatureName>:
    status: present | removed          # removed = kept for the record (e.g. CharacterRain)
    personal_original: true | false    # true ONLY if no upstream contributed implementation
    provenance:                        # one or more entries
      - source: <upstream key | personal>
        role: <short label>            # e.g. implementation, base-feature, design-reference
        confidence: confirmed | likely | unresolved
        evidence: [<personal sha>, <upstream sha>]   # optional; short hashes only
        note: <one line; required when unresolved>
    components:                        # optional; only when parts have different sources
      <component_name>:
        provenance: [ ...same entry shape... ]
        personal_original: true | false
```

Rules:
- A feature may have several provenance entries. Do not force composites into a single owner.
- Use `components` only when different parts genuinely have different sources. Do not subdivide
  every feature.
- A `design-reference` role (the idea or approach came from elsewhere, the code did not) does
  **not** make `personal_original` false on its own. Record it anyway.
- Do not invent certainty. If unclear, write `confidence: unresolved` plus a one-line `note`
  saying what needs investigating.
- Record facts, not narratives. `source: open`, `evidence: [adbcd77e, 7ae52a55]` is good. "On
  September 12 we compared..." is bad. Investigations belong in Git history or the archive.

Seed entries to verify in the audit. These are not final:

```yaml
features:
  NeuralRendering:
    status: present
    personal_original: true
    provenance:
      - source: personal
        role: implementation
        confidence: likely          # confirm no code (vs. design) was lifted from the NR references
      - source: optiscaler_dlssnr
        role: design-reference      # Separate Upscaling: a1eb303df8 "based on deferred residual experiment from wilsjo2"
        confidence: confirmed
        evidence: [a1eb303df8]
      - source: dlssnr_cost_scaler
        role: design-reference      # proxy raster / kMinimumModelExtent (Backend.cpp)
        confidence: confirmed
  CharacterRain:
    status: removed                 # D3
    personal_original: false
    provenance:
      - source: open
        role: implementation
        confidence: confirmed
        evidence: [adbcd77e, 7ae52a55]
      - source: bottle
        role: ported-then-reverted
        confidence: confirmed
        evidence: [3afd2b6277]
  PostProcessing:
    status: present
    personal_original: false
    provenance:
      - source: bottle
        role: implementation        # audit: confirm Jiayev/Open lineage of sub-effects
        confidence: likely
    components:
      dialogue_dof_menu_gate:
        personal_original: true
        provenance:
          - source: personal
            role: bug-fix
            confidence: confirmed
            evidence: [a7e142722c]
  FoliageLighting:
    status: present
    personal_original: false
    provenance:
      - source: open
        role: original-port
        confidence: confirmed
        evidence: [f18e9543, 09442614]
      - source: bottle
        role: later-merged-implementation
        confidence: likely
        evidence: [6ba0c1a1]
```

The FoliageLighting breakdown must be established from current source, not copied from this
example.

## 3.4 Maintenance policy — `docs/development/maintenance-policy.yaml`

```yaml
features:
  <FeatureName>:
    policy: bottle-exact | bottle-plus-components | bottle-plus-seam | external-maintained | personal
    reference: <upstream key>        # external-maintained only
    components:                      # bottle-plus-components only: what is retained and why
      <component_name>:
        source: <upstream key | personal>
        paths: [<path>, ...]         # or hunk anchors, see §3.6
    seam:                            # bottle-plus-seam only
      - <path>: <anchor description>
    deferred: <one line>             # an explicit owner decision not to sync something yet
```

Policy vocabulary. Keep it small:

| Policy | Meaning |
|---|---|
| `bottle-exact` | Must match the pinned Bottle SHA. |
| `bottle-plus-components` | Bottle is the base. Named components from another source (or Personal) are retained. |
| `bottle-plus-seam` | Bottle is the base, plus a minimal documented hook seam for a feature owned elsewhere. Used only for Upscaling (D2). |
| `external-maintained` | Bottle does not contain this. Track the named upstream directly (e.g. Wind → open, PseudoSunBounce → jiayev). |
| `personal` | Personal-original implementation (Neural Rendering). |

There is **no `composite` policy**. "Composite" describes provenance (several `provenance`
entries), not a sync target. If no single base is reasonable yet, choose the closest real policy,
list the components, and add a `note`. Converge toward `bottle-plus-components` over time.

Seed values (subject to the audit, except where fixed by §1):

```yaml
features:
  Upscaling:        { policy: bottle-plus-seam }          # D2
  WetnessEffects:   { policy: bottle-exact }              # D3
  PostProcessing:   { policy: bottle-plus-components }    # D4
  NeuralRendering:  { policy: personal }                  # D5
  ReverseZ:         { policy: bottle-exact }
  FootstepParticles:{ policy: bottle-exact }
  ProceduralSun:    { policy: bottle-plus-components }
  FoliageLighting:  { policy: bottle-plus-components }
  Wind:             { policy: external-maintained, reference: open }
  PseudoSunBounce:  { policy: external-maintained, reference: jiayev }
```

## 3.5 Path-level metadata

A `bottle-plus-components` policy may list `paths` per component. Only do this when it is useful.
Do not list ordinary Bottle files. The path list is **required** for `bottle-plus-components` and
`bottle-plus-seam` features, because `tools/bottle_sync.py` (§8.3) can only verify what it can map.
Features that touch many shared shaders (e.g. Reverse Z edits `FrameBuffer.hlsli`,
`DeferredCompositeCS.hlsl`, `Lighting.hlsl`, `Water.hlsl`, …) must list their shared paths too, or
"CLEAN" cannot be computed.

## 3.6 Hunk-level records for hot files

Path-level ownership does not work for files that mix sources:
`Lighting.hlsl`, `RunGrass.hlsl`, `SharedData.hlsli`, `Permutation.hlsli`, `State.h`, `Hooks.cpp`,
`State.cpp`, `Deferred.cpp`.

For these, record behavior per hunk under a `shared_integrations` section of
`maintenance-policy.yaml`. Use a stable anchor (function name, `#if` define, struct field), not a
line number:

```yaml
shared_integrations:
  package/Shaders/Lighting.hlsl:
    nr_category_classification: { source: personal, anchor: "NeuralRenderingCategories / IsHumanoidActor block" }
    foliage_scattering:        { source: open,     anchor: "<define or function>" }
  src/State.h:
    local_extra_descriptor_bits: { source: personal, anchor: "ExtraShaderDescriptors high bits (see §9.3)" }
```

Only use this for important, conflict-prone integrations.

---

# 4. Phasing

Do the phases in order. Each phase ends in a buildable, validated, committed state. Do not start a
phase while the previous one is failing validation.

| Phase | Work | Depends on |
|---|---|---|
| 0 | Remotes, pinned baseline, schema files, instruction update, archive of the tracker | — |
| 1 | Extract Neural Rendering from **current** Upscaling, behavior-preserving (§5) | 0 |
| 2 | Reset Upscaling to Bottle + NR seam (§5.6) | 1 |
| 3 | Remove Character Rain; Wetness Effects → `bottle-exact` (§6) | 0 |
| 4 | Provenance audit + per-feature normalization, one feature at a time (§9) | 0; do features touching Upscaling/Wetness after 2/3 |
| 5 | Tooling: `bottle_sync.py`, `provenance_audit.py` (§8.3–8.4) | 0; useful from Phase 2 onward, required to close Phase 4 |

Phase 1 comes before Phase 2 on purpose. Extracting NR against today's Upscaling produces a pure
refactor whose output can be A/B-compared. Syncing to Bottle afterwards is then a separate diff
with its own validation. Doing both at once makes regressions impossible to attribute.

## Phase 0 — Setup

1. `git remote set-url bottle https://github.com/InTheBottle/Bottled-Shaders.git`, then
   `git fetch bottle`. Confirm `bottle/Bottle-Compendium` resolves and its history continues from
   `dac6803377`. Add a `mainline` remote for `community-shaders/skyrim-community-shaders` if the
   audit needs it.
2. Create `upstreams.yaml` (§3.2) and pin `bottle.sha` to the fetched `Bottle-Compendium` head.
3. Create `feature-provenance.yaml` and `maintenance-policy.yaml` with the seed entries from §3.
   Mark everything not established by §1/§2 as `confidence: unresolved`.
4. **Before archiving the tracker, migrate its live decisions.** Search
   `upstream-port-tracking.md` for deferred / declined / skipped / moot / "by user request" items.
   Move each still-relevant one into `maintenance-policy.yaml` (`deferred:`) or
   `feature-provenance.yaml`. Known ones:
   - FSR4 deferral: **superseded by D2**. Record it as reversed.
   - Character Rain parity: **superseded by D3**.

   Then do §8.2 (archive) and §8.5 (instructions).

---

# 5. Neural Rendering Extraction (Phases 1–2)

Target state:

```text
Bottle owns Upscaling.
Personal owns Neural Rendering.
Pipeline placement is independent of ownership.
```

## 5.1 Layout

Move the existing split (§2.3). It is already factored, so this is relocation, not redesign:

```text
src/Features/NeuralRendering.{cpp,h}                     ← feature class (new) + today's Upscaling/NeuralRendering.{cpp,h}
src/Features/NeuralRendering/{Backend,Runtime,D3D12Interop}.{cpp,h}
features/Neural Rendering/Shaders/Features/NeuralRendering.ini
features/Neural Rendering/Shaders/NeuralRendering/*.hlsl(i)
```

- Move all 14 NR methods and all NR state members out of `Upscaling` into the NeuralRendering
  feature. That covers the category capture, `materialCategoriesSnapshot`, display-transform
  capture, Finished Image textures/snapshots/guides, and the comparison state machine.
- Move `BSLightingShader_SetupNeuralCategory` into NR's own hook install. Chain it on the shared
  thunk from `PostPostLoad()`, the same way `SubsurfaceScattering` does. Do not add it to
  Upscaling or Hooks.cpp.
- Update the hardcoded shader paths in `Backend.cpp` (`Data\Shaders\NeuralRendering\...`). Old
  installs keep stale files under `Data\Shaders\Upscaling\NeuralRendering\`. That is harmless, but
  mention it in the commit body.
- Register the feature in all five places: `Feature.cpp` include **and** `GetFeatureList()` entry,
  `Globals.h`, `Globals.cpp`, `FeatureBuffer.cpp`. The `GetFeatureList()` entry has no compile-time
  guard. Missing it makes the `.ini` show up as an "Unknown Feature" in game.
- `NeuralRendering.ini`: `Version = 1-0-0`, no stage flag. NR already ships on Personal, so it is
  not pre-release. Upscaling has no `CORE` marker, so NR gets none either.

## 5.2 Settings and compatibility

- Move **every** `neuralRendering*` field out of `Upscaling::Settings` into
  `NeuralRendering::Settings`. That is the full set at Upscaling.h ~L71–104, not a sample. It
  includes:
  - enabled, placement, style, intensity, color / local-tone / local-structure / skin-structure
    strengths;
  - automatic mask, resolution mode / scale / scaleX / scaleY;
  - transfer, luminosity, max ratio, ratio-guard enabled;
  - all seven `CategoryStrengths`;
  - depth-aware resolve, alternate frames, debug category view, raw model output.

  Drop the `neuralRendering` prefix inside the new struct.
- **Migration:** existing user settings store these keys under the Upscaling section. The
  NeuralRendering feature must, on load, read the legacy keys from the Upscaling section when its
  own section is absent. After Phase 2, Upscaling is Bottle's code and will not read or forward
  them. Before implementing, verify how `State` hands each feature its JSON subobject, and whether
  a feature can reach the settings root. If it cannot, do the migration in `State`'s load path as a
  one-off, documented shim. Stale keys left in the Upscaling section are ignored by Bottle's
  loader. Confirm that as well.
- Menu hotkeys (`NeuralRenderingToggleKey`, `CompareKey`, `ScaleUpKey`, `ScaleDownKey`) stay in
  Menu settings under their existing names, so no migration is needed. Repoint their handlers to
  `globals::features::neuralRendering`.
- **i18n:** rename the 81 keys `feature.upscaling.neural_rendering_*` → `feature.neural_rendering.*`
  (use `#define I18N_KEY_PREFIX "feature.neural_rendering."` + `TKEY`). Carry the existing values
  across in **every** `Translations/*.json` with a one-off script, then run
  `python tools/extract-i18n.py --write`, `--check`, `--orphans` and
  `python tools/sort-i18n.py --write`.
- Move the NR settings UI out of `Upscaling::DrawSettings` into `NeuralRendering::DrawSettings`.
  Update `Menu/SettingsTabRenderer.*` and `Menu.cpp` accordingly.

## 5.3 Preserve all four placements, unchanged

```text
Scene
  ├── NR Before Upscaling        (render-res colour + guides, existing jitter; result feeds DLSS input)
Upscaling (Bottle)
  ├── NR After Upscaling         (display-res colour, render-res guides with guide-jitter correction; before sharpening and before UpscaleDepth)
  ├── NR Separate Upscaling      (Feature 18 → matched residual → carrier encode → private DLSS-SR → residual decode → composite with main upscale)
Post Processing / Tonemap
  └── NR Finished Image          (tonemap hook in Hooks.cpp; depth snapshot taken before UpscaleDepth)
Present
```

Feature ownership must not dictate render location. The private DLSS-SR in Separate Upscaling is
NR-owned, even though it is DLSS.

**Do not simplify these contracts during the refactor:**
- tonemap ownership;
- Effects11 and Post Processing interplay;
- depth-snapshot timing relative to `UpscaleDepth()`;
- the motion-vector raster (NR uses the **raw** game motion vectors, not Streamline's dilated
  copy, except where `PrepareSeparateUpscaling` needs both);
- guide jitter;
- active dimensions;
- target texture format;
- HDR;
- fail-closed behavior;
- loading-screen history reset;
- category classification timing (opaque snapshot before decals, forward categories added after).

## 5.4 Category classification

NR owns the material categories (skin, hair, eyes, foliage, landscape, equipment, everything else)
and the geometry-classification logic. The shared shader integration in `Lighting.hlsl` /
`RunGrass.hlsl` / `Permutation.hlsli` stays where it is. Keep it narrow and record it under
`shared_integrations` (§3.6).

Don't redesign the `Masks2` storage now. Do avoid adding coupling that would make it harder to
move to NR-owned mask resources (category, depth, transparency) later.

## 5.5 Frame resources

NR reads renderer state through a small struct passed at each seam call:

```cpp
struct FrameResources { render extent; display extent; jitter; scene colour; depth; motion vectors; /* + seam-specific inputs */ };
```

Use existing types (`Texture2D`, `RE::RENDER_TARGETS`). Do not build a general abstraction layer
just to fit this sketch.

**D3D12/NGX:** Feature 18, `nvngx_dlssnr.dll`, NR parameter contracts, evaluation, residual
resources and NR synchronization stay NR-owned. NR gets the D3D12 device and interop from
Upscaling's `DX12SwapChain`/proxy through public accessors that Bottle already exposes. If a
generic D3D11↔D3D12 helper turns out to be duplicated in both, extract a small shared utility,
but only if Bottle's side can stay unmodified. No broad renderer rewrite.

## 5.6 The Upscaling seam (Phase 2)

After Phase 2, the diff `git diff <bottle.sha> -- src/Features/Upscaling* features/Upscaling` must
contain **only** the seam calls below. Each is a single guarded call into
`globals::features::neuralRendering`, with no NR logic in Upscaling:

| # | Location (Bottle function) | Purpose |
|---|---|---|
| S1 | `Upscale()`, DLSS branch, just before the Streamline evaluate | Before/Separate: NR may substitute the DLSS input resource. Pass what it needs, including `motionVectorCopyTexture`. |
| S2 | `PerformUpscaling()`, after `Upscale()` returns | After/Separate resolve on `sharpenerTexture`. |
| S3 | `PerformUpscaling()`, immediately before `UpscaleDepth()` | Finished Image depth/guide snapshot. |
| S4 | `ApplySharpening()` sharpening source | Use NR's result when valid. **Prefer eliminating S4:** have S2 write NR's result back into `sharpenerTexture` so `ApplySharpening()` stays Bottle-exact. Keep S4 only if the extra display-res copy costs measurably more. Measure it and record the result. |

Handle the **loading-screen reset** without a seam: NR registers its own `MenuOpenCloseEvent`
handler for `LoadingMenu` instead of piggybacking on `pendingDLSSReset`. Handle the **comparison
capture** without a seam too: `ServiceNeuralRenderingComparison` moves to NR and is called from
the same `Main_PostProcessing` site that calls it today.

Record the seam in `maintenance-policy.yaml` under `Upscaling.seam` with anchors, and in
`shared_integrations`. Target budget is **≤ ~20 changed lines** against Bottle. If a placement
cannot be preserved within the seam, stop and ask the owner. Do not grow the seam or quietly change
placement behavior.

## 5.7 Phase 2 procedure

1. Reset `src/Features/Upscaling*`, `src/Features/Upscaling/**` (minus the NR files already moved
   out) and `features/Upscaling/**` to `bottle.sha`. This adopts FSR4's `RuntimeUpscaler`, the
   FidelityFX DLLs/license and Bottle's `Upscaling.ini` version.
2. Every non-NR Personal hunk that the reset drops (e.g. in `Streamline.*`, `DX12SwapChain.*`,
   `EncodeTexturesCS.hlsl`) is **dropped** under D2. If NR needed one, it either moves into NR or
   becomes a seam entry. Nothing else survives.
3. Re-apply S1–S3 (S4 only if measured necessary).
4. Validate (§10).

---

# 6. Character Rain Removal (Phase 3)

1. Use Bottle's revert `3afd2b6277` as the map. Delete `CharacterRainSurfaces.{cpp,h}` and
   `CharacterRain{Lighting,Spots}.hlsli`.
2. Reset `WetnessEffects.{cpp,h}` and `features/Wetness Effects/**` (including `WetnessEffects.ini`)
   to `bottle.sha`.
3. Hot files (`Lighting.hlsl`, `SharedData.hlsli`, `Permutation.hlsli`, `State.h`): remove the
   Character Rain hunks **only** (`CHARACTER_RAIN_SURFACE` block, `IsCharacterRainSurface`,
   `IsHeldWeapon`, the settings fields). These files also carry NR and Wind hunks, so do not reset
   them wholesale.
4. `GrassCollision.cpp`, `src/Utils/ActorUtils.*`: if Personal carries character-wetness hunks
   there, remove them. Reset these files to Bottle only if no other Personal hunks exist.
5. Remove the Character Rain feature registration (all five places, §5.1) if it is registered as
   its own feature.
6. Remove its i18n keys from `en.json` and the translations. Run `extract-i18n.py --orphans`.
7. Remove any `CHARACTER_RAIN_SURFACE` entries from `.github/configs/shader-validation.yaml`. Grep
   the whole repo for leftovers: `CharacterRain`, `character_rain`, `CHARACTER_RAIN`,
   `IsHeldWeapon`.
8. Update `feature-provenance.yaml`: `CharacterRain.status: removed` (§3.3). Update
   `maintenance-policy.yaml`: `WetnessEffects: bottle-exact`.
9. Result: `git diff <bottle.sha> -- src/Features/WetnessEffects* "features/Wetness Effects"` is
   empty.

---

# 7. Dialogue DOF Fix (D4)

- Keep the `menuOpen &&` gate in `DoF::GetInDialogue()` (§2.6).
- When Post Processing is normalized against Bottle, this must be the **only** Personal component
  unless the audit finds others with established provenance.
- Record it as `PostProcessing.components.dialogue_dof_menu_gate` (§3.3/§3.4) with
  `paths: [src/Features/PostProcessing/DoF.cpp]`, anchored to `DoF::GetInDialogue`.
- Post Processing itself is not Personal-original just because this fix is.

---

# 8. Documentation and Tooling

## 8.1 Layout

Replace the giant living port tracker with:

```text
docs/development/
    README.md                  (update links)
    maintainability.md         concise: the model in this plan's Purpose + §3, nothing historical
    upstreams.yaml
    feature-provenance.yaml
    maintenance-policy.yaml    (includes shared_integrations)
    neural-rendering.md        (existing; see §8.6)
    history/upstream-port-tracking-2026-09.md
```

A separate `local-integrations.md` is optional. `maintenance-policy.yaml` `components` +
`shared_integrations` already cover it. If you write one, list only real cross-source combinations
(Foliage Lighting: Bottle base + Open effects; Post Processing: Bottle + Personal DOF gate) and do
not let it become a changelog.

`maintainability.md` must say, concisely:
- Bottle is the primary maintenance baseline;
- provenance is independent of the baseline;
- many Personal features are ports and some are composites;
- only Neural Rendering and the Dialogue DOF gate are known Personal-original;
- source comparison is authoritative.

## 8.2 Archive the port tracker

After Phase 0 step 4, move `docs/development/upstream-port-tracking.md` →
`docs/development/history/upstream-port-tracking-2026-09.md` and prepend:

```text
HISTORICAL SNAPSHOT
This file contains previous port investigations and audit notes.
It is not authoritative for current provenance, parity, or maintenance status.
```

Nobody maintains it after this. Update the `docs/development/README.md` link.

## 8.3 `tools/bottle_sync.py`

It reads `upstreams.yaml` (pinned SHA) and `maintenance-policy.yaml` (policies, component paths,
seam, shared_integrations). Example output:

```text
Bottle exact:
  ReverseZ                 CLEAN
  FootstepParticles        CLEAN
  WetnessEffects           CLEAN
Bottle + seam:
  Upscaling                seam only (3 anchors)       ← any other hunk = DRIFT
Bottle + components:
  FoliageLighting          Bottle base, Open components: 3 paths
  PostProcessing           Bottle base, Personal: dialogue_dof_menu_gate
External:
  Wind                     open
  PseudoSunBounce          jiayev
Personal:
  NeuralRendering
Upstream drift since pin:  bottle/Bottle-Compendium is N commits ahead of <sha>; touched: <features>
```

Model it on `tools/feature_version_audit.py` (style, exit codes). Exit non-zero on DRIFT so it
can gate CI later.

## 8.4 `tools/provenance_audit.py` (if practical)

```bash
python tools/provenance_audit.py FoliageLighting
```

```text
Personal introduction:   f18e9543 - ported from Open
Current exact matches:   FoliageLighting.cpp → open ; packaged shader X → bottle
Current divergent:       FoliageLighting.h → differs from both
Known source commits:    open 09442614 ; bottle 6ba0c1a1
```

It uses `git log --follow`, `git log -S` and tree comparisons against each upstream remote, plus
the commit trailers from §9.4.

## 8.5 AI instruction update

`.claude/CLAUDE.md` is canonical; `AI-INSTRUCTIONS.md` and `.github/copilot-instructions.md`
point to it. Add the section there and nowhere else. Update the other two only if they contradict
it.

```text
## Maintenance Baseline and Provenance (Personal branch)

Bottle-Compendium (InTheBottle/Bottled-Shaders), pinned in docs/development/upstreams.yaml,
is the primary maintenance baseline. This is not authorship: most Personal functionality
was imported from other Community Shaders forks, and features may be composites of
Bottle/Open/Jiayev/mainline code. Known Personal-original work: Neural Rendering, and the
dialogue DOF menu gate in DoF::GetInDialogue.

Before modifying a shared feature:
1. read its entries in docs/development/feature-provenance.yaml and maintenance-policy.yaml;
2. compare actual current source against the pinned Bottle SHA (tools/bottle_sync.py);
3. preserve every documented component, seam and shared_integrations hunk;
4. never infer provenance from repository path, commit author, or a generic "feat:" subject;
5. Upscaling must stay Bottle + the documented NR seam only.
```

## 8.6 `neural-rendering.md`

Keep the detailed technical document. Update its source-layout section to the new paths, and
change the architectural introduction to:

> Neural Rendering is Personal-original and owned by the Neural Rendering feature. It consumes
> renderer/Upscaling resources at several pipeline stages through a minimal documented seam, but is
> not part of the Bottle-owned Upscaling feature. Its Separate Upscaling design follows wilsjo2's
> OptiScaler deferred-residual experiment, and its proxy raster follows xenmods' DLSSNR-Cost-Scaler.

Keep the detailed explanations of all four placement modes.

---

# 9. Provenance Audit and Normalization (Phase 4)

## 9.1 Audit, one feature at a time

Run the audit per feature, immediately before normalizing that feature. There is no big-bang audit
of every feature first. Order:
1. likely `bottle-exact` features (smallest diffs first);
2. `bottle-plus-components`;
3. `external-maintained`.

For each feature/component:
1. inspect current Personal source;
2. inspect Git history (`git log --follow`, `-S`). A generic `feat: X` may still be a manual copy
   from another fork, and "Port X" obviously is;
3. inspect Bottle at the pinned SHA;
4. inspect Open, Jiayev and mainline where relevant;
5. record provenance with confidence (§3.3);
6. identify any genuinely Personal-original functionality. If found, it is new information:
   record it and tell the owner.

Never determine state solely from:
- the archived tracker;
- commit names;
- feature names;
- previous audits;
- docs saying a port is "current";
- assumptions about which fork owns a feature.

## 9.2 Normalize

For each feature:
1. **Baseline:** Bottle whenever Bottle contains the functionality.
2. **Differences:** list all behavior Personal has that Bottle lacks, and vice versa.
3. **Origin of each difference:** open / jiayev / mainline / personal / unresolved.
4. **Keep or drop:** keep a difference only if Personal still gains something Bottle cannot
   provide. Ask:
   - Does Bottle now contain equivalent functionality?
   - Can Bottle's implementation replace this portion without losing behavior?
   - If not, can the non-Bottle part be isolated cleanly?

   Unresolved-origin differences are **not** dropped silently. List them for the owner.
5. **Rebuild:** start from Bottle and reapply only the retained components, each as its own
   commit (§9.4).

This turns "a mysteriously divergent Personal feature" into "Bottle baseline + known Open
component + known Jiayev component + small Personal fix".

A composite may become progressively less composite, e.g.
`Bottle base + Open foliage component + Open bug fix + Personal integration edits` →
`Bottle current + one retained Open foliage behavior`. That is the goal. Keep provenance for
whatever is retained.

## 9.3 Hot-file rule: `ExtraShaderDescriptors`

Invert today's scheme (§2.7). **Bottle owns the low sequential bits verbatim. Local bits live at
the top and are allocated downward from bit 31.**
- Adopt Bottle's values exactly: `IsEye = 1 << 7`, `NoSnow = 1 << 8`, `NoFoliageTint = 1 << 9`,
  plus any later Bottle bits.
- Reallocate the remaining local bits from the top: `IsHumanoidActor`, `IsHair` (NR) and
  `TreeBend` (Wind/Open), e.g. 31, 30, 29. `IsCharacterRainSurface`/`IsHeldWeapon` are gone after
  Phase 3.
- Change `src/State.h` and `package/Shaders/Common/Permutation.hlsli` in the **same commit**.
  Nothing enforces that they match at compile time. Grep every user of each flag in C++ and HLSL.
- Record the local bits under `shared_integrations` (§3.6).

Apply the same principle to other append-style shared layouts. For the `FeatureData` cbuffer
(`SharedData.hlsli` ↔ `FeatureBuffer.cpp` positional pack): Bottle's order first, local structs
appended at the end, each a 16-byte multiple.

## 9.4 Commits

Use this repo's conventional types (`feat`, `fix`, `refactor`, `docs`, `style`, `test`, `chore`,
plus `perf`/`build`/`ci`/`revert` that PR-title lint accepts). Custom types such as `sync:`,
`port:`, `personal:` or `compat:` fail `pr-lint.yaml` and are ignored by semantic-release, so do
not use them. Carry provenance in the subject and in **trailers**, which `provenance_audit.py`
parses:

```text
refactor(foliage): reset shared base to Bottle

Upstream-Source: bottle@<sha>
```

```text
feat(foliage): restore Open scattering on Bottle base

Upstream-Source: open@09442614
```

```text
refactor(foliage): adapt Open scattering to Bottle data layout

Upstream-Source: open@09442614
```

```text
refactor(nr): move Neural Rendering out of Upscaling
```

```text
refactor(upscaling): match Bottle, keep NR seam

Upstream-Source: bottle@<sha>
```

```text
revert(wetness): remove Character Rain for Bottle parity

Upstream-Source: bottle@3afd2b6277
```

- One commit per source when normalizing a composite: never a single "refactor foliage".
- Never label imported work as `feat: implement X` without its `Upstream-Source:` trailer.
- The patch stack itself should document the composition.

---

# 10. Validation

The refactor must not change behavior, except where §1 decides otherwise (FSR4 added, Character
Rain removed). Agents must not deploy to the user's game install. In-game checks are run by the
owner from a build the agent produced.

**Every phase:**
- `./BuildDevFast.bat` while iterating, then `./BuildPR.bat` (CI parity) before committing.
- `python tools/extract-i18n.py --check`, `--orphans`, `python tools/sort-i18n.py --check`.
- `python tools/feature_version_audit.py` (new NR `.ini`, Upscaling/Wetness `.ini` version changes).
- `tools/bottle_sync.py` once it exists.

**Shaders:**
- Moved-only NR shaders (Phase 1): `pwsh tools/verify-shader-refactor.ps1 <file>` must report
  identical bytecode.
- Any edited shader: `hlslkit-compile` on the file. For code behind a feature define that the
  validation YAML never enables (e.g. category/NR blocks), force-compile representative permutations
  by hand with `fxc /D <DEFINE>=1`. A green hlslkit run does not prove new `#if` code compiled.

**Neural Rendering (Phases 1 and 2):** use the built-in NR comparison-screenshot hotkey
(`Data/DLSS 5 Screenshots/`, `_NR-off`/`_NR-on`) plus the RenderDoc A/B harness
(`tools/taa-renderdoc-ab.py`, `docs/development/shader-runtime-ab.md`). Capture before and after
each phase, in the same save and position, for:

```text
disabled · Before Upscaling · After Upscaling · Separate Upscaling · Finished Image
DLSS · FSR (incl. FSR4 after Phase 2) · TAA/native where applicable
SDR · HDR · Effects11 · Post Processing · category debug view · raw model output
resolution changes · placement changes · loading/menu transitions · NR hotkeys
settings migration: an existing SettingsUser.json with NR keys under Upscaling loads identical values
```

Phase 1 output must be pixel-identical. Phase 2 output must be identical for NR, and for Upscaling
must match Bottle's behavior.

**Each normalized non-NR feature:** before/after capture of a scene that exercises it, plus the
feature's settings round-trip.

---

# 11. Definition of Done

**Provenance**
- Character Rain is recorded as Open-sourced and `status: removed`.
- Known imported features are no longer mislabeled Personal.
- Composites carry multiple provenance entries.
- NR (with its design references) and the Dialogue DOF gate are the only `personal_original`
  entries, unless the audit proved more and the owner was told.
- Unresolved origins are marked `unresolved` with a note, never guessed.

**Architecture**
- Neural Rendering is a standalone registered feature with its own settings, UI, i18n keys and
  shaders.
- All four placements behave as before.
- Upscaling differs from the pinned Bottle SHA only by the documented seam (≤ ~20 lines).
- Wetness Effects matches Bottle exactly.
- The `ExtraShaderDescriptors` layout follows §9.3.

**Maintenance**
- `bottle_sync.py` reports every feature as CLEAN, seam-only, or a documented component. It exits 0.
  This is the concrete exit condition for "accidental divergence removed".
- Each retained non-Bottle component traces to a provenance entry and an `Upstream-Source:` trailer.

**Documentation**
- `maintainability.md`, `upstreams.yaml`, `feature-provenance.yaml` and `maintenance-policy.yaml`
  exist and agree with the source.
- The tracker is archived after its live decisions were migrated.
- `.claude/CLAUDE.md` carries §8.5.
- `neural-rendering.md` reflects the new layout.
- This file is deleted.

---

# 12. Mental Model

```text
          mainline Community Shaders
                     │
                     ▼
        Bottle-Compendium (pinned SHA)  ◄── primary maintenance baseline
                     │
       ┌─────────────┴──────────────┐
       │                            │
  Bottle-exact features      Bottle + named components ◄── Open / Jiayev / Personal
                                    │
  External-maintained features (Wind ← Open, PseudoSunBounce ← Jiayev)
                                    │
                         Personal integration
                                    │
             ┌──────────────────────┴───────────────────┐
             │                                          │
  Neural Rendering (own feature;             Dialogue DOF menu gate
  Upscaling seam only)                       (PostProcessing component)
```

The task is to **establish the cleanest baseline, preserve the sourced components that are still
useful, isolate genuinely original work, and record provenance accurately enough that the next
maintainer does not have to rediscover it.**
