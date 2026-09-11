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

> The port commits did **not** record the exact upstream SHA they were taken
> from. Baselines *observed on 2026-09-10* (use as an approximate "since" point,
> then pin properly on the first re-sync):
> - `alandtse/open-shaders@dev` — `7d5622f8d53268c7ce3a95912f07fa6aa8bde5bc`
> - `InTheBottle/skyrim-community-shaders@Bottle-Compendium` — `cb9a1fb` (Snow Cover
>   itself landed upstream in `a661e44` "feat: snow", 2026-09-03)

---

## 1. Watch targets

### 1a. The two source forks

```bash
git remote add open-shaders   https://github.com/alandtse/open-shaders.git
git remote add bottle         https://github.com/InTheBottle/skyrim-community-shaders.git
git fetch open-shaders dev
git fetch bottle Bottle-Compendium
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
```

GitHub compare URLs (paste the synced SHA in for a quick browser diff):

- `https://github.com/alandtse/open-shaders/compare/<SHA>...dev`
- `https://github.com/InTheBottle/skyrim-community-shaders/compare/<SHA>...Bottle-Compendium`

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

---

## 4. Re-sync checklist

1. `git fetch` both fork remotes; run the `git log` range filters in §1a against the last synced SHA recorded here.
2. For each feature with upstream movement, diff the **upstream** self-contained files against the local copies (`features/<F>/…`, `src/Features/<F>.*`) — these usually apply near-verbatim.
3. Re-derive the shared-file hunks from the upstream diff, not by copying whole files. Re-apply into the regions in §3, respecting:
   - flag bits at 7/8 (not the fork's numbering),
   - cbuffer append order == `FeatureBuffer.cpp` order,
   - RunGrass PS hunks inserted in **both** `#ifdef GRASS_LIGHTING` branches,
   - `GetFeatureList()` vector entry present.
4. Re-apply local adaptations listed per feature (i18n routing, `SafePow`, `kGrass`/`kLighting`, no `SupportsVR`, Color HSV block, placeholder-texture fallback).
5. Build `BuildDevFast.bat`; then force-compile every touched base shader with the feature define set (`Lighting.hlsl`, `RunGrass.hlsl` incl. PS, `Sky.hlsl`, `DistantTree.hlsl`) — CI's hlslkit config does not define these features.
6. `python tools/extract-i18n.py --check && python tools/extract-i18n.py --orphans && python tools/sort-i18n.py --check`.
7. Update the "Baselines" note and the per-feature port commit references at the top of this file with the new synced SHAs.

## 5. Cadence

Both forks are moving fast (multiple commits/day in early Sept 2026). A monthly
check against the compare URLs in §1a is enough unless a bug traced to one of
these features points at an upstream fix. Mainline-CS merges into `Personal`
should trigger an immediate re-verify of the §2 hot files.
