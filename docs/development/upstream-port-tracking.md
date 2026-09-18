# Upstream Port Tracking

Features on the `Personal` branch that were **ported from other Community Shaders
forks** rather than written here. This document lists what to watch upstream so
the ports can be refreshed when the source features change, and records the
local adaptations that must survive any re-sync.

Ported so far:

| Feature | Upstream repo | Upstream branch | Port commit |
| --- | --- | --- | --- |
| Snow Cover | `InTheBottle/skyrim-community-shaders` (Bottle-Compendium) | `Bottle-Compendium` | `0d5c19633`; re-synced `b8afc7b00` (fire-melt removal, upstream `85f2020e8`) |
| Cloud Relight | `alandtse/open-shaders` | `dev` | `f18e9543f`; re-synced `105a90cea` (align lighting and controls, upstream `1d67aaead`/#681) |
| Foliage Lighting | `alandtse/open-shaders` | `dev` | `f18e9543f` |
| Vanilla Fresnel | `alandtse/open-shaders` | `dev` | `f18e9543f` |
| Post Processing | `jiayev/skyrim-community-shaders` | `compendium-clean` | `10d2eba1b`, `51c03d33b` |
| Light Limit Fix (diet SLF) | `InTheBottle/skyrim-community-shaders` | `Bottle-Compendium` | `69201a6ab`, `fe9228b99`; re-synced against head `7c58cb1ee` (2026-09-17, see §3) |
| Advanced Skin profiles / overrides | `jiayev/skyrim-community-shaders` | `compendium-clean` | `4c4eb6d25`; TRUE_PBR compile fix `2da0eb7a5` |
| TruePBR micro shadow AO | `InTheBottle/skyrim-community-shaders` | `Bottle-Compendium` | `0a69dbbc5` |
| Volumetric Lighting god ray strength / focused rays | `InTheBottle/skyrim-community-shaders` | `Bottle-Compendium` | `a582a558a` (base strength/shaft-definition/priority), `122f4e2fc` (sun focus) |
| Dynamic Cubemaps lighting-change detection | `jiayev/skyrim-community-shaders` | `compendium-clean` | `49c35c6ef` |

> The port commits did **not** record the exact upstream SHA they were taken
> from. Baselines *reviewed on 2026-09-17* (use as an approximate "since" point,
> then pin properly on the next re-sync):
> - `alandtse/open-shaders@dev` — `95bacd821` (was `7ae52a5543` on 2026-09-15; Cloud Relight
>   re-synced through #681, see the Cloud Relight section below for what's still outstanding)
> - `InTheBottle/skyrim-community-shaders@Bottle-Compendium` — `352e736e6` (was `497916e45e` on
>   2026-09-15; Snow Cover re-synced through `85f2020e8` above)
> - `jiayev/skyrim-community-shaders@compendium-clean` — `b8f93c390` (was `72041475c8` on
>   2026-09-15; no movement on the ported Post Processing/Advanced Skin paths).
>   Not in the README's [Branch-Specific Credits](../../README.md#branch-specific-credits) —
>   add it there too.

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

### Light Limit Fix (diet-SLF / local shadow cache)  (`InTheBottle/skyrim-community-shaders@Bottle-Compendium`)

**Upstream lineage**: `42e236274d45da5305ff5f383086208ec26c94ee` (feat: diet SLF),
`a0fe19bcae32825958ba56781e7ddc0b74164b32` (fix: shadow bias scaling),
`8421c9cca263ebc20d26c6ea108d23b3f5b2af3c` (fix: finalize diet SLF),
`7da7f6c4e605d1f0158764efb158fe6e4d8070f9` (fix: LLS bugs and double cache cap),
`dbcc02ffc3ee29bfeb443acdbbb15ea3a93c67ca` (fix: LLS spot light position and shadow updates), and
later Bottle head commits through `7c58cb1ee9f08bd5d590399b44ac1807498eda47`.

**Upstream source paths**

- `src/Features/LightLimitFix.cpp`, `src/Features/LightLimitFix.h`
- `features/Light Limit Fix/Shaders/LightLimitFix/LightLimitFix.hlsli`,
  `LocalShadowCopyCS.hlsl` (byte-identical between the two branches — untouched)

This repo already has its own from-scratch diet-SLF architecture (not a direct copy of Bottle's),
so a re-sync here is always a genuine comparison against current Bottle head, never a cherry-pick.

**2026-09-17 re-sync — what was present already vs. what was pulled forward:**

| Area | Before this pass | After |
| --- | --- | --- |
| Shadow-cache capacity | 32 slots, no memory cap, hard-throw on allocation failure | 64 slots, 2 GiB cap (`LOCAL_SHADOW_MAX_CACHE_BYTES`), halves the slot count and retries on `CreateTexture2D` failure, falls back to the game's own shadow masks (warn-logged) if even the minimum can't be allocated |
| Engine shadow-resource handling | Always compute-copy via `LocalShadowCopyCS.hlsl` | Tracks engine mip levels/array-slice count (`localShadowEngineMipLevels`/`localShadowEngineSlices`); `GetDepthCopyFamily()` picks a direct `CopySubresourceRegion` path when cache and engine share a format family and resolution, compute-copy remains the fallback |
| Cache invalidation | Reused a caster's slot indefinitely as long as the same `BSShadowLight*` kept appearing; no store of the underlying `NiLight*` or its rotation | Caster stores `niLight`/`rotation`; a changed `NiLight*` or a teleport beyond `max(LOCAL_SHADOW_TELEPORT_DISTANCE, radius*0.25)` resets the caster's bookkeeping (slice preserved); rotation delta now also counts toward "moved"; `CopyLocalShadowMaps` is a two-pass accumulate-then-copy so two casters claiming the same engine slice this frame are detected (`localShadowStatCollisions`) and the colliding copy is skipped instead of one silently reading the wrong slice |
| Scheduling | Starvation was an inline `1002.0f` literal at 120 stale frames, no rescue mechanism | Explicit `starved` flag, `LOCAL_SHADOW_STARVED_SCORE` (500 + staleness so ties break toward the longest-waiting caster), threshold lowered to 60 frames, and the top-N selection swaps in the highest-scoring starved caster over the lowest-priority admitted slot if none of the top N is already starved. Actor/moving-light priority tiers unchanged (verified identical to Bottle's, which also didn't touch them) |
| Filtering/bias | Depth bias scaled only by engine bias and the user slider | Also scaled by the cache/engine resolution ratio, so a downsampled cache doesn't show more self-shadow acne at the same nominal bias. Resolution-aware PCF/filter radius and the "Match Game" resolution option were **already present and confirmed functionally identical** to Bottle's — not re-touched |
| Contact shadows | Fixed step count, single nearest-depth sample per step, no ray-length awareness, no end fade | Step count now clamped to the ray's actual projected screen-space pixel length (`GetPixelLimitedSteps`); first two steps sample a bilinear-filtered/nearest depth pair (`GetScreenDepthPair`) to separate occlusion from penetration; occlusion fades out over the last steps of the ray. The coarse `MayBeOccluded()` rejection prepass is untouched (confirmed byte-identical to Bottle's) |

**Local adaptations / things a future re-sync must not blindly overwrite**

- Personal's `CopyLocalShadowMaps` uses a callback (`ForEachAccumulatedShadowLight`) rather than a
  raw loop; the collision-detection restructure kept that shape (accumulate into `pending` inside
  the callback, copy in a second pass afterward) rather than switching to Bottle's loop structure
  wholesale.
- The Statistics panel now also shows a slice-collision count and the cache's actual memory
  footprint in MB — `localShadowStatCollisions` is tracked on both branches but Bottle itself never
  surfaces it in the UI; showing it here is a small local addition beyond upstream, not a
  divergence to preserve against a future resync (feel free to keep it or drop it).
- No settings-schema or i18n changes were needed — every improvement above is compile-time
  constants and internal bookkeeping, not new user-facing settings.
- **Present** — describes exactly which current-Bottle-head improvements are pulled forward, per
  the table above; this is not a full architectural replacement of Personal's diet-SLF design.

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
- Ships with Tamriel + the five city worldspaces enabled; falls back to bundled placeholder textures (`SnowCover/default/`) when a worldspace's configured PBR snow texture isn't installed.
- **2026-09-17 (`b8afc7b00`):** followed upstream `85f2020e8` "remove snow cover fire detection" —
  dropped fire-melt entirely (settings, `FireSources` GPU buffer, the
  `BSEffectShader_SetupGeometry` hook, `CheckFireSource()`). `SnowCoverSettings` cbuffer member
  and `SnowCover::PerFrame` shrank 464 → 176 bytes. `SnowCover.ini` bumped to `1-2-0`. Upstream's
  same commit also flipped `Tamriel.json`'s `AffectTreeTint` 0 → 1 as an apparently unrelated
  default change — **not** carried over; re-review if picking that up later.

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
| `package/Shaders/Common/SharedData.hlsli` | `CloudRelightSettings` (cloudRelightMix, cloudOriginalMix, silverLiningMix, silverLiningSpread, celestialLightWeights) + cbuffer member |

**Local adaptations**

- New i18n keys for the settings UI (upstream has none) — `extract-i18n`/`sort-i18n` after edits.
- `shader-validation.yaml` has no `CLOUD_RELIGHT` define — a green hlslkit run does **not** exercise this code. Force-compile `Sky.hlsl` with the define.
- **2026-09-17 (`105a90cea`):** ported upstream `1d67aaead` "align lighting and controls" (#681).
  `CloudRelightSettings.pad` repurposed as `celestialLightWeights` (same 32-byte size, no cbuffer
  append). `CloudRelight.hlsli`'s silver-lining math reworked (`GetSilverDensity()` split out,
  `Phase::SilverLining()` no longer spread-driven). Added `SkySync::GetCelestialLightWeights()` —
  new API on `SkySync` (not present here before this sync) that exposes the eased sun/Masser/
  Secunda transition weights already tracked internally by `ShadowFader`; `CloudRelight`
  falls back to the engine directional light (`{-1,0,0}` sentinel) when Sky Sync is inactive or
  unloaded. `CloudRelight.ini` bumped to `1-1-1`.
  - **2026-09-17:** ported `fix(grass): remove vanilla lighting dimming` (#680, `5fb80fe62`) —
    removed the two `FogNearColor.w *` multiplications in `RunGrass.hlsl`'s forward-path
    `outputColor` and deferred-path `psout.Diffuse.xyz` (both PS branches use the same source
    file region for this fix). A third `FogNearColor.w * diffuseColor` later in the file (the
    third `PS_OUTPUT main`, a different permutation) was left untouched, matching upstream's own
    scope. Present by code.
  - Grass backface stabilization / open-shaders #686 — **present by code/behavior** already;
    this repo's `RunGrass.hlsl` normal handling does not exhibit the flip artifact #686 fixes.
    Not a pending pickup.
  - `fix(foliage): fade scattering at shadow limits` (#677) — see the
    Foliage Lighting section below; blocked on an architectural gap, not merged (**Deferred**).

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
- **Blocked, not merged (reviewed 2026-09-17):** upstream `58fd866d2` "fix(foliage): fade
  scattering at shadow limits" (#677) adds an `out float directionalCoverage` overload of
  `LightLimitFix::GetDirectionalShadow()` (via a `DirectionalShadow.hlsli` wrapper) so foliage
  transmission can be zeroed on the lit side of a VSM cascade boundary. **This repo has neither
  `DirectionalShadow.hlsli` nor a two-output `LightLimitFix::GetDirectionalShadow()`** — our
  Light Limit Fix directional-shadow path has architecturally diverged from upstream's current
  VSM cascade merging (see `dirVSMDetailedShadow` in `Lighting.hlsl` instead). Porting this fix
  requires reconciling that VSM refactor first; it is not a Foliage-Lighting-scoped change. Do
  not hand-wire a stub `directionalCoverage` — it would silently no-op the shadow-limit fade
  this fix exists to add. **Status: Deferred — architectural dependency not currently present.**
  Re-evaluate once/if `LightLimitFix`'s directional-shadow path adopts VSM cascade merging with a
  `directionalCoverage` output of its own.
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

### Advanced Skin profiles / overrides  (`jiayev/skyrim-community-shaders@compendium-clean`)

**Upstream source paths**

- `features/Skin/Shaders/Skin/Skin.hlsli` (small shared-shader touch)
- `src/Features/Skin.cpp`, `src/Features/Skin.h`

**Local files produced** — same paths (`features/Skin/`, `src/Features/Skin.*`). No `SharedData.hlsli`
cbuffer changes; the port is scoped to the C++ profile/override layer plus a `Skin.hlsli` hook.

**Local adaptations that must survive a re-sync**

- New i18n keys for the profile/override UI — `extract-i18n --write` / `sort-i18n --write` after
  edits (upstream predates our i18n routing here too).
- `package/Shaders/Skin/Overrides/README.md` is a local addition documenting the override format;
  not present upstream.

**2026-09-17 (`2da0eb7a5`):** ported `fix(shaders): compile skin and hair permutations under
`TRUE_PBR`` (appears on both the `jiayev` and `bottle` histories — shared ancestor commit, not a
coincidence). `TRUE_PBR` gives `DirectContext`/`MaterialProperties` layout priority, so
`RoughnessSecondary`/`SecondarySpecIntensity`/`Curvature`/`FuzzRoughness`/`hairShadow` don't exist
in a PBR permutation; the old guards only tested `SKIN`/`CS_SKIN` and `HAIR`/`CS_HAIR`, so those
paths still compiled and referenced absent members whenever `TRUE_PBR` was also defined. Derived
`CS_SKIN_SHADING`/`CS_HAIR_SHADING` in `LightingCommon.hlsli` (`SKIN`/`HAIR` && `CS_SKIN`/`CS_HAIR`
&& `!TRUE_PBR`) and re-gated the actual shading-path selection/usage sites in
`Skin.hlsli`/`LightingCommon.hlsli`/`LightingEval.hlsli`/`Lighting.hlsl` on those. Left raw (per
upstream): the `Lighting.hlsl` texture declarations preceding the `LightingCommon.hlsli` include,
the puddle-noise geometry-classification check, and the bare `SKIN` geometry checks. Verified via
fxc: `TRUE_PBR+SKIN+CS_SKIN` and `TRUE_PBR+HAIR+CS_HAIR` now compile (forward and deferred); the
non-PBR permutations still compile unchanged. **Present by code.**

### TruePBR micro shadow AO  (`InTheBottle/skyrim-community-shaders@Bottle-Compendium`)

**Upstream source paths**

- `features/TruePBR/Shaders/Features/TruePBR.ini` (version bump)
- `package/Shaders/Common/PBR.hlsli`, `package/Shaders/Common/SharedData.hlsli`
- `src/TruePBR.cpp`, `src/TruePBR.h`

**Shared-file injection points**

| File | Region |
| --- | --- |
| `package/Shaders/Common/PBR.hlsli` | end of `PBR::GetDirectLightInput` — `EnableMicroShadows` branch attenuating `lightingOutput.diffuse`/`.specular`/`.coatDiffuse` by `ApproximateDirectOcculusion(material.AO, NdotL)` blended by `MicroShadowStrength` |
| `package/Shaders/Common/SharedData.hlsli` | `TruePBRSettings` gains `EnableMicroShadows`/`MicroShadowStrength`, replacing 12 bytes of `pad` (struct stays 16 bytes) |

**Local adaptations**

- Reused this repo's existing `ApproximateDirectOcculusion()` in `Common/Shading.hlsli` (already
  used by the Skin shading path) instead of introducing a duplicate — upstream's commit uses the
  same spelling, so no rename was needed.
- `src/TruePBR.h`'s `Settings` and `SharedData.hlsli`'s `TruePBRSettings` must stay byte-identical
  (`FeatureBuffer.cpp` passes `TruePBR::settings` straight through as the GPU struct).
- `TruePBR.ini` bumped `1-0-0` → `1-1-0`.
- **Present.**

### Volumetric Lighting god ray strength / focused rays  (`InTheBottle/skyrim-community-shaders@Bottle-Compendium`)

**Upstream source paths**

- `features/Volumetric Lighting/Shaders/Features/VolumetricLighting.ini` (version bumps)
- `package/Shaders/Common/SharedData.hlsli`, `package/Shaders/ISVolumetricLightingGenerateCS.hlsl`
- `src/Features/VolumetricLighting.cpp`, `src/Features/VolumetricLighting.h`
- `src/Features/Effects11.cpp`, `src/Features/SkySync.cpp`, `src/Hooks.cpp`, `src/FeatureBuffer.cpp`

**Local adaptations that must survive a re-sync**

- This repo's `VolumetricLighting` feature had **neither** the base god-ray strength/shaft-
  definition/Effects11-priority system nor the sun-focus follow-up before this pass — the source
  anchor commit (`122f4e2fc`, "fix: directional light focused rays") is a fix *on top of* a
  prerequisite feature (`a582a558a`, "feat: dedicated basic godray adjustment") that Personal
  never had. Both were ported together as one feature; a re-sync must check both commits, not
  just the outward-facing "focused rays" one.
- `VolumetricLighting` now owns `GetRenderData()` (resolves the engine's
  `BSVolumetricLightingRenderData` once) and `ClaimEffects11Intensity()` (arbitrates ownership
  against Effects 11's `GAMEVOLUMETRICRAYS` preset intensity). `Effects11::OverrideWeather` and
  `SkySync::PostPostLoad` now go through these instead of resolving the render data address
  themselves — **do not re-introduce a second raw `REL::RelocationID(527719, 414629)` resolution
  site**; there must be exactly one.
- `ApplyGodRaySettings()` is invoked once per `Sky::UpdateColors`, from `Hooks.cpp`'s
  `WeatherExtensions::Sky_UpdateColors::thunk`, after Effects11 and SkySync have run for the tick.
- `VolumetricLightingSettings` (`GodRayGain`, `GodRayExponent`) was appended to the **end** of
  `SharedData.hlsli`'s `FeatureData` cbuffer and `FeatureBuffer.cpp`'s initializer list — matching
  this repo's existing append-only convention (§2), not Bottle's complete `SharedData.hlsli`.
- `VolumetricLighting.ini` bumped `1-1-0` → `1-2-0` (this repo's own version history; does not
  track Bottle's `1-2-0`/`1-3-0` numbering since Personal was starting from a smaller feature).
- `shader-validation.yaml` coverage for `ISVolumetricLightingGenerateCS.hlsl` not confirmed;
  force-compiled by hand with `fxc` (`CSHADER`, with and without `TERRAIN_SHADOWS`/
  `CLOUD_SHADOWS`) at port time.
- **Present.**

### Dynamic Cubemaps lighting-change detection  (`jiayev/skyrim-community-shaders@compendium-clean`)

**Upstream source paths**

- `features/Dynamic Cubemaps/Shaders/DynamicCubemaps/CaptureCommon.hlsli` (new),
  `DetectCaptureLightingCS.hlsl` (new), `InferCubemapCS.hlsl` (2-line hook), `UpdateCubemapCS.hlsl`
  (rewritten around the new shared helpers)
- `src/Features/DynamicCubemaps.cpp`, `src/Features/DynamicCubemaps.h`

**Shared-file injection points**

| File | Region |
| --- | --- |
| `CaptureCommon.hlsli` | new: `CaptureLightingState` UAV struct/buffer (u3), `SampleCapture`/`AdjustCapturePosition`/`CaptureGeometryMatches`/`CaptureHistoryExpired` helpers shared by update + detect shaders |
| `DetectCaptureLightingCS.hlsl` | new: dispatched `(1,1,1)` once per `UpdateCubemapCapture` call, before the update dispatch; writes `LightingState[0].Reset`/`.PendingResetMask` |
| `UpdateCubemapCS.hlsl` | rewritten `main()` around `CaptureCommon`'s history/geometry helpers; reset now clears history instead of the old per-UAV `ClearUnorderedAccessViewFloat` block |
| `src/Features/DynamicCubemaps.h` | `UpdateCubemapCB` gains `CaptureIndex`/`CaptureDeltaTime`/`ResetCapture`; new `CaptureLightingState` struct, `captureLightingState` (`StructuredBuffer`), `detectCaptureLightingCS`, member-level `cameraPreviousPosAdjust[2]`/`previousCaptureTime[2]` (previously a function-local `static` in `UpdateCubemapCapture`) |
| `src/Features/DynamicCubemaps.cpp` | `UpdateCubemapCapture` binds a 4th UAV slot for `captureLightingState`, dispatches `GetComputeShaderDetectLighting()` before the capture dispatch; `SetupResources` creates/clears the structured buffer |

**Local adaptations that must survive a re-sync**

- Kept this repo's own colorspace helpers (`Color::IrradianceToLinear`/`IrradianceToGamma` in
  `Common/Color.hlsli`) instead of introducing the source fork's `Common/ColorManagement.hlsli` —
  this repo never had that header. Both are gated the same way (behind
  `defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)`); a re-sync must keep using
  `Color::Irradiance*`, not reintroduce `ColorManagement::`.
- Kept this repo's `#if !defined(REFLECTIONS)` sky exclusion in `SampleCapture` (only the
  non-reflections capture ignores `depth == 1.0`) — a pre-existing local divergence from the
  source fork, which excludes the sky unconditionally in both captures. Re-verify this still
  matches intent if the source fork's own sky-handling changes.
- **Deliberately not ported**: the same upstream commit also bundles an unrelated refactor that
  collapses `envTexture`/`envReflectionsTexture` into one `envFilteredTexture` for the
  irradiance/BC6H pipeline (new format, dropped `uavReflectionsArray`/
  `envReflectionsTextureArraySRV`). That refactor has nothing to do with lighting-change
  detection; this repo's `envTexture`/`envReflectionsTexture` dual-texture structure is untouched.
  A future re-sync should treat that refactor as a separate, optional pickup, not something
  silently carried in in this feature's diff.
- Also deliberately not ported: the source fork's `UpdateCubemap()` dropped a
  `nextTask = NextTask::kCaptureInferAndIrradianceA;` reset (paired with a "restart the split
  pipeline so stale mid/last irradiance mips aren't compressed before recapture" comment) from its
  time-jump `resetCapture` block. This repo's `UpdateCubemap()` retains that reset; nothing here
  established why the source fork removed it, and removing it looked like a potential regression,
  not a lighting-detection-scoped change.
- `shader-validation.yaml` coverage for these compute shaders not confirmed; all six touched
  permutations (detect; update × plain/`REFLECTIONS`/`FAKEREFLECTIONS`; infer × plain/
  `REFLECTIONS`) were force-compiled by hand with `fxc -D COMPUTESHADER=1` at port time — compute
  shaders need `COMPUTESHADER` (not `CSHADER`) since `Util::CompileShader` injects that macro for
  `.hlsl` compute-shader compiles (see `Utils/D3D.cpp`).
- **Present.**

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

---

## 7. New upstream features observed, not currently ported or watched (reviewed 2026-09-17)

Survey of everything each fork shipped since the 2026-09-15 baselines (§1a/§1b), beyond the
already-ported feature surface. None of this has been ported — recorded here so a future re-sync
pass can decide whether any of it is worth adopting. Nothing in this section changes what §1a's
`git log` path filters need to cover unless a port is actually taken from it later.

### `alandtse/open-shaders@dev` (`7ae52a5543` → `95bacd821`)

- **`features/Wind/`** — new top-level feature, "add shared wind field system" (#634). A
  cross-feature wind model other features (grass, foliage, clouds) can query; potentially
  relevant context for anything that touches grass/foliage motion here later.
- **Scene Manager** (#589) and an **"OS Menu" editor tab** (#674) — new UI/workflow surface, not
  overlapping any of our ported features' code paths.
- **Procedural sun** (#678) and **linear lighting rework** (#666, `feat: linear lighting rework`)
  — the latter touches `linearLightingSettings`, which `CloudRelight.hlsli` already reads
  (`SharedData::linearLightingSettings.enableLinearLighting` etc.); worth a diff against our
  Linear Lighting feature next time Cloud Relight is re-synced, in case the field set moved.
  Cloud Relight's own math changes from this range are captured in `105a90cea` above.
- **PBR grass** (#2709) and **terrain-variation mesh support** (#2703) — mainline-CS features
  merged into this fork via its `chore(sync)` commits, unrelated to our ports.
  `fix(grass): fix renderdoc crash with grass-opt enabled` (#2706) may be worth a look given we
  carry our own grass-PS duplication (§2) but wasn't investigated this pass.
- VR-specific work (dynamic near clip #615, native-menu VR awareness, instance-culling fixes) —
  out of scope, this fork's Skyrim VR support doesn't apply here unless VR is added later.
- `refactor(math): centralize ClampFinite` (#638), `refactor(ui): adopt CheckboxFlag across
  features` (#639), `feat(feature): add generic per-render-pass hook` (#654) — general
  infrastructure refactors, not evaluated for adoption.

### `InTheBottle/skyrim-community-shaders@Bottle-Compendium` (`497916e45e` → `7c58cb1ee`)

- **Character skin wetness** (`374fab73b`) — sizeable new feature: `CharacterRainSurfaces`,
  `CharacterRainLighting.hlsli`, `CharacterRainSpots.hlsli`, wired into `WetnessEffects` and
  `GrassCollision`. Large diff (~1200 lines); would be a standalone port, not a small pickup.
  Personal already has its own Character Rain implementation — **present by code** (see the
  Character Rain note below), so this is a candidate for a parity re-check, not a fresh port.
- The `snow-rework` branch (already flagged in §1a) is still the one to watch for a Snow Cover
  rewrite that would supersede `Bottle-Compendium`'s version entirely.
- **2026-09-17: Character Rain — corrected capability record.** Personal's own Character Rain
  implementation (predates this tracking pass) is **present by code**; an earlier read of this
  document's "not currently ported" language was inaccurate. No action taken here beyond
  correcting the record — a line-by-line parity diff against Bottle's `374fab73b` wetness work is
  still open, per the note above.
- **2026-09-17:** the diet-SLF/local-shadow re-sync (see the Light Limit Fix section above) was
  reviewed against Bottle head `7c58cb1ee` (moved from the `352e736e6` baseline recorded in this
  file on 2026-09-15/17) — see that section for exactly what was and wasn't pulled forward.

### DLSS-NR forks (§6) — skimmed 2026-09-17

- `YtzyFvra/skyrim-community-shaders@feature/dlssnr-vr` — active NGX Feature 18 (DLSS Ray
  Reconstruction) renderer work, plus `fix(post-processing): guard tonemapper index` (#554) and
  `fix(effects11): sync ENB state with settings save` (#555) — both land in the tonemap-ownership
  seam Post Processing/Effects11 share here; worth a look if a tonemap-ownership bug surfaces.
- `wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass@main` — "Add finished-picture NR with HDR and pre-SR
  transfer" is the most recent head; ongoing HDR/pre-SR timing work relevant to
  `HDRDisplay/HDROutputCS.hlsl`'s assumptions if DLSS-NR is ever integrated here.
- `xenmods/DLSSNR-Cost-Scaler@main` — not a CS fork (a standalone ReShade-adjacent add-on); its
  "preserve native hue and saturation across ColorStrength" and anamorphic scaling work is
  conceptually adjacent but shares no code seam with this repo.

### `jiayev/skyrim-community-shaders@compendium-clean` (`72041475c8` → `b8f93c390`)

No movement on the ported Post Processing or Advanced Skin paths (§1a `git log` filter came back
empty). Everything in this range is new/unported surface:

- **2026-09-17:** ported **Dynamic Cubemaps lighting-change detection** (`49c35c6ef`) — see the
  dedicated §3 section below for what was ported vs. deliberately left out.
- **Linear Lighting refactor** (`LinearLighting.cpp/h`) alongside the Physical Sky cloud work
  below — touches `package/Shaders/Lighting.hlsl`, `RunGrass.hlsl`, `Water.hlsl`, `Particle.hlsl`,
  `Effect.hlsl` (6 lines each), so any future port from this range should re-check those hunks
  against our own Linear Lighting state.
- **Physical Sky cloud improvements** — `CloudMotion.hlsli` (new), substantial `CloudTemporal.hlsli`
  rework (+209/-lines), `CloudBlur.hlsli`/`CloudBoundary.hlsl`/`Volumetrics.cs.hlsl` changes,
  `VolumetricClouds.cpp` and `PhysicalSky.cpp/h` updates.

None of the three forks' new work above was ported in this pass — recorded for the next re-sync
to triage, not acted on.
