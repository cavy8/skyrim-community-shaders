# Upstream Port Tracking

Features on the `Personal` branch that were **ported from other Community Shaders
forks** rather than written here. This document lists what to watch upstream so
the ports can be refreshed when the source features change, and records the
local adaptations that must survive any re-sync.

Ported so far:

| Feature | Upstream repo | Upstream branch | Port commit |
| --- | --- | --- | --- |
| Snow Cover | `InTheBottle/skyrim-community-shaders` (Bottle-Compendium) | `Bottle-Compendium` | `0d5c19633` |
| Cloud Relight | `alandtse/open-shaders` | `dev` | `f18e9543f` |
| Foliage Lighting | `alandtse/open-shaders` | `dev` | `f18e9543f` |
| Vanilla Fresnel | `alandtse/open-shaders` | `dev` | `f18e9543f` |
| Post Processing | `jiayev/skyrim-community-shaders` | `compendium-clean` | `10d2eba1b` |

> The port commits did **not** record the exact upstream SHA they were taken
> from. Baselines *observed on 2026-09-10* (use as an approximate "since" point,
> then pin properly on the first re-sync):
> - `alandtse/open-shaders@dev` — `7d5622f8d53268c7ce3a95912f07fa6aa8bde5bc`
> - `InTheBottle/skyrim-community-shaders@Bottle-Compendium` — `cb9a1fb` (Snow Cover
>   itself landed upstream in `a661e44` "feat: snow", 2026-09-03)
> - `jiayev/skyrim-community-shaders@compendium-clean` — not yet pinned; the port
>   commit (`10d2eba1b`, "port jiayev's Post Processing feature") didn't record the
>   source SHA. Pin on first re-sync. Not in the README's
>   [Branch-Specific Credits](../../README.md#branch-specific-credits) — add it there too.

---

## 1. Watch targets

### 1a. The three source forks

```bash
git remote add open-shaders   https://github.com/alandtse/open-shaders.git
git remote add bottle         https://github.com/InTheBottle/skyrim-community-shaders.git
git remote add jiayev         https://github.com/jiayev/skyrim-community-shaders.git
git fetch open-shaders dev
git fetch bottle Bottle-Compendium
git fetch jiayev compendium-clean
```

Check for movement since the last sync (record the SHA you synced to in this
file each time):

```bash
# Everything that touched the ported surface upstream since <LAST_SYNCED_SHA>
git log <LAST_SYNCED_SHA>..open-shaders/dev --oneline -- \
  "features/Cloud Relight" "features/Foliage Lighting" "features/Vanilla Fresnel" \
  "features/Cloud Shadows/Shaders/CloudShadows/CloudShadows.hlsli" \
  "features/Grass Lighting/Shaders/GrassLighting/GrassLighting.hlsli" \
  "package/Shaders/Lighting.hlsl" "package/Shaders/RunGrass.hlsl" "package/Shaders/Sky.hlsl" \
  "package/Shaders/Common/LightingCommon.hlsli" "package/Shaders/Common/LightingEval.hlsli" \
  "package/Shaders/Common/PBR.hlsli" "package/Shaders/Common/Math.hlsli" \
  "package/Shaders/Common/SharedData.hlsli" "package/Shaders/Common/Permutation.hlsli" \
  "src/Features/CloudRelight.cpp" "src/Features/CloudRelight.h" \
  "src/Features/FoliageLighting.cpp" "src/Features/FoliageLighting.h" \
  "src/Features/VanillaFresnel.cpp" "src/Features/VanillaFresnel.h"

git log <LAST_SYNCED_SHA>..bottle/Bottle-Compendium --oneline -- \
  "features/Snow Cover" "src/Features/SnowCover.cpp" "src/Features/SnowCover.h" \
  "src/Utils/FormIdParser.cpp" "src/Utils/FormIdParser.h" \
  "package/Shaders/Lighting.hlsl" "package/Shaders/RunGrass.hlsl" "package/Shaders/DistantTree.hlsl" \
  "package/Shaders/Common/SharedData.hlsli" "package/Shaders/Common/Permutation.hlsli" \
  "package/Shaders/Common/Color.hlsli"

git log <LAST_SYNCED_SHA>..jiayev/compendium-clean --oneline -- \
  "features/Post Processing" "src/Features/PostProcessing.cpp" "src/Features/PostProcessing.h" \
  "package/Shaders/ISHDR.hlsl" "features/HDR Display/Shaders/HDRDisplay/HDROutputCS.hlsl" \
  "src/ShaderCache.cpp" "src/ShaderCache.h" "src/Utils/D3D.h" "src/Utils/D3D.cpp" \
  "src/Features/Effects11.cpp" "src/State.h" \
  "package/Shaders/Common/SharedData.hlsli"
```

GitHub compare URLs (paste the synced SHA in for a quick browser diff):

- `https://github.com/alandtse/open-shaders/compare/<SHA>...dev`
- `https://github.com/InTheBottle/skyrim-community-shaders/compare/<SHA>...Bottle-Compendium`
- `https://github.com/jiayev/skyrim-community-shaders/compare/<SHA>...compendium-clean`

Also watch the `snow-rework` branch on the Bottle fork — Snow Cover is actively
being reworked there and may supersede the `Bottle-Compendium` version.

### 1b. This fork's own upstream (mainline Community Shaders)

Every port edits shared base-shader files (`Lighting.hlsl`, `RunGrass.hlsl`,
`Sky.hlsl`, `DistantTree.hlsl`, `Common/*.hlsli`) and shared C++ registration
files. When **mainline** CS changes those same files, the ported hunks are the
most likely merge-conflict / silent-breakage site. After any merge from
mainline, re-verify each feature's hunks below still apply and still compile
with the feature define forced (see [shader-workflow.md](shader-workflow.md) and
the porting notes in `.claude` memory).

---

## 2. Shared "hot" files (all ports touch these)

Conflicts here affect multiple features at once. Order in the cbuffer / flag
enums is load-bearing.

| File | What the ports added | Cross-feature constraint |
| --- | --- | --- |
| `package/Shaders/Common/SharedData.hlsli` | `CloudRelightSettings`, `FoliageLightingSettings`, `VanillaFresnelSettings`, `SnowCoverSettings` structs + members appended to the `FeatureData` cbuffer | Byte-matched to `src/FeatureBuffer.cpp` `_GetFeatureBufferData()` positional pack. Append only, each struct a 16-byte multiple, same order in both files. |
| `package/Shaders/Common/Permutation.hlsli` | `NoSnow = 1<<7`, `NoFoliageTint = 1<<8` in `ExtraFlags` | Must stay in lockstep with `src/State.h` `ExtraShaderDescriptors`. This repo deliberately parks `IsEye` at `1<<31` so upstream flags append sequentially after `AdditiveLighting` (bit 6). Nothing enforces the match at compile time. |
| `src/State.h` | `NoSnow = 1<<7`, `NoFoliageTint = 1<<8` | mirror of the above |
| `src/Feature.cpp` | `#include` + entry in the hard-coded `GetFeatureList()` vector, per feature | The vector entry has **no compile-time guard**. Omitting it makes the deployed `.ini` show as "Unknown Feature" in-game. |
| `src/FeatureBuffer.cpp` | `#include` + positional pack entry, per feature | order must match `SharedData.hlsli` |
| `src/Globals.h` / `src/Globals.cpp` | fwd decl + `extern` + `#include` + instance, per feature | compile-guarded |
| `package/Shaders/Lighting.hlsl` | Vanilla Fresnel, Foliage Lighting **and** Snow Cover all inject into `PS_OUTPUT main` | Highest-traffic file. See per-feature hunk maps below. |
| `package/Shaders/RunGrass.hlsl` | Vanilla Fresnel, Foliage Lighting, Snow Cover inject into the PS | Its PS body is **duplicated** behind `#ifdef GRASS_LIGHTING / #else`; every grass-PS hunk goes in **twice**. `.github/configs/shader-validation.yaml` has a VSHADER entry only — the PS is never validated by hlslkit; force-compile with `fxc`. |

---

## 3. Per-feature watch lists

### Snow Cover  (`InTheBottle/skyrim-community-shaders@Bottle-Compendium`)

**Upstream source paths**

- `features/Snow Cover/Shaders/**` — `SnowCover/SnowCover.hlsli`, `Features/SnowCover.ini`,
  per-worldspace `SnowCover/*.json`, `SnowCover/*.dds`, `SnowCover/default/*.dds`,
  `SnowCover/whitelist.txt`, `SnowCover/blacklist.txt`
- `src/Features/SnowCover.cpp`, `src/Features/SnowCover.h`
- `src/Utils/FormIdParser.cpp`, `src/Utils/FormIdParser.h` (whitelist/blacklist name hashing; unused elsewhere here)

**Local files produced** — same paths (feature folder is `features/Snow Cover/`).

**Shared-file injection points to re-diff**

| File | Region |
| --- | --- |
| `package/Shaders/Lighting.hlsl` | include of `SnowCover/SnowCover.hlsli`; early-out `if (!SharedData::snowCoverSettings.EnableSnowCover)`; large block ~L2071+ in `PS_OUTPUT main` — `snowOcclusion`, `SnowCover::ApplySnowPBR` / `ApplySnow`, `SnowCover::ApplyFoliageColor`, `NoSnow` / `NoFoliageTint` flag tests; terrain-height/`disp` handling |
| `package/Shaders/RunGrass.hlsl` | include; `SnowCover::ApplySnowFoliage` call in **both** PS branches (~L560, ~L865) |
| `package/Shaders/DistantTree.hlsl` | include; `SnowCover::ApplySnowFoliage` + `GetObjectFade` in both `PS_OUTPUT main` variants |
| `package/Shaders/Common/SharedData.hlsli` | `SnowCoverSettings` struct (Month, TimeSnowing, fire-melt params, angle blend, tint, fade…) + cbuffer member |
| `package/Shaders/Common/Permutation.hlsli` / `src/State.h` | `NoSnow`/`NoFoliageTint` bits |
| `package/Shaders/Common/Color.hlsli` | `RGBtoHSV` / `HSVtoRGB` / `HueToRGB` block (added next to `GammaToLinearSafe`) |

**Local adaptations that must survive a re-sync**

- All UI strings routed through `T()` / `TKEY()` (source fork predates i18n). Re-run `python tools/extract-i18n.py --write` and `python tools/sort-i18n.py --write` after.
- `NoSnow`/`NoFoliageTint` landed at bits **7/8**, not the source fork's 8/9 (see §2).
- Fire-melt hook uses this repo's shared `BSLightingShader/BSEffectShader_SetupGeometry` thunk pattern (like SubsurfaceScattering) — no `Hooks.cpp` edit.
- Ships with Tamriel + the five city worldspaces enabled; falls back to bundled placeholder textures (`SnowCover/default/`) when a worldspace's configured PBR snow texture isn't installed.

### Cloud Relight  (`alandtse/open-shaders@dev`)

**Upstream source paths**

- `features/Cloud Relight/Shaders/**` — `CloudRelight/CloudRelight.hlsli`, `CloudRelight/Draine.hlsli`, `CloudRelight/LICENSE` (NVIDIA MIT — keep), `Features/CloudRelight.ini`
- `features/Cloud Shadows/Shaders/CloudShadows/CloudShadows.hlsli` (small hook, ~6 lines)
- `src/Features/CloudRelight.cpp`, `src/Features/CloudRelight.h`

**Shared-file injection points**

| File | Region |
| --- | --- |
| `package/Shaders/Sky.hlsl` | `#include "CloudRelight/CloudRelight.hlsli"`; in `PS_OUTPUT main`, `if (SharedData::cloudRelightSettings.enabled)` → `CloudRelight::RelightCloud(baseColor, viewDir, SampBaseSampler)` |
| `features/Cloud Shadows/Shaders/CloudShadows/CloudShadows.hlsli` | 6-line hook block |
| `package/Shaders/Common/SharedData.hlsli` | `CloudRelightSettings` (cloudRelightMix, cloudOriginalMix, silverLiningMix, silverLiningSpread, pad) + cbuffer member |

**Local adaptations**

- New i18n keys for the settings UI (upstream has none) — `extract-i18n`/`sort-i18n` after edits.
- `shader-validation.yaml` has no `CLOUD_RELIGHT` define — a green hlslkit run does **not** exercise this code. Force-compile `Sky.hlsl` with the define.

### Foliage Lighting  (`alandtse/open-shaders@dev`, CORE feature)

**Upstream source paths**

- `features/Foliage Lighting/**` — `CORE` marker, `Shaders/Features/FoliageLighting.ini`
- `features/Grass Lighting/Shaders/GrassLighting/GrassLighting.hlsli` (~20 lines changed)
- `src/Features/FoliageLighting.cpp`, `src/Features/FoliageLighting.h`

**Shared-file injection points**

| File | Region |
| --- | --- |
| `package/Shaders/Common/LightingCommon.hlsli` | `namespace Foliage` constants + `float GetFoliageTransmission(NdotL, VdotL)` (wrap + Draine-style forward scatter) |
| `package/Shaders/Common/LightingEval.hlsli` | in `EvaluateLighting`, `[branch] if (SharedData::foliageLightingSettings.EnableFoliageScattering != 0)` transmission term |
| `package/Shaders/Common/PBR.hlsli` | foliage transmission in the diffuse path (~L171); ambient boost under `#if defined(TREE_ANIM)` (~L286) |
| `package/Shaders/Lighting.hlsl` | `[branch] if (SharedData::foliageLightingSettings.EnableFoliageAmbientFlip != 0)` (~L2563) |
| `package/Shaders/RunGrass.hlsl` | `GetFoliageTransmission` in the dir-light and point-light loops (both PS branches) |
| `package/Shaders/Common/SharedData.hlsli` | `FoliageLightingSettings` (EnableFoliageScattering / AmbientBoost / AmbientFlip, FoliageAmbientAmount) + cbuffer member |
| `package/Shaders/Common/Math.hlsli` | `float SafePow(base, exp)` added to `namespace Math` (this repo lacked it) |

**Local adaptations**

- `SafePow` added to `Math.hlsli` — if upstream later adds its own, de-dup.
- `Feature` category: this repo has no `kFoliage` — uses `kGrass` / `kLighting`.
- Strip any `virtual bool SupportsVR() override` from the copied header (C3668 here).
- i18n keys added.

### Vanilla Fresnel  (`alandtse/open-shaders@dev`, CORE feature)

**Upstream source paths**

- `features/Vanilla Fresnel/**` — `CORE` marker, `Shaders/Features/VanillaFresnel.ini`
- `src/Features/VanillaFresnel.cpp`, `src/Features/VanillaFresnel.h`

**Shared-file injection points**

| File | Region |
| --- | --- |
| `package/Shaders/Lighting.hlsl` | Multiple hunks in `PS_OUTPUT main`: `complexSpecular` F0 multiplier (~L1431); `enableVanillaFresnel` / `isEyeMaterial` / `material.F0` / roughness remap block (~L1834); dynamic-cubemap→F0 conversion (~L1923–1986); GGX gating in the specular apply (~L2680–2730) |
| `package/Shaders/RunGrass.hlsl` | `F0` / `roughness` init from `vanillaFresnelSettings` (~L479); GGX-on-grass gate (~L654) — both PS branches |
| `package/Shaders/Common/LightingEval.hlsli` | `float3 MicrofacetSpecular(context, F0, roughness)` helper; `if (…EnableGGX)` branch in `EvaluateLighting` using `BRDF::EnvBRDF` |
| `package/Shaders/Common/SharedData.hlsli` | `VanillaFresnelSettings` (RoughnessMultiplier, SpecularRoughnessBlend, BaseF0Multiplier, MinF0, CubemapToF0Multiplier, ComplexMaterialF0Multiplier, Enable/GGX/Eye toggles) + cbuffer member |
| `package/Shaders/Common/Permutation.hlsli` | shares the `IsEye` bit test (already `1<<31` here) |
| `src/TruePBR.h` | 1-line addition |

**Local adaptations / notes**

- Relies on the invariant that `TRUE_PBR` never combines with `ENVMAP`/`EMAT`/`MULTI_LAYER_PARALLAX`/`EYE` in the validation matrix — the Fresnel hunks reference vars from the non-`TRUE_PBR` `MaterialProperties` branch on purpose.
- `shader-validation.yaml` has no `VANILLA_FRESNEL` define — force-compile `Lighting.hlsl` and `RunGrass.hlsl` (PS included) by hand with `fxc /D VANILLA_FRESNEL=1`.
- i18n keys added.

### Post Processing  (`jiayev/skyrim-community-shaders@compendium-clean`)

**Upstream source paths (per port commit `10d2eba1b`)**

- `features/Post Processing/**` — full feature folder (DoF, Vignette, Local Exposure,
  Histogram Auto Exposure, COD Bloom, Lens Flare, Physical Glare, Motion Blur, Colour
  Grading incl. 17 tonemappers + OpenDRT, LUT, Camera, Border, Composite)
- `src/Features/PostProcessing.cpp`, `src/Features/PostProcessing.h`
- `src/Features/PostProcessing/ColorSpace.h` (local adaptation layer, see below)

**Shared-file injection points to re-diff**

| File | Region |
| --- | --- |
| `package/Shaders/ISHDR.hlsl` | `POSTPROCESS` passthrough branch (vanilla tonemap bypass when Post Processing owns the frame) |
| `features/HDR Display/Shaders/HDRDisplay/HDROutputCS.hlsl` | branch that treats a Post-Processing-owned frame as already linear / already in the output gamut — skips gamma-decode and the BT.2020 conversion |
| `src/State.h` / tonemap ownership | `GetTonemapOwner()` (FrameChecker-cached) arbitrating Effects11 vs. Post Processing — Effects11 wins ties |
| `src/Features/Effects11.cpp` (`HandleTonemapRender`) | split into `WantsTonemapOwnership()` and `RenderTonemap()`; menu warning when Effects11 takes the pass from Post Processing |
| `src/ShaderCache.*` | `EnqueueStandaloneShaderCompile` / `EnqueueComputeShaderCompile` / `ClearStandaloneComputeCache`, `TryTakeNext` over a shared dispatch budget (replaces `WaitTake`) |
| `src/Utils/D3D.h` | `Util::CustomInclude` (moved here from `D3D.cpp` so every HLSL compile site shares it) |
| `package/Shaders/Common/SharedData.hlsli` / `src/FeatureBuffer.cpp` | `postProcessingSettings` appended as the **last** `FeatureData` b6 slot — every existing offset stays put |

**Local adaptations that must survive a re-sync**

- FontAwesome stripped (this repo doesn't ship the font); `JiayeStatement` dropped.
- No `enableACEScg` mode in this repo's Linear Lighting — the six call sites that would
  branch on it instead go through `SceneUsesWideGamutWorkingSpace()` in
  `src/Features/PostProcessing/ColorSpace.h`.
- `FeatureBuffer` wires `GetCommonBufferData()` rather than reading settings directly, so
  `DisableVanillaTonemapping` is masked when Post Processing doesn't own the frame.
- LUT/bokeh textures renamed for RenderDoc debuggability; unused upstream `textures/` dropped.
- `shader-validation.yaml` has no Post Processing–specific defines beyond what CI already
  exercises (149 PP permutations + 9 ISHDR permutations were fxc-validated at port time,
  not by hlslkit) — force-compile with `fxc` rather than trusting a green hlslkit run alone.
- Marked alpha at port time — confirm current release stage in
  `features/Post Processing/Shaders/Features/PostProcessing.ini` before assuming defaults.

---

## 4. Re-sync checklist

1. `git fetch` all three fork remotes; run the `git log` range filters in §1a against the last synced SHA recorded here.
2. For each feature with upstream movement, diff the **upstream** self-contained files against the local copies (`features/<F>/…`, `src/Features/<F>.*`) — these usually apply near-verbatim.
3. Re-derive the shared-file hunks from the upstream diff, not by copying whole files. Re-apply into the regions in §3, respecting:
   - flag bits at 7/8 (not the fork's numbering),
   - cbuffer append order == `FeatureBuffer.cpp` order (Post Processing's `postProcessingSettings` stays **last**),
   - RunGrass PS hunks inserted in **both** `#ifdef GRASS_LIGHTING` branches,
   - `GetFeatureList()` vector entry present.
4. Re-apply local adaptations listed per feature (i18n routing, `SafePow`, `kGrass`/`kLighting`, no `SupportsVR`, Color HSV block, placeholder-texture fallback, Post Processing's `ColorSpace.h` ACEScg substitute).
5. Build `BuildDevFast.bat`; then force-compile every touched base shader with the feature define set (`Lighting.hlsl`, `RunGrass.hlsl` incl. PS, `Sky.hlsl`, `DistantTree.hlsl`, `ISHDR.hlsl`, `HDRDisplay/HDROutputCS.hlsl`) — CI's hlslkit config does not define these features.
6. `python tools/extract-i18n.py --check && python tools/extract-i18n.py --orphans && python tools/sort-i18n.py --check`.
7. Check the neural-rendering (DLSS-NR) forks in §6 for changes touching the same tonemap-ownership / upscaling seams Post Processing and Effects11 share.
8. Update the "Baselines" note and the per-feature port commit references at the top of this file with the new synced SHAs.

## 5. Cadence

Both forks are moving fast (multiple commits/day in early Sept 2026). A monthly
check against the compare URLs in §1a is enough unless a bug traced to one of
these features points at an upstream fix. Mainline-CS merges into `Personal`
should trigger an immediate re-verify of the §2 hot files.

---

## 6. Also watch: neural-rendering (DLSS-NR) forks

These forks aren't sources for any port in §3 — none of the ported features
(Snow Cover, Cloud Relight, Foliage Lighting, Vanilla Fresnel, Post Processing)
were taken from them. They're listed in the README's
[Branch-Specific Credits](../../README.md#branch-specific-credits) because they
do independent DLSS Ray/Neural-Reconstruction (DLSS-NR) work that lands in the
**same** upscaling/tonemap seams the ports touch — most notably Post
Processing's and Effects11's tonemap-ownership arbitration (`ISHDR.hlsl`,
`HDRDisplay/HDROutputCS.hlsl`, `State::GetTonemapOwner()`, §3 "Post Processing").
A fix or regression in one of these forks' DLSS-NR handling can be relevant to
that code even though it never flows in as a port.

| Fork | Repo | Branch |
| --- | --- | --- |
| dlssnr-vr (Open Shaders fork) | `YtzyFvra/skyrim-community-shaders` | `feature/dlssnr-vr` |
| OptiScaler DLSS-NR pre-SR multipass | `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass` | `main` |
| DLSSNR-Cost-Scaler | `xenmods/DLSSNR-Cost-Scaler` | `main` |

```bash
git remote add ytzy-dlssnr https://github.com/YtzyFvra/skyrim-community-shaders.git   # already added as `ytzy` locally
git remote add optiscaler-dlssnr https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass.git
git remote add dlssnr-cost-scaler https://github.com/xenmods/DLSSNR-Cost-Scaler.git
git fetch ytzy-dlssnr feature/dlssnr-vr
git fetch optiscaler-dlssnr main
git fetch dlssnr-cost-scaler main
```

Unlike §1a, there's no shared feature-path filter to `git log` against — these
forks aren't laid out like this codebase. Skim commit subjects/PR titles for
tonemap, HDR-output, gamut, or frame-generation changes near a release, rather
than diffing full trees on a schedule. Fold into the monthly check in §5.
