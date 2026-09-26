> **HISTORICAL SNAPSHOT**
> This file contains previous port investigations and audit notes.
> It is not authoritative for current provenance, parity, or maintenance status.
> Current state: [maintainability.md](../maintainability.md), `upstreams.yaml`,
> `feature-provenance.yaml`, `maintenance-policy.yaml`.

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
| Reverse Z | `InTheBottle/skyrim-community-shaders` | `Bottle-Compendium` | `124bc7e22` (feature), `1698b4beb` (depth-convention edits across `FrameBuffer.hlsli`/`DeferredCompositeCS.hlsl`/`Effect.hlsl`/`IS*.hlsl`/`Lighting.hlsl`/`Utility.hlsl`/`Water.hlsl`/`LightLimitFix.hlsli`); undocumented until the 2026-09-25 review — see §3; residual follow-ups (`9a6d3b2426`, `0a9f8f8ac0`, `3b9a9dc3c9`) closed in the 2026-09-25 implementation pass, see §3 |
| Footstep Particles | `InTheBottle/skyrim-community-shaders` | `Bottle-Compendium` | `124bc7e22`; undocumented until the 2026-09-25 review — see §3; re-checked in the 2026-09-25 implementation pass, byte-identical to Bottle head, no drift |
| ENB Depth of Field (Effects11) | `InTheBottle/skyrim-community-shaders` | `Bottle-Compendium` | `2a39ad34e9` (DOF half of "DOF and water readd" — the bundled water-settings half was left out, out of scope); ported 2026-09-25, see §3 |
| Sky Scattering (Effects11) | `InTheBottle/skyrim-community-shaders` | `Bottle-Compendium` | `0657cf0ab4`; ported 2026-09-25 (second pass), see §3 |

> The port commits did **not** record the exact upstream SHA they were taken
> from. Baselines *audited on 2026-09-25* against the actual source trees (this is a
> full upstream port review — Bottle audited in full including unported features per explicit
> request; `open-shaders`/`jiayev` restricted to movement on already-ported surface only, per
> explicit request — not a commit-message-matching exercise; use these as the "since" point, then
> pin properly on the next re-sync):
> - `alandtse/open-shaders@dev` — `a6291d6f59` (was `95bacd821`). No movement in Cloud Relight's,
>   Foliage Lighting's, or Vanilla Fresnel's own feature files. Real findings: a Wind commit
>   (`f58cc4f61d`) that finishes the "local spring-field spatial variation"/grass-flutter capability
>   this doc lists as deferred for Wind; a possible latent GPU-state bug in `WindGrass.cpp`/
>   `WindTrees.cpp` (`6150e8d39e`); a 3-commit Procedural Sun / `Sky.hlsl` composition fix cluster
>   (`f537f4b9cf`, `a0eafffe21`, `1c0d36c350`); and a TRUE_PBR skin/hair guard fix
>   (`ce9367f300`→superseded by `25161eb4f3`) landing immediately adjacent to Vanilla Fresnel's/
>   Foliage Lighting's own hunks in `Lighting.hlsl`/`LightingCommon.hlsli`/`LightingEval.hlsli`. See
>   the Wind, Procedural Sun, and Vanilla Fresnel sections in §3 for detail.
> **2026-09-25, second pass (same day):** explicit follow-up request to fully align with Bottle
> across five areas, each landed as its own commit: (1) merged `community-shaders/dev` (72 commits
> behind; dev's own tip is an ancestor of Bottle, so Bottle already carries all of it) — the
> System-menu SEH crash guard added 2026-09-12 (`e8da56037f`) was removed in favor of dev's actual
> root-cause fix (`4163322011`/`25069c3f97`: session-gated ticking + function-pointer-only callback
> storage, fixing a GPtr-lifetime use-after-free); (2) Procedural Sun brought fully current — Bottle
> moved well past `26611dd183` (`31f9890f27`/`05c07ee292`/`343bf8cc44`/`3d64571711`): cloud
> "occlusion strength" replaced by a proper optical-depth "extinction" model with `sunVisibility`/
> `radianceLimit` HDR-aware additive blending and a new CLOUDS-pass cloud-darkening-near-sun effect;
> kept our own billboard anti-clip resize and EFFECTS11 sun-ownership arbitration, neither of which
> Bottle's file has; (3) **Light Limit Fix reverted, not advanced** — discovered Bottle's own
> content-hash-based caster caching (which this doc's table already listed as ported) was itself
> later replaced by a leaner heuristic scheduler in Bottle's "diet SLF" chain
> (`42e236274d`..`6ae45a32fa`..`7da7f6c4e6`) after a real shadow-flicker bug; re-synced to match,
> keeping our own fade-in-on-slice-assignment addition and fixing a `localShadowStatCollisions`
> stat that was never being reset; (4) ported Sky Scattering (`0657cf0ab4`) in full — see below;
> (5) FSR4 (`9ea5d7e64a`) investigated in depth and **explicitly deferred by user request** after
> quantifying the real blocker: `Upscaling.cpp`/`.h` have diverged from Bottle's pre-commit baseline
> by ~1700 lines (independent upscaler work on this fork), so Bottle's 119/22-line diff can't be
> applied mechanically — it would need a genuine hand merge in the GPU swapchain/frame-generation
> code, the least recoverable subsystem to get wrong, with no FSR4-capable hardware available here
> to verify the result actually renders. `FidelityFX.cpp/h` and `DX12SwapChain.h` do still match
> Bottle's pre-commit baseline byte-for-byte if a future pass wants to start from the easy end.
> - `InTheBottle/skyrim-community-shaders@Bottle-Compendium` — `dac6803377` (was `7c58cb1ee`).
>   **Full feature audit this pass** (not just movement in already-ported paths), per explicit
>   request. Found two undocumented ports already on `Personal` (Reverse Z, Footstep Particles —
>   added to the table above and §3, landed 2026-09-23 without a tracking-doc update) and
>   significant further movement in Light Limit Fix (contact-shadow rewrite + cache-scheduling
>   rework, landed *after* both the 2026-09-17 LLF re-sync and the Sept-23 port) that supersedes
>   what this doc previously recorded as "Present" — see the Light Limit Fix section in §3. Several
>   genuinely new, unported Bottle subsystems were also found (Physical Sun/Effects11 adaptation
>   integration, Sky Scattering rework for Effects11, `ENBDepthOfField`, FSR4 runtime-upscaler
>   split) — recorded in §7. Character Rain parity task closed as moot: Bottle reverted its own
>   character-wetness implementation (`3afd2b6277`), so there is no longer an upstream counterpart
>   to diff Personal's own implementation against.
> - `jiayev/skyrim-community-shaders@compendium-clean` — `330cb5dd30` (was `b8f93c390`). No movement
>   in Dynamic Cubemaps lighting-detection or Pseudo Sun Bounce. Advanced Skin's TRUE_PBR guard
>   coverage (`25161eb4f3`) confirmed already fully present locally, line-by-line — no action needed
>   beyond a cosmetic `Skin.ini` version bump upstream took that Personal hasn't (not a functional
>   gap). Post Processing: two `src/ShaderCache.cpp` robustness fixes (disk-cache probe exception
>   handling, `backgroundCompilation` atomic+notify) ported 2026-09-25 — see the Post Processing
>   section in §3; the same source commit's SSS null-guard fix ported alongside it, see the
>   Subsurface Scattering section in §3. (A prior note here claiming jiayev was missing from the README's
>   [Branch-Specific Credits](../../README.md#branch-specific-credits) was stale — it's listed there.)
> - `community-shaders/dev` (mainline, via the `cavy8` fork's synced `dev` branch) — `aebf01c2ef`.
>   `Personal` is fully merged through local `dev` (`66369e5c58`); mainline is one trivial commit
>   ahead (`feat: detect Dll version causing grass stutter (#2716)`, 1-line removal in
>   `RunGrass.hlsl` + `XSEPlugin.cpp` addition) that hasn't been pulled into local `dev` yet. Low
>   risk, doesn't touch any ported hunk.

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
  "src/Features/VanillaFresnel.cpp" "src/Features/VanillaFresnel.h" \
  "features/Wind" "src/Features/Wind" "src/Utils/ActorUtils.h" "src/Utils/ActorUtils.cpp" \
  "src/Utils/LazyShader.h" \
  "package/Shaders/Common/GrassWind.hlsli" "package/Shaders/Common/GrassWindResponse.hlsli" \
  "package/Shaders/Common/GrassWindSpring.hlsli" "package/Shaders/Common/TreeWind.hlsli" \
  "package/Shaders/Common/TreeWindSpring.hlsli" "package/Shaders/Common/WindField.hlsli" \
  "package/Shaders/Common/WindFieldTypes.hlsli" "package/Shaders/Common/TransientWindImpulse.hlsli" \
  "package/Shaders/Common/DampedSpring.hlsli" \
  "package/Shaders/GrassWindSpringCS.hlsl" "package/Shaders/TreeWindSpringCS.hlsl" \
  "features/Procedural Sun" "src/Features/ProceduralSun.cpp" "src/Features/ProceduralSun.h" \
  "package/Shaders/Sky.hlsl" "package/Shaders/Tests/TestProceduralSun.hlsl"

git log <LAST_SYNCED_SHA>..bottle/Bottle-Compendium --oneline -- \
  "features/Snow Cover" "src/Features/SnowCover.cpp" "src/Features/SnowCover.h" \
  "src/Utils/FormIdParser.cpp" "src/Utils/FormIdParser.h" \
  "package/Shaders/Lighting.hlsl" "package/Shaders/RunGrass.hlsl" "package/Shaders/DistantTree.hlsl" \
  "package/Shaders/Common/SharedData.hlsli" "package/Shaders/Common/Permutation.hlsli" \
  "package/Shaders/Common/Color.hlsli" \
  "features/ReverseZ" "src/Features/ReverseZ.cpp" "src/Features/ReverseZ.h" \
  "features/Footstep Particles" "src/Features/FootstepParticles.cpp" "src/Features/FootstepParticles.h" \
  "package/Shaders/Common/FrameBuffer.hlsli" "package/Shaders/DeferredCompositeCS.hlsl" \
  "package/Shaders/Effect.hlsl" "package/Shaders/Utility.hlsl" "package/Shaders/Water.hlsl" \
  "src/ShaderCache.cpp" \
  "features/Light Limit Fix/Shaders/LightLimitFix/LightLimitFix.hlsli" \
  "src/Features/LightLimitFix.cpp" "src/Features/LightLimitFix.h"

git log <LAST_SYNCED_SHA>..jiayev/compendium-clean --oneline -- \
  "features/Post Processing" "src/Features/PostProcessing.cpp" "src/Features/PostProcessing.h" \
  "package/Shaders/ISHDR.hlsl" "features/HDR Display/Shaders/HDRDisplay/HDROutputCS.hlsl" \
  "src/ShaderCache.cpp" "src/ShaderCache.h" "src/Utils/D3D.h" "src/Utils/D3D.cpp" \
  "src/Features/Effects11.cpp" "src/State.h" \
  "package/Shaders/Common/SharedData.hlsli" \
  "features/Pseudo Sun Bounce" "src/Features/PseudoSunBounce.cpp" "src/Features/PseudoSunBounce.h" \
  "package/Shaders/Lighting.hlsl"
```

GitHub compare URLs (paste the synced SHA in for a quick browser diff):

- `https://github.com/alandtse/open-shaders/compare/<SHA>...dev`
- `https://github.com/InTheBottle/skyrim-community-shaders/compare/<SHA>...Bottle-Compendium`
- `https://github.com/jiayev/skyrim-community-shaders/compare/<SHA>...compendium-clean`

~~Also watch the `snow-rework` branch on the Bottle fork~~ — **retired 2026-09-25.**
`bottle/snow-rework` (tip `a661e44600`) is an ancestor of current `Bottle-Compendium`
(`git merge-base --is-ancestor bottle/snow-rework bottle/Bottle-Compendium` is true) — its content
was folded into mainline `Bottle-Compendium` history before the `7c58cb1ee` baseline and is the
same source material Snow Cover is already synced against. The branch has no commits past
2026-09-02 and is stale/abandoned. Nothing further to watch here.

### 1b. This fork's own upstream (mainline Community Shaders)

Every port edits shared base-shader files (`Lighting.hlsl`, `RunGrass.hlsl`,
`Sky.hlsl`, `DistantTree.hlsl`, `Common/*.hlsli`) and shared C++ registration
files. When **mainline** CS changes those same files, the ported hunks are the
most likely merge-conflict / silent-breakage site. After any merge from
mainline, re-verify each feature's hunks below still apply and still compile
with the feature define forced (see [shader-workflow.md](shader-workflow.md) and
the porting notes in `.claude` memory).

**2026-09-25 status (superseded same day, see correction below):** `Personal` is fully merged
through local `dev` (`66369e5c58` — verified via `git merge-base Personal dev` equalling `dev`'s
tip). Mainline (`community-shaders/dev`) was one trivial commit ahead: `aebf01c2ef` "feat: detect
Dll version causing grass stutter (#2716)".

**Correction, later the same day (2026-09-25 implementation pass):** re-checking
`community-shaders/dev` mid-pass found it had jumped to `b4e7b08ec4` — **73 commits** ahead of local
`dev`, not one. The `aebf01c2ef` baseline above was accurate at the time it was recorded a few hours
earlier; the fork's mirror of upstream mainline caught up in between (this environment's editor
auto-fetches remotes periodically, so the jump was observed rather than triggered by anything in
this pass). `git log dev..community-shaders/dev --oneline` lists all 73; skimmed titles include
several `fix(effects11)`/`fix(shadercache)`/`fix(shadows)` items and the sortable-profiler-tables
feature already cross-referenced in §7's Bottle section. **Not merged in this pass** — 73
unreviewed upstream commits is a substantial, separate undertaking (conflict resolution across
whatever files they touch, then a full re-verify of every §2 hot file per this section's own
standing instruction), not something to fold into a Bottle-focused sync pass. Local `dev`/`Personal`
should get a dedicated mainline-merge pass soon; the gap will only grow.

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
- **Present (as of the 2026-09-17 pass only)** — describes exactly which Bottle-head improvements
  from that pass were pulled forward; **superseded by further Bottle movement, see below.**

**2026-09-25 review — significant further Bottle movement, found pending re-sync:**

Bottle kept reworking diet-SLF after both the 2026-09-17 re-sync above and the undocumented
2026-09-23 Reverse Z/Footstep Particles port (which itself touched 95 lines of
`LightLimitFix.hlsli` for the reverse-Z depth convention — already captured). Direct file-diff
against current Bottle head (`dac6803377`) confirmed `LightLimitFix.cpp` diverged 308 lines,
`LightLimitFix.h` 56 lines, `LightLimitFix.hlsli` 229 lines:

- **`d05a80bb00`** "chore: improve contact shadows" and **`dac6803377`** "feat: tiny glade like
  contact shadows" — a full rewrite of the contact-shadow ray representation the 2026-09-17 pass
  ported: replaces `ContactShadowRay{clipOrigin,clipStep, viewDepth}` with a UV-space
  `{uvOrigin,uvDelta,inverseDepth,tMax}` form, exponential march (`CONTACT_SHADOW_MARCH_EXPONENT`),
  and a new end-fade constant. **Ported 2026-09-25** (see below) — depended on
  `FrameBuffer::CameraProj`/`DynamicResolutionParams1`, i.e. the already-ported Reverse Z work, and
  was done after confirming Reverse Z was fully current.
- **`6ae45a32fa`** "fix: SLF shadow flicker" (185/20 lines `LightLimitFix.cpp/h`) — replaces the
  starvation/`contentHash`/`cachedGeomHash`/`LOCAL_SHADOW_STARVED_SCORE` cache-invalidation model the
  2026-09-17 pass ported with a different reclaim-priority model (`LOCAL_SHADOW_ACTOR_SCORE = 1000`,
  `LOCAL_SHADOW_AGE_URGENCY = 64`, a static `IsLocalShadowSliceReclaimable()` helper). **Not yet
  re-synced** — this directly supersedes what's documented as "Present" in the 2026-09-17 pass above
  and still needs a full re-diff against current Bottle, not a patch on top of the 2026-09-17 work.
  Deferred out of the 2026-09-25 pass to keep that pass's contact-shadow rework reviewable on its
  own; still the single largest outstanding LLF gap.
- **`a0ac9312e8`** "fix: unforce bias LLS" — **ported 2026-09-25** (see below).

**2026-09-25 implementation pass — contact-shadow rewrite ported.**

Replaced `ContactShadowRay`/`GetPixelLimitedSteps`/`MayBeOccluded`/`ContactShadows` in
`LightLimitFix.hlsli` with Bottle's current-head algorithm verbatim (perspective-correct UV-space
ray with linearly-interpolated inverse depth, `tMax` clipping the ray to the screen up front instead
of an in-loop `IsSaturated` bailout, exponential step packing toward the shaded point, a relative
depth bias to fight acne, a single-hit ray-end-fade occlusion model replacing the old
accumulate-and-max one, and `GatherRed` instead of four `Load()`s for the bilinear scene-depth pair).
Dropped the now-unused `IsSaturated()` helper, matching Bottle. `ContactShadows()`/
`GetContactShadowSceneDepths()`/`MayBeOccluded()` now take a `SamplerState` parameter (`GatherRed`
needs one); the `Lighting.hlsl` call site passes `LinearSampler` (already in scope there, used by the
adjacent `GetLocalShadow()` call).

Settings-schema changes that came with it (byte-matched on both the C++ and HLSL sides, `pad1`
widened to `float[2]` to keep `LightLimitFix::PerFrame`/`LightLimitFixSettings` at the same 80-byte
size so every field after it in `FeatureData` keeps its offset — see §2):
- `ContactShadowStride` (per-step distance, default 2.0) → `ContactShadowLength` (total ray length,
  default 8.0) — the new ray is built once from a length, not accumulated step-by-step.
- `ContactShadowThickness` (0–1 fractional, default 0.2) → `ContactShadowDepthThickness` (1–128 game
  units, default 16.0) — thickness is now compared directly against absolute depth deltas.
- `ContactShadowDepthFade` **removed** — the old accumulate-and-max falloff model it fed doesn't
  exist anymore; the new model's single ray-end fade (`CONTACT_SHADOW_RAY_END_FADE`) replaces it.
- Also folded in `a0ac9312e8` "fix: unforce bias LLS" while touching the same settings list:
  removed `LocalShadowBiasScale` (default 0.25) and its UI slider — local-shadow depth bias is no
  longer user-scaled, matching Bottle.
- `LightLimitFix.ini` bumped `3-3-0` → `3-4-0`. i18n re-extracted/sorted; `contact_shadow_stride`/
  `contact_shadow_depth_fade`/`local_shadow_bias` keys are gone, `contact_shadow_length` is new,
  `contact_shadow_thickness`'s tooltip text changed under the same key.

Verified: `BuildDevFast` clean (no new warnings); `Lighting.hlsl` force-compiled with `fxc` for
`PSHADER+LIGHT_LIMIT_FIX+DEFERRED` (with and without `REVERSE_Z`) and the non-deferred forward path.
Not tested in-game. **`d05a80bb00`/`dac6803377`'s contact-shadow rework and `a0ac9312e8`: Present.**

**2026-09-25, second pass — scheduling/cache-invalidation revert, not advancement.** The
`6ae45a32fa` scheduling rework marked "Deferred" above was a misreading: what Personal actually had
here (content-hash-based caster caching, geometry rehashing, a `starved` flag) was itself a *port
of an earlier, more complex* Bottle iteration than what's at Bottle's current head. Bottle's "diet
SLF" chain (`42e236274d` "feat: diet SLF" → `a0fe19bcae` → `8421c9cca2` → `7da7f6c4e6` →
`dbcc02ffc3` → `6ae45a32fa` "fix: SLF shadow flicker" → `4a1b4acd9a`) **replaced** that content-hash
system with a leaner admission-budget/importance-urgency heuristic (`IsLocalShadowSliceReclaimable`,
actor-preemption in `AcquireLocalShadowSlice`, `LOCAL_SHADOW_AGE_URGENCY`/`LOCAL_SHADOW_ACTOR_SCORE`)
specifically to fix a shadow-flicker bug. Reverted `LightLimitFix.cpp`/`.h` to match — removed
`ComputeLocalShadowContentHash`, `cachedGeomHash`/`cachedGeomFrame`/`cachedGeomCount`/
`skinnedCasters`/`radiusAnchor`/`starved`/`contentHash`/`renderedContentHash`, restored
`IsLocalShadowSliceReclaimable` and the newcomer-admission-budget logic. Also picked up
`7da7f6c4e6`'s `light.lightFlags.reset(LightFlags::Shadow, LightFlags::ShadowCaster,
LightFlags::LocalShadow)` call on both constant-point-light setup paths (was missing entirely).
Kept two Personal-only additions Bottle's file doesn't have: the `assignedFrame`-based fade-in for
newly assigned shadow slices (`LOCAL_SHADOW_FADE_FRAMES`), and `localShadowStatCollisions` — which
turned out to have never been reset each frame; now fixed alongside the revert. No settings-schema
change (this is pure C++ scheduling logic, no `LightLimitFix::Settings`/`.ini` involvement). Verified:
`BuildDevFast` clean. Not tested in-game — this changes real shadow-caster eviction/admission
behavior and deserves an in-game look for flicker/pop regressions before shipping.

### Reverse Z  (`InTheBottle/skyrim-community-shaders@Bottle-Compendium`)

Ported `124bc7e22` (feature) + `1698b4beb` (shared depth-convention edits), 2026-09-23 —
**undocumented until this 2026-09-25 review.** Rewrites depth handling across `FrameBuffer.hlsli`
(new), `DeferredCompositeCS.hlsl`, `Effect.hlsl`, most `IS*.hlsl` post-process shaders,
`Lighting.hlsl`, `Utility.hlsl`, `Water.hlsl`, `LightLimitFix/ClusterBuildingCS.hlsl`,
`GrassOptimizations/*` (culling/Hi-Z/SPD), `ScreenSpaceGI/common.hlsli`,
`ScreenSpaceShadows/RaymarchCS.hlsl`/`bend_sss_gpu.hlsli`, `TerrainBlending/DepthBlend.hlsl`,
`DynamicCubemaps/CaptureCommon.hlsli`. Given this blast radius, most apparent "unrelated churn" in
`Lighting.hlsl`/`SharedData.hlsli` on any future Bottle diff is likely Reverse Z, not a separate
feature — check here first before assuming a new port target.

**2026-09-25 review — residual follow-up fixes — closed in the 2026-09-25 implementation pass:**

- `src/ShaderCache.cpp` diverged 86 lines from Bottle head. **`9a6d3b2426`** "fix: reverse z map" —
  ported: un-commented the `BSImagespaceShaderWorldMap`/`BSImagespaceShaderWorldMapNoSkyBlur`/
  `BSImagespaceShaderISSnowSSS` descriptor-table entries and added the `reverseZOnly` guard in
  `GetImagespaceShaderDescriptor()` that rejects the first two unless Reverse Z is active (needed
  `#include "Features/ReverseZ.h"` for the full type). **`0a9f8f8ac0`** "fix: reverse z
  imagespaces" — un-commented `BSImagespaceShaderISSAOCameraZ` in the same table (a same-evening
  sister commit to the one above, not a separate imagespace concern). **`3b9a9dc3c9`**'s
  `ShaderCache.cpp` piece — ported: `EnqueueStandaloneShaderCompile` now pushes a `REVERSE_Z` define
  when `globals::features::reverseZ.IsActive()`, since Post Processing's standalone compile path
  (unlike the main permutation matrix) never went through the generic per-feature `HasShaderDefine`
  loop. One divergence investigated and **left alone**: Bottle's `GetUtilityShaderDefines()` pushes
  a `REVERSE_Z` define for `BSShader::Type::Utility` that Personal lacks — confirmed a no-op here,
  since Personal's `Utility.hlsl` gets its Reverse-Z awareness from runtime `FrameBuffer::
  IsReverseProjection()` calls inside `FrameBuffer::ToNativeDepth()`/`ToStandardClip()`/etc., not
  from a `#ifdef REVERSE_Z` branch — grepped and confirmed `Utility.hlsl` has zero references to the
  macro on either branch. Adding the define would do nothing; not ported.
- `package/Shaders/Effect.hlsl` diverged 45 lines total, but investigated down to the single commit
  (`42d5129c75` "fix: reverse z effect shaders") that's actually Reverse-Z-scoped: the `SKY_OBJECT`
  view-proj depth-row fix (`skyObjectDepthRow`) — confirmed **already present, byte-identical**. The
  remaining ~40 lines of divergence are unrelated, not-yet-ported Effects11 particle-intensity/
  light-sprite and Exponential Height Fog `useVanillaFogSettings` work from the same file region;
  out of scope for this pass, not a Reverse Z gap.
- `package/Shaders/PostProcessing/MotionBlur/motionblur_*.cs.hlsl` (4 files) +
  `src/Features/PostProcessing/MotionBlur.cpp`, from `3b9a9dc3c9` "fix: Motion blur and DOF PP z" —
  ported. `motionblur_blurpass.cs.hlsl` had never picked up depth linearization at all (matches
  jiayev's own upstream, which still samples `TexDepth` raw) — replaced with
  `SharedData::GetScreenDepth(...)` (Reverse-Z-aware), added the `VelocityToBlurPixels()` helper,
  fixed `sampleCount` clamping/`pixelToSampleUnitsScale`/`offsetLen` math bugs, and renamed
  `MB_SOFTZ_INCHES`→`MB_SOFTZ_GAME_UNITS`; whole-file diff against Bottle head confirmed this was the
  *only* divergence, so the file was replaced wholesale rather than hand-patched. The other three
  `.cs.hlsl` files each had one bug: `saturate()` was incorrectly clamping the encoded velocity
  magnitude to 0..1 before it's later multiplied back out — removed, matching Bottle. Also found and
  fixed two further `MotionBlur.cpp` gaps while diffing against Bottle head that predate
  `3b9a9dc3c9` (not Reverse-Z-scoped, just previously-missed correctness issues): a missing
  `ClearUnorderedAccessViewFloat` on `blurOutputTexture` at (re)creation, and a missing
  zero-dispatch guard before `context->Dispatch()`. (One further Bottle difference —
  `dispatchY` computed from `horizontalPassTexture->desc.Height` instead of `dynamicHeight` —
  was investigated and is a **regression in Bottle**, not a gap in Personal: Personal already uses
  `dynamicHeight` consistently for dynamic-resolution correctness; left as-is, not "fixed" to match
  Bottle.)
- `src/Features/ReverseZ.cpp` itself is confirmed **byte-identical** to current Bottle head — the
  `1e88b27471` "fix: reverse z interior perk menu" fix (68 lines) is already captured.
- Verified: `BuildDevFast` clean (no new warnings).

### Footstep Particles  (`InTheBottle/skyrim-community-shaders@Bottle-Compendium`)

Ported `124bc7e22`, 2026-09-23 — **undocumented until this 2026-09-25 review.** Direct port,
feature-owned files only (`features/Footstep Particles/**`, `src/Features/FootstepParticles.cpp/h`,
934/177 lines). No shared-file injection points beyond feature registration
(`src/Deferred.cpp`, `src/Feature.cpp`). Not independently checked for upstream movement since the
port SHA this pass — no specific gap flagged, but also not confirmed current; a dedicated
path-filtered `git log` against `features/Footstep Particles` / `src/Features/FootstepParticles.*`
is still owed on the next re-sync pass.

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
- **2026-09-25 review: no movement.** `git log 7c58cb1ee..bottle/Bottle-Compendium` against Snow
  Cover's own paths is empty; the hot-file filter (`Lighting.hlsl`/`RunGrass.hlsl`/
  `DistantTree.hlsl`/`Color.hlsli`) shows commits but none touch a `SnowCover::` call site
  (verified via `grep "SnowCover::"` on the diff) — that churn is entirely Reverse Z/Procedural
  Sun/cloud-shadow work in the same files. No re-sync needed.

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
- **2026-09-25 review: no movement in Cloud Relight's own files**
  (`features/Cloud Relight/**`, `src/Features/CloudRelight.cpp/h`, the Cloud Shadows hook). But
  **`a0eafffe21`** "fix(ll): correct sky composition" (#738, 2026-09-21) reworks `Sky.hlsl`'s color
  composition under Linear Lighting (`ENABLE_LL`) — introduces `ComposeSkyColor()`/
  `composeAuthoredSky` threaded through `PS_OUTPUT main()`, changing when `Color::Sky()` applies
  (pre- vs post-multiply) across base/blend/cloud-relight/procedural-sun/HDR-sun/moonmask/horizfade
  paths, and directly touches the `CR_CLOUDS` block Cloud Relight's own hook sits inside. This needs
  a careful diff (not a mechanical cherry-pick) next time `Sky.hlsl` is touched — bundle with the
  Procedural Sun re-sync below, since both hook the same function and the same commit landed
  alongside two Procedural Sun fixes the same evening. The `linearLightingSettings` field set Cloud
  Relight reads has **not** moved (zero diff on `SharedData.hlsli` for that struct in this range).

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
- **2026-09-25 review: no movement** in Foliage Lighting's own files. The deferred-item blocker
  check was re-verified: `package/Shaders/Common/DirectionalShadow.hlsli` exists on current
  `open-shaders@dev` but is **unchanged** since the `95bacd821` baseline — the blocker situation is
  identical to 2026-09-17, still correctly deferred, nothing new to re-evaluate. `Math.hlsli` (where
  Personal's local `SafePow` lives) also has zero upstream commits in this range — the de-dup
  concern remains hypothetical, no action.

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
- **2026-09-25 review: no movement in Vanilla Fresnel's own files**, but two commits land right
  next to its hunks in the same three shared files: **`ce9367f300`** "fix(skin): guard non-PBR
  material access" (#716, 2026-09-19) — narrows a `SKIN`/`CS_SKIN` guard in `Lighting.hlsl` (~L2094)
  immediately after a `VANILLA_FRESNEL`/`TRUE_PBR` `#endif` block — and **`25161eb4f3`**
  "fix(shaders): compile skin and hair under TRUE_PBR" (#2738, 2026-09-24, supersedes `ce9367f300`)
  — generalizes the same fix into `CS_SKIN_SHADING`/`CS_HAIR_SHADING` macros used at ~15 guard sites
  across `Lighting.hlsl`/`LightingEval.hlsli`/`LightingCommon.hlsli`. Not a Vanilla Fresnel content
  change, low urgency (guard-only, no visible artifact), but a straight cherry-pick of either commit
  will likely conflict against Personal's local Vanilla Fresnel/Foliage Lighting hunks in the same
  files — fold in opportunistically next time these files are touched for either feature. Note:
  `#2738`'s equivalent fix already reached Personal via `jiayev`'s shared-ancestor commit and is
  confirmed fully present (see Advanced Skin section) — this is the `open-shaders` side of the same
  underlying upstream fix propagating through a different fork; no separate action needed once the
  guard-adjacency is acknowledged.

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
- LUT/bokeh textures renamed for RenderDoc debuggability.
- **2026-09-17:** restored `features/Post Processing/textures/rgbnoise.dds` and `starburst.dds`
  from `jiayev/skyrim-community-shaders@compendium-clean`, byte-identical to the source blobs.
  Neither texture is sampled by any code on either branch (`git grep` for `rgbnoise`/`starburst`
  turns up only UI strings and the unrelated procedurally-generated `PhysicalGlare` starburst
  pattern) — they're unused upstream too, but the desired Personal implementation keeps them in
  the tree rather than omitting them. No packaging changes needed: `CMakeLists.txt` globs and
  copies each feature folder recursively, so a non-empty `textures/` subfolder is picked up
  automatically. **Included.**
- `shader-validation.yaml` has no Post Processing–specific defines beyond what CI already
  exercises (149 PP permutations + 9 ISHDR permutations were fxc-validated at port time,
  not by hlslkit) — force-compile with `fxc` rather than trusting a green hlslkit run alone.
- Marked alpha at port time — confirm current release stage in
  `features/Post Processing/Shaders/Features/PostProcessing.ini` before assuming defaults.
- **2026-09-25 review: two `src/ShaderCache.cpp` robustness fixes, both from `c321154fa4` "fix:
  shader wakeups, cache probes and SSS guards" (#2722, 2026-09-21) — ported in the 2026-09-25
  implementation pass:**
  - **Disk-cache probe exception safety.** Changed `std::filesystem::exists(diskPath)` (the
    throwing overload) to the `std::error_code`-taking overload at both call sites — the main
    permutation-matrix disk-cache check (~L1398) and, additionally, Personal's own
    `EnqueueStandaloneShaderCompile` disk-cache check (~L2536, not from this upstream commit — it's
    Personal-only code with the identical bug pattern, fixed the same way) — "a failed filesystem
    probe is a cache miss, not a failed compilation task." Also removed a redundant inner
    `std::filesystem::exists(shaderSourcePath)` throwing check, since the immediately-following
    `last_write_time(shaderSourcePath, ec)` already reports the same failure via `ec`.
  - **`backgroundCompilation` synchronization.** `bool backgroundCompilation` → `std::atomic_bool`,
    plus a new `ShaderCache::SetBackgroundCompilation(bool)` that locks `compilationMutex`, sets the
    flag, and notifies `conditionVariable` — `CompilationSet` gained `friend class ShaderCache;` so
    the setter can reach its private `conditionVariable`. `Complete()` erases the entry before
    notifying, so flipping the mode while the dispatcher thread is parked in
    `conditionVariable.wait()` previously wouldn't wake it. Only `src/Menu.cpp`'s
    `SkipCompilationKey` hotkey handler was switched to the setter (matching upstream's own scope);
    `src/XSEPlugin.cpp`'s boot-time toggle was left as a plain `= true` assignment — upstream doesn't
    touch it either, and `std::atomic_bool` still supports direct assignment (it just skips the
    wake, which doesn't matter at boot before the dispatcher is parked waiting).
  - **Not applicable:** the same commit also swaps `TryTakeNext`'s admission-budget check from
    `compilationPool.get_tasks_total()` to `tasksInProgress.size()` to fix a related race — Personal's
    `TryTakeNext` already uses a dedicated `std::atomic<uint32_t> dispatchedTasksInFlight` counter (a
    different, race-free mechanism), so this part doesn't apply.
  - The same commit's third component, **SSS guards**, is not Post-Processing-scoped — see
    Subsurface Scattering below.
  - Verified: `BuildDevFast` clean (no new warnings).

### Subsurface Scattering  (`jiayev/skyrim-community-shaders@compendium-clean`, via the same `c321154fa4` commit as Post Processing above)

Not a tracked port — Subsurface Scattering is native to Personal, not sourced from any fork — but
`c321154fa4` "fix: shader wakeups, cache probes and SSS guards" (#2722) also carried a crash fix in
`src/Features/SubsurfaceScattering.cpp` that applied here unmodified: `DataLoaded()` called
`RE::TESForm::LookupByEditorID("IsBeastRace")->As<RE::BGSKeyword>()` unconditionally — a null-pointer
dereference if a mod ever removes or renames that keyword. **Ported 2026-09-25**: null-checks the
`LookupByEditorID` result before dereferencing, logs a warning and falls back to
`isBeastRaceKeyword = nullptr` when missing, and `BSLightingShader_SetupSkin` now skips the
`HasKeyword(isBeastRaceKeyword)` branch entirely when the keyword never resolved (previously would
have called `HasKeyword(nullptr)`). Verified: `BuildDevFast` clean.

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
- **2026-09-25 review: no movement.** `package/Shaders/Common/PBR.hlsli` is byte-for-byte unchanged
  against current Bottle head since `7c58cb1ee`. `src/TruePBR.cpp/h` show large diffs (240/62
  lines) but these come entirely from an unrelated Bottle feature ("per-object material override")
  that was added and then fully reverted in the same window (`a6d5d34d4a` "revert per object
  mato") — net zero effect on the ported micro-shadow-AO surface.

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
- **2026-09-25 review: effectively no movement.** `VolumetricLighting.cpp/h`/`.ini` are byte-for-
  byte unchanged since `7c58cb1ee`. The only touched file in the god-ray path filter is
  `ISVolumetricLightingGenerateCS.hlsl` (5 lines, from the Reverse Z commit `6db6512c96`) — already
  byte-identical between local HEAD and Bottle head, captured by the Reverse Z port. No action.

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
- **2026-09-25 review (jiayev): no movement.** Zero commits in `b8f93c390..jiayev/compendium-clean`
  touch `CaptureCommon.hlsli`, `DetectCaptureLightingCS.hlsl`, `InferCubemapCS.hlsl`,
  `UpdateCubemapCS.hlsl`, or `src/Features/DynamicCubemaps.cpp/h`.

### Wind  (`alandtse/open-shaders@dev`)

**Upstream source anchor**: `95bacd8211fec29a46ada67a65c237a02bf219c9` (dev tip audited 2026-09-17),
introduced at `7acc8d70ad1dd60fb3ec46319a1f836f5d4065ca` (feat(wind): add shared wind field system,
#634) but evolved substantially since — use current `dev`, not that original commit, as the
source of truth for any re-sync.

**Upstream source paths**

- `src/Features/Wind/**` (feature-owned: `Wind.h/.cpp`, `WindField`, `WindGrass`, `WindTrees`,
  `WindUpdate`, `TransientWindImpulse`, `Grass/`, `Trees/`, `Runtime/`, `Settings/`, `UI/`,
  `WindEffects/` — Fus Ro Dah, Dragon, SpellShout, StormCall, ProjectileMagic, WeaponThrowVR,
  Explosion, HeavyImpact routers)
- `features/Wind/CORE`, `features/Wind/Shaders/Features/Wind.ini`
- `package/SKSE/Plugins/CommunityShaders/WindSettings/**`
- `package/Shaders/Common/GrassWind.hlsli`, `GrassWindResponse.hlsli`, `GrassWindSpring.hlsli`,
  `TransientWindCulling.hlsli`, `TransientWindImpulse.hlsli`, `TreeWind.hlsli`,
  `TreeWindSpring.hlsli`, `WindField.hlsli`, `WindFieldTypes.hlsli`, `DampedSpring.hlsli`
- `package/Shaders/GrassWindSpringCS.hlsl`, `TreeWindSpringCS.hlsl`,
  `package/Shaders/Tests/TestWindField.hlsl`, `WindFieldParitySamples.hlsli`

**Shared-file injection points**

| File | Region |
| --- | --- |
| `src/State.h` / `package/Shaders/Common/Permutation.hlsli` | `PermutationCB`/`PerShader`: `TreeBend` `ExtraShaderDescriptors` bit (`1 << 13`); per-mesh tree-bend fields (`TreeBendModelSensitivity`…`TreeWindProbeTop`) set by `Wind::OnTreeBendRenderPassBegin`; whole-frame grass/tree defaults (`WindIntensityOverride`…`GrassWindCompressionToBend`) set once/frame by `Wind::Reset()` from `Wind::GetPermutationContribution()` |
| `package/Shaders/Common/SharedData.hlsli` | `FeatureData : register(b6)`: flat `WindFieldTuning`/`WindFieldAmbient`/`WindFieldCurrent`/…/`WindFieldTransientImpulses[]` fields (not a nested struct — `WindField.hlsli` references these directly as `SharedData::WindFieldXxx`), appended after `volumetricLightingSettings`; matches `Wind::GetSharedWindData()`/`WindSharedData` byte-for-byte |
| `src/FeatureBuffer.cpp` | `globals::features::wind.GetSharedWindData()` appended as the last `_GetFeatureBufferData` argument |
| `src/Globals.h/.cpp`, `src/Feature.cpp` | feature registration (forward decl/extern/instance/`GetFeatureList()` entry) |
| `package/Shaders/RunGrass.hlsl` | `CalculateWindDisplacement` — ambient-field blend on top of vanilla sway, gated by `Permutation::EnableAmbientGrassWind`; both call sites now pass a computed `windWorldPosition`; `GRASS_OPTIMIZATIONS` path scaled by `GrassWind::GetWindIntensityOverrideScale()` only |
| `package/Shaders/Lighting.hlsl` | new block after `vsout.Position = viewPos;`, inside `#if defined(TREE_ANIM) && !defined(SKINNED)`, runtime-gated by the `TreeBend` bit — trunk displacement via `TreeWind::SampleCurrent`/`GetWorldDisplacement`, recomputes `viewPos` via `mul(ViewProj, worldPosition)` only when the bit is set |
| `package/Shaders/Common/Math.hlsli` | `EPSILON_WIND_*`/`EPSILON_DAMPED_SPRING_*` constants (were missing from upstream's own diff footprint here, needed by the copied Wind `.hlsli` files) |
| `src/Utils/ActorUtils.h/.cpp` | `IsDragon`, `GetVisualOrigin`, `GetMagicOrigin`, `GetAimDirection` merged into this repo's **pre-existing** `ActorUtils` (from "Port character wetness from openshaders") — do not replace that file wholesale on a re-sync, it also owns `ForEachLoadedActor`/`ForEachActorGeometry`/`ForEachHeldWeaponGeometry`/`GetShapeBound`/`ExtractShapeBound`, used by `CharacterRainSurfaces.cpp` |
| `src/Utils/FileSystem.h/.cpp` | `PathHelpers::GetWindSettingsPath()` |
| `src/Utils/LazyShader.h` | added; trimmed to drop the `ID3DBlob` specialization (`Util::CompileShaderBlob` doesn't exist here, and Wind only instantiates `LazyShader<ID3D11ComputeShader>`) |

**Local adaptations from upstream's Feature/render-pass framework, which this repo doesn't have**

- Dropped entirely (dead code with no caller in this repo): `SupportsVR`, `GetDiagnostics`,
  `RegisterUxActions`, `GetRuntimeFlags`/`SetRuntimeFlag` (devbench/UX-action registry —
  `Utils/DevBenchUx.h` doesn't exist here), and the page-scoped `HasScopedDefaultSettings`/
  `RestoreCurrentPageDefaultSettings`/`HasScopedOverrideSettings`/`ReapplyCurrentPageOverrideSettings`
  (a generic per-page reset/reapply dispatch this repo's Menu never calls — confirmed no other
  caller anywhere in `src/`).
- `Feature::OnRenderPassBegin`/`WantsRenderPassHook` (a generic per-pass hook virtual this repo's
  `Feature` doesn't have) reimplemented as a self-installed `BSLightingShader`/`BSUtilityShader`
  `SetupGeometry` vfunc hook (`Wind::Hooks::Install`, called from `PostPostLoad`) — the same idiom
  `TerrainVariation`/`SnowCover` already use here. No restore-after-draw closure needed: the hook
  clears the `TreeBend` bit unconditionally at the top of every call, and `State::Update()` already
  diffs+reuploads `PermutationCB` on every draw when it changes, same as `TerrainVariation`'s
  `ExtraFeatureDescriptor` flag.
- `FeatureCategories::kFoliage` doesn't exist here; used `kGrass`.
- `globals::game::isVR` doesn't exist (no VR build target) — `WeaponThrowVRWind::DataLoaded()`'s
  VR-gate replaced with an unconditional early return (VR weapon-throw wind tracking permanently
  inert here; the router itself has no VR-only SDK dependency, so it was still worth keeping as a
  no-op rather than deleting).
- `TreeWindSpring.hlsli`'s local spring-field texture cache (`register(s14)`, `t111`-`t126`)
  collides with `Lighting.hlsl`'s pre-existing `SampShadowMaskSampler : register(s14)`, and isn't
  wired to any C++-side texture/SRV binding here anyway. Its VS-facing functions
  (`SampleCurrent`/`SamplePrevious`/`TrySample{Current,Previous}Transient`) were replaced with the
  same "plain ambient field, no local cache" fallback every caller already treated as the
  out-of-cache case. The compute-only branch (`TREE_WIND_SPRING_COMPUTE`, used by
  `TreeWindSpringCS.hlsl`) is untouched.

**Capability status — Partial port.** What works: CPU-side simulation (ambient procedural field,
weather-driven direction/speed, transient impulse routing from Fus Ro Dah/dragons/spells/shouts/
storm call/explosions/heavy impacts/projectiles), feature registration, settings UI, and the GPU
side reaches real geometry — grass gets ambient-field-driven sway (direction/speed/gust, plus
transient impulses via `WindField::SampleCurrent`) and full-detail (non-LOD) trees get trunk bend
from the ambient field, gated by the per-mesh `TreeWindPatcher` sensitivities. Verified: C++ build
clean; `RunGrass.hlsl` and `Lighting.hlsl` vertex shaders force-compiled with `fxc` across the
relevant permutations (plain, `GRASS_OPTIMIZATIONS`, `GRASS_LIGHTING`, `TREE_ANIM`,
`TREE_ANIM`+`SKINNED`). **Not yet tested in-game.**

What's deferred, not yet present:
- **Local spring-field spatial variation** (grass and tree wind response smoothed/cached per-cell
  via `GrassWindSpringCS.hlsl`/`TreeWindSpringCS.hlsl` compute output) — the compute shaders exist
  and compile, but nothing binds their output textures to the vertex shaders; both grass and tree
  response currently use the direct procedural field only, no local caching/smoothing layer.
- **Tree transient impulses** (shouts/dragons visibly bending tree trunks) — `TreeWind.hlsli`'s
  transient sampling exclusively goes through the now-stubbed `TreeWindSpring::TrySample*Transient`
  (always returns no sample); trees get ambient sway only. Grass is unaffected by this — its
  transient response goes through `WindField::SampleCurrent` directly, not the spring cache.
- **`GRASS_OPTIMIZATIONS` path** — only gets the wind-intensity-override scale; full ambient-field
  sampling there needs Grass Optimizations' own `InstanceExtras`-building compute shader to sample
  the wind field and pack a response per instance, which wasn't touched.
- **Grass Collision interaction** — not reviewed; the `GRASS_COLLISION` fxc permutation wasn't
  force-compiled (blocked on an unrelated pre-existing include-path issue in the verification
  pass, not a code defect from this port — needs re-checking with a correct `/I` root).
- Leaf-specific flutter refinement (`Sample.leafAnimationStrength`) computed but not yet consumed
  by any leaf-normal perturbation in the vertex shader — trunk bend only.
- Devbench diagnostics/UX-action integration (see above) — intentionally dropped, not deferred.

**2026-09-25 review — high-value movement found, changes the shape of the deferred items above:**

- **`f58cc4f61d`** "feat(wind): grass flutter and ambient tuning" (#760, 2026-09-25, newest commit
  audited in this whole pass). This is upstream **finishing the exact "local spring-field spatial
  variation" deferred item above** — not a small fix. Adds a new `WindField::Components
  .ambientTurbulence` field (`WindFieldTypes.hlsli`, threaded through `WindField.hlsli`'s
  `SampleField`/`SampleCurrentComponents`); `GrassWindSpringCS.hlsl` now computes and **stores a
  per-cell "flutter" value in the alpha channel of its `Response`/`Velocity` output textures**,
  driven by ambient turbulence, gust envelope, and a transient-impulse-driven flutter with
  exponential half-life decay (`TransientFlutterHalfLife = 0.15f`) — the spring-field output is
  finally being *consumed*, not just computed-and-unbound; `GrassWindResponse.hlsli`'s
  `ComputeWindResponse` now reads that per-cell flutter out of the spring-field sample
  (`currentSample.w`/`previousSample.w`) instead of the old direct-procedural
  `CalculateFlutterWave()` frequency-scaling, when `EnableAmbientGrassWind` is set;
  `RunGrass.hlsl`'s `ApplyGrassWindResponse` gains a `GrassWind::CalculateFlutterDisplacement`
  branch (gated on `Permutation::EnableGrassWindSpringBend`/`EnableAmbientGrassWind`) turning the
  flutter scalar into a per-vertex displacement via a bend-axis-perpendicular direction, replacing
  the always-vanilla-style flutter when ambient wind is active. New tunables land in `WindSettings`/
  `FieldData`: `GrassWindSpring::EvaluateFlutterAmplitudeMultiplier`/`FlutterFrequency`/
  `TransientFlutterStrength`/`FlutterAmplitudeResponse`. **If Wind's deferred spring-field/flutter
  gap is picked up, this commit is now the reference implementation to port from** rather than
  designing the missing compute-texture-binding + flutter-consumption logic from scratch. No
  movement found on tree transient impulses (`TreeWindSpring::TrySample*Transient` still stubbed
  upstream in this range), the `GRASS_OPTIMIZATIONS` path, or Grass Collision interaction — those
  deferred items are unchanged.
- **`6150e8d39e`** "fix(wind): preserve shared compute buffers" (#726, 2026-09-20) — possible latent
  bug, needs a direct check against Personal's code (not yet verified either way). Upstream's
  `WindGrass.cpp`/`WindTrees.cpp` were unbinding constant-buffer slot 5
  (`context->CSSetConstantBuffers(5, 1, &nullBuffer)`) after grass/tree wind-spring compute
  dispatch, which the commit message says clobbers a **shared** b5 buffer needed by subsequent
  compute passes; fix drops that unbind call (keeps only slot 0's unbind). Also touches
  `GrassOptimizations.cpp` (shrinks a null-buffer unbind array from 4 to 2, same "shared b5/b6 must
  stay bound" rationale) — `GrassOptimizations.cpp` is outside Wind's tracked port-path list, so
  check separately whether Personal has this file and the same pattern. Personal's
  `WindGrass.cpp`/`WindTrees.cpp` were ported from this exact upstream lineage and plausibly carry
  the identical unbind-slot-5 line — worth a direct grep-and-check; if present, it's a one-line fix
  with a hard-to-notice GPU-state-corruption failure mode on subsequent draws. Moderate urgency.

### Pseudo Sun Bounce  (`jiayev/skyrim-community-shaders@compendium-clean`)

**Upstream source anchor**: `b8f93c39028a8d82ae7a9c09e76f1ad32c169765` (audited 2026-09-17, current head).

**Upstream source paths**

- `src/Features/PseudoSunBounce.cpp`, `src/Features/PseudoSunBounce.h`
- `features/Pseudo Sun Bounce/Shaders/Features/PseudoSunBounce.ini`
- `features/Pseudo Sun Bounce/Shaders/PseudoSunBounce/sunbounce.hlsli` — pure SH math, ported
  unmodified; this repo's `Common/Spherical Harmonics/SphericalHarmonics.hlsli` already has every
  function it calls (`Zero`/`Add`/`Scale`/`EvaluateCosineLobe`/`HanningConvolution`/`Unproject`/
  `FauxSpecularLobe`/`FuncProductIntegral`) with matching signatures.

**Shared-file injection points**

| File | Region |
| --- | --- |
| `package/Shaders/Common/SharedData.hlsli` | `PseudoSunBounceSettings` struct + `pseudoSunBounceSettings` field, appended to the end of `FeatureData : register(b6)` |
| `src/FeatureBuffer.cpp` | `globals::features::pseudoSunBounce.settings` appended as the last `_GetFeatureBufferData` argument |
| `src/Globals.h/.cpp`, `src/Feature.cpp` | feature registration |
| `package/Shaders/Lighting.hlsl` | `#include "PseudoSunBounce/sunbounce.hlsli"` under `#if defined(PSEUDO_SUN_BOUNCE)` near the other feature includes (~L945, by `EXP_HEIGHT_FOG`); diffuse-bounce block right after the `IBL` contribution, before `reflectionDiffuseColor = diffuseColor + directionalAmbientColor` (~L2976) — this repo's ambient-composition code at that exact point matches upstream's closely enough (same `directionalAmbientColor`/`ambientNormal`/`skylightingDiffuse`/`MultiBounceAO` names) to drop the block in directly |
| `src/Features/PseudoSunBounce.cpp` | added `#include "Utils/Serialize.h"` — needed for this repo's `float3`↔JSON `to_json`/`from_json` overloads (declared in `nlohmann::`) to be visible to the `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` macro; upstream's own build resolves this differently and didn't need the explicit include |

**Capability status — Adapted port, intentionally scoped down.** The diffuse bounce term (ground/
wall SH lobes → `directionalAmbientColor`) is fully wired and verified (`fxc /D PSEUDO_SUN_BOUNCE=1`,
alone and with `SKYLIGHTING=1`). **Deliberately not ported**: upstream also computes a "faux
specular bounce" term (`SunBounce::CalcFauxSpecularBounce`) consumed at two further points deep in
the forward/deferred specular composition code (`indirectLobeWeights.specular` blocks, one per
output path). That needs closer comparison against this repo's specular composition before it can
be safely inserted — porting it speculatively risked getting two more delicate insertion points
wrong for a secondary visual refinement. `sunbounce.hlsli` still declares
`CalcFauxSpecularBounce`; it's just uncalled from this repo's `Lighting.hlsl`. Not tested in-game.
No cloud-shadow gating either (upstream's `CLOUD_SHADOWS`-conditional multiply on the bounce
color) — this repo's `Lighting.hlsl` doesn't expose a `CloudShadows::` hook at the insertion point,
so the bounce always uses the plain directional light color.

**2026-09-25 review (jiayev): no movement.** Zero commits in `b8f93c390..jiayev/compendium-clean`
touch `src/Features/PseudoSunBounce.cpp/h` or `features/Pseudo Sun Bounce/**` — the diffuse bounce
term Personal has is unchanged; the deliberately-skipped specular/cloud-shadow gaps remain as
described above with no new upstream movement to reconsider.

### Procedural Sun  (`alandtse/open-shaders@dev`)

**Upstream source anchor**: `95bacd8211fec29a46ada67a65c237a02bf219c9` (audited 2026-09-17, current
head), introduced at `470f847247223a7a2db99aff2253c261f0bd7f4c` (feat: procedural sun, #678).

**Upstream source paths**

- `src/Features/ProceduralSun.cpp`, `src/Features/ProceduralSun.h`
- `features/Procedural Sun/Shaders/Features/ProceduralSun.ini`,
  `features/Procedural Sun/Shaders/ProceduralSun/ProceduralSun.hlsli` (pure math, no dependencies
  beyond intrinsics — ported unmodified), `.../LICENSE`
- `package/Shaders/Tests/TestProceduralSun.hlsl` — copied unmodified, **not independently
  force-compiled** (uses absolute include paths for a test harness, not standalone `fxc`)

**Shared-file injection points**

| File | Region |
| --- | --- |
| `package/Shaders/Common/SharedData.hlsli` | `ProceduralSunSettings` struct + `proceduralSunSettings` field, appended to `FeatureData : register(b6)` |
| `src/FeatureBuffer.cpp` | `globals::features::proceduralSun.GetCommonBufferData()` appended as the last `_GetFeatureBufferData` argument |
| `src/Globals.h/.cpp`, `src/Feature.cpp` | feature registration |
| `package/Shaders/Sky.hlsl` | `#include "ProceduralSun/ProceduralSun.hlsli"` under `#if defined(PROCEDURAL_SUN)`, by the `HDRSun.hlsli` include; coexistence block inserted right before the **pre-existing** `#if defined(TEX) && defined(EFFECTS11)` block that calls this repo's own simpler `ComputeProceduralSun()` (screen-space disk+corona, driven by `SharedData::enbSettings.EnableProceduralSun`) |

**Coexistence with this repo's existing Effects11 procedural sun — do not remove either path.**
Personal already had a simpler, screen-space `ComputeProceduralSun()` owned by Effects11
(`enbSettings.EnableProceduralSun`); this port adds the angular, limb-darkened standalone version
alongside it, not instead of it. Ownership is resolved exactly as upstream designed it (ported
verbatim, not reimplemented): a local `effects11OwnsSun = SharedData::enbSettings
.EnableProceduralSun != 0` gates the new standalone block off when Effects11's own toggle is on;
Effects11's own block (unconditional on its own setting) then runs immediately after and
overwrites `baseColor` again, so Effects11 always wins when both are enabled, and the standalone
feature's own `enabled` setting only takes effect when Effects11 isn't using its own. No
double-render, no ambiguity, verified across all four `PROCEDURAL_SUN`/`EFFECTS11` define
combinations plus the `CLOUD_SHADOWS`-gated occlusion refinement.

**Capability status — Present.** Verified: C++ build clean; `Sky.hlsl` pixel shader force-compiled
with `fxc` across the default case, Effects11-only, standalone-only, both-coexisting, and
`CLOUD_SHADOWS`. Not tested in-game.

**Correction to prior tracking-document language**: earlier notes described Procedural Sun as
"Partial overlap" (Effects11's simpler version only). That is superseded — Personal now has both
the Effects11 screen-space version and the standalone angular version, with deterministic
ownership between them.

**2026-09-25 review — active bug-fix cluster, re-sync warranted.** Upstream had a burst of
`Sky.hlsl`/sun-related fixes on 2026-09-21, all from the same author within ~3 hours of each other
— recommend re-syncing as one unit, in this chronological order, since they're sequential edits to
the same function:

1. **`1c0d36c350`** "fix(sky): restore vanilla sun glare" (#733) — one-line: the depth-occlusion
   `#else` branch (occludes the sun disc by scene depth) narrows to
   `#elif !defined(DITHER) || !defined(TEX)`. Minor alone, in the same `Sky.hlsl` region as
   Procedural Sun's rendering.
2. **`f537f4b9cf`** "fix(sun): prevent procedural sun clipping" (#735) — direct Procedural Sun fix,
   touches `ProceduralSun.hlsli`/`Sky.hlsl`/`ProceduralSun.cpp/h`/`SharedData.hlsli`, adds
   `TestProceduralSun.hlsl` coverage. Adds `GetOcclusionBillboardScale`/`GetBillboardScale`/
   `ResizeBillboardVertex` to rescale the sun billboard mesh based on a new `sunQuadModelRadius`
   field (read from `sky->sun->sunBase->GetModelData().modelBound.radius` in
   `ProceduralSun::GetCommonBufferData()`) vs. the engine's fixed glare-visibility-sample assumption
   (`VANILLA_SUN_BASE_HALF_WIDTH = 425.0f`), stopping the sun disc from clipping; also refactors
   `IsProceduralSunActive()` into a shared helper called from both VS and PS. Real, non-trivial
   correctness fix — visible clipping artifact, moderate urgency.
3. **`a0eafffe21`** "fix(ll): correct sky composition" (#738) — see the Cloud Relight section
   above; substantial `Sky.hlsl` composition rework (`ComposeSkyColor()`/`composeAuthoredSky`) that
   sets `composeAuthoredSky = false` in the procedural-sun and HDR-sun branches specifically.
   **Needs careful diffing, not mechanical cherry-pick**, since it touches the exact composition
   logic both Cloud Relight and Procedural Sun hook into.

Out-of-scope note: `47f5e45630` "feat(utility): expand atmosphere controls" (#741) also touches
`Sky.hlsl`/`SharedData.hlsli` in the same area (CS Utility's own `cloudBrightness`/
`cloudSaturation`/`sunGlareIntensity` fields) — CS Utility itself remains out of scope for this
repo, but it will textually interleave with any `Sky.hlsl` re-sync of the three commits above.
**Still outstanding** — the 2026-09-25 implementation pass below resolved a different Procedural
Sun question (Bottle's competing implementation) and did not touch this bug-fix cluster; `Sky.hlsl`
still needs the `1c0d36c350`/`f537f4b9cf`/`a0eafffe21` re-sync described above.

**2026-09-25 implementation pass — Bottle's competing "Physical Sun" implementation, resolved by
merging its one genuine improvement rather than replacing this feature.**

Bottle carries its **own, separately-lineaged** copy of a feature also named "Procedural Sun"
(`26611dd183` "feat: readd physical sun w/ e11 adaptation"), re-added after Bottle had dropped it
earlier. It occupies the exact same feature slot as this one (`GetName()`/`GetShortName()` are both
`"Procedural Sun"`/`"ProceduralSun"` on both branches) but the two lineages have diverged:

- **What Bottle's version has that this one lacked**: `excludeFromAdaptation` — a setting that masks
  the sun disc out of the image Effects11's eye-adaptation (auto-exposure) pass measures, via a new
  `ENBAdaptation::MaskProceduralSun()` screen-space pass (`AdaptationSunMaskPS.hlsl`) that resamples
  the surrounding sky in the sun's angular position instead of the sun itself. This is the "links to
  Effects11" integration.
- **What this repo's version has that Bottle's lacks**: the `f537f4b9cf` anti-clipping fix ported
  from `open-shaders` on 2026-09-17 (`sunQuadModelRadius`, `GetOcclusionBillboardScale`/
  `GetBillboardScale`/`ResizeBillboardVertex`) — Bottle's `26611dd183` predates that fix and doesn't
  have it.
- **Critically, Bottle's commit also deletes Effects11's own native screen-space procedural sun**
  (`ComputeProceduralSun()` in `Sky.hlsl`, driven by the ENB-preset `EnableProceduralSun`/`Size`/
  `EdgeSoftness`/`GlowIntensity`/`GlowCurve` settings) and makes its own standalone feature the sole
  sun-rendering path, with no coexistence arbitration at all. This repo's existing coexistence
  design (`effects11OwnsSun`, ported from `open-shaders`, documented above) was built specifically
  to avoid that outcome and is called out by name in this doc and in the original implementation
  brief as something **"do not remove."** A literal adoption of Bottle's patch — the
  literal reading of "strip ours out in favor of Bottle's" — would silently delete that
  ENB-preset-driven capability, i.e. it breaks another feature, not just this one.

**Resolution**: kept this repo's existing standalone Procedural Sun (open-shaders lineage, anti-clip
fix intact) and Effects11's native `ComputeProceduralSun()` coexistence exactly as they were, and
merged only Bottle's purely-additive value — the adaptation-exclusion capability — onto this
feature: added `excludeFromAdaptation` to `ProceduralSun::Settings`/JSON/UI (checkbox + tooltip,
default on, matching Bottle), and ported `ENBAdaptation`'s `MaskProceduralSun()`/
`EnsureSunMaskResources()`/`ClearShaderCache()` plus `AdaptationSunMaskPS.hlsl` verbatim — this part
is self-contained (its own cbuffer, no `SharedData.hlsli` dependency) and reads
`globals::features::proceduralSun.settings.{enabled,excludeFromAdaptation,sunDiskAngularRadius}`
directly rather than going through the GPU-side settings struct. `Effects11::ClearShaderCache()` now
also calls `enbAdaptation.ClearShaderCache()` to release the mask pixel shader. This gets the
Effects11 integration the "likely preferred" framing was after, without the destructive side effect.
Verified: `BuildDevFast` clean; `AdaptationSunMaskPS.hlsl` force-compiled standalone with `fxc`.
**Flagged as a conflict, not auto-resolved**: if the literal Bottle behavior (no Effects11-native
procedural sun at all) is actually wanted, that's a separate, deliberate call — say so and it's a
small removal from here.

**2026-09-25, second pass — full cloud-extinction re-sync (`31f9890f27`/`05c07ee292`/`343bf8cc44`/
`3d64571711`), resolving the bug-fix cluster noted above at the same time.** Bottle moved
substantially past `26611dd183` since the first pass. `cloudOcclusionStrength` (plain power-based
cloud fade) replaced with `cloudExtinction`: an optical-depth model shared between the sun disc's
own cloud fade (`GetGlareCloudTransmission`, `sqrt(occlusion)` before the power curve) and a new
`CLOUDS`+`DEFERRED` pass that darkens/backlights actual cloud pixels near the sun
(`EvaluateCloudExtinction`/`ApplyCloudExtinction`), using `sunVisibility` (from the sun mesh's own
`kBlendColor.alpha`, so it fades correctly at the horizon/behind terrain) and `radianceLimit` (4096
for HDR-capable formats, 1 otherwise) to additive-blend (`ToAdditiveBlend`) correctly against the
target's dynamic range. `ProceduralSun::PerFrameData` grew from 3 to 4 vec4s (still respecting the
16-byte-aligned append convention) to carry `sunVisibility`/`radianceLimit` alongside the kept
`sunQuadModelRadius`. New `GetSunVisibility()`/`GetMainTargetRadianceLimit()` static methods added.
This also resolves the `1c0d36c350`/`a0eafffe21` bug-fix-cluster note above as far as Procedural
Sun's own rendering path is concerned — Bottle's cloud-extinction rework superseded that code
entirely; the composition-arbitration parts of that cluster remain a Cloud Relight/`ComposeSkyColor`
concern, not a Procedural Sun one. One HLSL wrinkle: the new `GetSunLuminance()` helper calls
`Color::Sky()`, which is PSHADER/CSHADER-only in `Color.hlsli` — since `ProceduralSun.hlsli` is
included at file scope in `Sky.hlsl` (both VS and PS need the billboard-resize functions), had to
wrap just that one function in the same `#if defined(PSHADER) || defined(CSHADER) ||
defined(COMPUTESHADER)` guard `Color.hlsli` itself uses, or the VS compile fails with an undeclared
identifier. Verified: `BuildDevFast` clean; `fxc`-compiled the VS (`TEX`, `OCCLUSION`) and PS
(disc-only, `CLOUDS`+cloud-extinction) permutations. Not tested in-game.

### Sky Scattering (Effects11)  (`InTheBottle/skyrim-community-shaders@Bottle-Compendium`)

**Ported 2026-09-25**, source commit `0657cf0ab4` "feat: rework sky scattering for effects 11".
New physically-motivated atmospheric scattering model shared by two existing Effects11 systems:

- **New file** `features/Effects11/Shaders/Effects11/SkyScattering.hlsli` (237 lines, copied
  verbatim) — Henyey-Greenstein phase functions, exponential optical depth against an analytic
  spherical cloud-layer intersection, single/multiple-scattering approximations. Depends only on
  `Common/Game.hlsli` (`GAME_UNIT_TO_M`), `Common/Random.hlsli`, `Common/SharedData.hlsli`, and
  optionally `CloudShadows/CloudShadows.hlsli` — all already present, all function/constant names
  matched on the local copy, ported unmodified.
- **Volumetric rays pipeline reused, not duplicated**: `RaymarchVolumetricRaysPS`/
  `ApplyVolumetricRaysPS` gained a second half-res render target (`skyTexA`/`skyTexB`, mirroring
  `vlTexA`/`vlTexB`) and are both byte-identical to Bottle's pre-commit baseline, so Bottle's diff
  applied verbatim. The blur passes (horizontal/vertical) are now a shared lambda dispatched once
  per effect that's actually enabled (`volumetricRays`/`skyScattering` bools), rather than
  unconditional — either can run without the other.
- **Cloud relighting in `Sky.hlsl`**: `SkyScattering::RelightCloud()` inserted into the existing
  `CLOUDS`+`EFFECTS11` block, right before the pre-existing `CloudsEdgeIntensity` moon/sun-phase
  glow — computes per-pixel sun/moon lighting and cloud-shadow self-occlusion for actual cloud
  color, optionally feeding `edgeTransmittance` back into the existing edge-glow term via
  `CalculateCloudsEdgeFromScattering`. This block only touches `cloudColor`/`edgeTransmittance`, not
  the same call sites `CloudRelight` (`CR_CLOUDS`) or Procedural Sun's own new cloud-extinction
  block (both landed the same day, see above) touch — the three coexist without literal collision,
  but a user enabling both `CloudRelight` and Effects11 Sky Scattering will get cloud color relit by
  two independent systems; not resolved, same category of interaction as the pre-existing
  `CloudsEdgeIntensity` effect.
- **`Effects11::PerFrame` cbuffer** (`src/Features/Effects11.h`) — Bottle's own struct has diverged
  further from this fork's (Bottle has Water fields this fork never ported; see the `ENBDepthOfField`
  entry above for the "DOF and water readd" split), so Bottle's diff didn't apply verbatim here. The
  20 new fields (`EnableCloudsScattering` through `CloudsLightingDensity`) were appended after
  `VolumetricRaysColorFilter` instead — that field ends the struct on a 16-byte boundary already, and
  the new fields form the same 5×vec4 grouping Bottle uses, so the layout is equivalent even though
  the byte offsets differ from Bottle's. Mirrored identically in `SharedData.hlsli`'s `ENBSettings`.
  `additiveBlendState` renamed `scatteringBlendState` (`DestBlend` changed `D3D11_BLEND_ONE` →
  `D3D11_BLEND_SRC_ALPHA` so the sky glow's alpha, carried from cloud-shadow occlusion, actually
  attenuates the blend).
- **`EffectManager::RegisterSettings()`** — new `SKYSCATTERING` category (14 settings, all
  time-of-day-interpolated except two bools), `EnableCloudsScattering` in `EFFECT`,
  `SetCategoryDependency`/`SetCategoryExteriorOnly` wired the same way `CLOUDSHADOWS` already is.
  Pure ENB-preset-driven data (not this repo's own JSON settings), so no `.ini` version bump and no
  i18n keys — consistent with every other `ENB*`/Effects11 setting.
- **`CloudShadows.hlsli`** — needs an include guard (`SkyScattering.hlsli` includes it too); this
  repo's copy already had one from unrelated earlier work, so no change needed there.

Verified: `BuildDevFast` clean; `fxc`-force-compiled `Sky.hlsl`
(`EFFECTS11`+`CLOUDS`+`DEFERRED`+`CLOUD_SHADOWS`+`PROCEDURAL_SUN`+`TEX`), `ApplyVolumetricRaysPS.hlsl`,
and `RaymarchVolumetricRaysPS.hlsl` (with `CLOUD_SHADOWS`+`TERRAIN_SHADOWS`). Not tested in-game —
this is a real new rendering pass (extra render targets, extra blur dispatches) that needs an actual
frame captured with an ENB preset that turns `EnableCloudsScattering` on to confirm it looks right
and doesn't regress volumetric-rays-only performance.

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

## 7. New upstream features observed, not currently ported or watched (reviewed 2026-09-17, updated 2026-09-25)

Survey of everything each fork shipped since the last baseline, beyond the already-ported feature
surface. **Scope note (2026-09-25):** per explicit instruction, this pass only did a from-scratch
new-feature survey for `InTheBottle/skyrim-community-shaders` (Bottle) — its subsection below is
fully refreshed against `dac6803377`. The `open-shaders` and `jiayev` subsections are left as they
stood at the 2026-09-17 review; those two forks were this pass only checked for movement on
already-ported surface (see §3's per-feature "2026-09-25 review" notes), not re-surveyed for brand
new features. None of this section's content has been ported — recorded here so a future re-sync
pass can decide whether any of it is worth adopting. Nothing in this section changes what §1a's
`git log` path filters need to cover unless a port is actually taken from it later.

### `alandtse/open-shaders@dev` (`7ae52a5543` → `95bacd821`, not re-surveyed 2026-09-25 — see scope note above)

- **`features/Wind/`** (#634) and **Procedural sun** (#678) — both now ported; see their entries
  under [3. Per-feature watch lists](#3-per-feature-watch-lists) above (**Partial port** and
  **Present** respectively) instead of this list.
- **Scene Manager** (#589) and an **"OS Menu" editor tab** (#674) — new UI/workflow surface, not
  overlapping any of our ported features' code paths. Scene Manager is tracked as a "Larger feature
  ports" item, not yet started as of this writing.
- **linear lighting rework** (#666, `feat: linear lighting rework`)
  — touches `linearLightingSettings`, which `CloudRelight.hlsli` already reads
  (`SharedData::linearLightingSettings.enableLinearLighting` etc.); worth a diff against our
  Linear Lighting feature next time Cloud Relight is re-synced, in case the field set moved.
  Cloud Relight's own math changes from this range are captured in `105a90cea` above.
- **PBR grass** (#2709) and **terrain-variation mesh support** (#2703) — mainline-CS features that
  reached open-shaders via its `chore(sync)` commits, not specific to any of our ports.
  **2026-09-17 — corrected capability record**: both are **present by code** in Personal already
  (`RunGrass.hlsl` has `#if defined(GRASS_LIGHTING) && defined(TRUE_PBR)` blocks; `TerrainVariation.cpp`
  has the `EnableMeshSupport` setting, UI checkbox and mesh-texture-cache path), independently of
  this fork — a prior pass over this document had left them out of the capability ledger entirely,
  which read as "not present." Not something this queue ported; recorded here only to correct that
  omission.
  `fix(grass): fix renderdoc crash with grass-opt enabled` (#2706) may be worth a look given we
  carry our own grass-PS duplication (§2) but wasn't investigated this pass.
- VR-specific work (dynamic near clip #615, native-menu VR awareness, instance-culling fixes) —
  out of scope, this fork's Skyrim VR support doesn't apply here unless VR is added later.
- `refactor(math): centralize ClampFinite` (#638), `refactor(ui): adopt CheckboxFlag across
  features` (#639), `feat(feature): add generic per-render-pass hook` (#654) — general
  infrastructure refactors, not evaluated for adoption.

### `InTheBottle/skyrim-community-shaders@Bottle-Compendium` (`7c58cb1ee` → `dac6803377`, full feature audit 2026-09-25)

**Character Rain parity task — closed, moot.** Bottle **reverted its own** character-wetness
implementation: `3afd2b6277` "revert: character wetness" deletes `CharacterRainSurfaces.cpp/h`,
`CharacterRainLighting.hlsli`, `CharacterRainSpots.hlsli` entirely and strips the associated
`WetnessEffects.cpp/h`/`GrassCollision.cpp`/`State.h`/`ActorUtils.*` hooks (net -1231/+38 lines).
Confirmed: `git ls-tree -r bottle/Bottle-Compendium | grep -i characterrain` returns nothing at
current head. There is no longer an upstream Bottle implementation to line-by-line-diff Personal's
own Character Rain against — the previously-open "still needs a parity recheck" item is closed;
only worth revisiting if Bottle reintroduces the feature later.

**`snow-rework` branch — retired, see §1a.** It's an ancestor of current `Bottle-Compendium`, not a
pending rewrite; nothing further to watch.

**Two large ports landed 2026-09-23 without a tracking-doc update — now recorded in §3/top table:**
Reverse Z (`124bc7e22`/`1698b4beb`) and Footstep Particles (`124bc7e22`). See their dedicated §3
sections for residual follow-up gaps.

**Light Limit Fix — significant further movement, re-sync now owed.** See the Light Limit Fix
section in §3 for the full detail (`d05a80bb00`, `dac6803377`, `6ae45a32fa`, `a0ac9312e8`) — this
supersedes the 2026-09-17 "Present" record and is the highest-priority item from this review.

**Genuinely new, unported Bottle subsystems found this pass** (none have any local counterpart;
ranked by rough size):

- **Light Limit Fix contact-shadow/scheduling rework** — **closed 2026-09-25 (second pass)**. The
  contact-shadow half was already ported; the scheduling half turned out to need a *revert* rather
  than a port forward — see the "second pass" note in the Light Limit Fix section in §3.
- **Sky Scattering rework for Effects11** (`0657cf0ab4`) — **ported 2026-09-25 (second pass)**; see
  the Sky Scattering section in §3. Not tested in-game.
- **Physical Sun / Effects11 sun-adaptation integration** (`26611dd183` "feat: readd physical sun w
  e11 adaptation") — the disambiguation this item called for is done; see the Procedural Sun section
  in §3 for the resolution (the `excludeFromAdaptation`/`AdaptationSunMaskPS.hlsl` piece was merged
  onto Personal's existing standalone Procedural Sun; Bottle's *removal* of Effects11's native
  `ComputeProceduralSun()` was **not** carried over — flagged as a conflict, not auto-applied).
- **`ENBDepthOfField`** (`src/Features/Effects11/Effects/ENBDepthOfField.cpp/h`) — **ported
  2026-09-25**. Small, self-contained `EffectBase` subclass (aperture/focus-time IDs, five 1×1/16×16
  SRVs for the aperture/focus ping-pong state, `enbdepthoffield.fx` resolved at runtime like every
  other ENB effect — no `.fx` file ships with this repo or with Bottle; it comes from the user's own
  ENB preset). Source commit (`2a39ad34e9` "feat: DOF and water readd") bundles an unrelated water
  settings/color feature in the same `Effects11.cpp/h`/`EffectManager.cpp` hunks — confirmed the two
  are cleanly separable (the water half touches only `Effects11::PerFrame`/`GetCommonBufferData()`/
  `OverrideWeather()` and `Water.hlsl`/`WaterParallax.hlsli`, none of which DOF's wiring needs) and
  left the water half out, out of scope for this pass. Ported: the new `ENBDepthOfField.cpp/h` files
  verbatim; `EffectManager.h`'s `enbDepthOfField` member, its `useDepthOfField` setting ID, and its
  slot in every `allEffects[]` enumeration (`GetFailedEffectCount`/`GetAllErrors`/`Apply`/`Load`/
  `RenderEffectsList` — 5 call sites); `EffectManager.cpp`'s `enbDepthOfField.Apply()`/`.Save()`
  calls, the `EnableDepthOfField`/`FocusingTime`/`ApertureTime` setting registrations, and
  `ExecuteEffect(enbDepthOfField, ids.useDepthOfField)` placed first in `ExecuteEffects()` (before
  `UpdateDownsampledTexture`, matching Bottle's ordering — DOF runs before the rest of the ENB chain
  samples the frame). No settings-schema changes to any feature Personal already owns; no i18n keys
  (ENB effect settings aren't routed through `T()`, matching every other `ENB*` effect here).
  Verified: `BuildDevFast` clean, no new warnings. Not tested in-game (needs an ENB preset that ships
  an `enbdepthoffield.fx` to exercise at all — the feature is inert without one, same as every other
  `ENB*` effect wrapper).
- **FSR4/FSR3 runtime-upscaler split** (`src/Features/Upscaling/FidelityFX/RuntimeUpscaler.cpp`,
  new file, references `../../ReverseZ.h`) — **still not ported**, deliberately. Investigated the
  source commit (`9ea5d7e64a` "feat: FSR4 support from OS"): 2953 insertions across a new
  1842-line `RuntimeUpscaler.cpp`, 222/215 lines of `FidelityFX.cpp/h` changes, 119/22 lines of
  `Upscaling.cpp/h`, new `cmake/FidelityFX-Runtime.cmake`/`cmake/FeaturePackaging.cmake` build-system
  files, a new vendored `include/FidelityFX/upscalers/` SDK header set, and a shipped-DLL swap
  (`amd_fidelityfx_framegeneration_dx12.dll`/`amd_fidelityfx_loader_dx12.dll` removed in favor of the
  new headers). This is a full FSR4 SDK integration, not a code-level port — multi-session scope of
  its own (build-system changes, new binary dependencies, needs an actual FSR4-capable GPU to verify
  at all). Recorded here with full scope so a future dedicated pass doesn't have to re-derive it.
  **2026-09-25, second pass:** re-investigated as part of an explicit 5-item Bottle-alignment
  request and quantified exactly why this stays deferred — diffed Personal's current
  `Upscaling.cpp`/`.h`/`FidelityFX.cpp`/`.h`/`DX12SwapChain.cpp`/`.h`/`CMakeLists.txt` against
  `9ea5d7e64a^` (Bottle's tree immediately before the FSR4 commit): `FidelityFX.cpp`, `FidelityFX.h`,
  and `DX12SwapChain.h` are still **byte-identical** to that baseline (Bottle's diff for those would
  apply cleanly), `DX12SwapChain.cpp` differs by only 2 lines, and `CMakeLists.txt` by 21 — but
  `Upscaling.cpp`/`.h` (the files Bottle's own FSR4 diff actually touches) have diverged by ~1700
  lines combined from this fork's independent upscaler work. Presented this scope breakdown to the
  user with three options (proceed / defer / port only the already-clean files); **user chose to
  defer**. If picked up later, start from `FidelityFX.cpp/h`/`DX12SwapChain.h` (free) and budget the
  real effort for reconciling `Upscaling.cpp/h` by hand plus in-game verification on FSR4-capable
  hardware, which this environment does not have.
- **Cloud self-shadowing** (`35e2151ab9` "feat: cloud self shadowing and cloud improvements") — low
  priority, uncertain net upstream state: Bottle **partially reverted this itself** two commits
  later (`6660d25a1b` "revert" removes the 60-line `CloudShadows.hlsli` self-shadow addition and
  `Sky.hlsl`'s +18 lines, but leaves some `CloudShadows.cpp/h` changes in place). Not evaluated
  further given the self-revert; re-check net state if picking this up later.
- **`b4e7b08ec4`** "feat(menu): sortable profiler timing tables" (#2759) — UI-only,
  `PerformanceOverlay.cpp`/`Menu/ProfilingRenderer.cpp`/`Utils/UI.cpp/h`. **Not Bottle-original** —
  this is a mainline-CS change Bottle absorbed via `e8ac64db3c` "chore: merge upstream/dev
  (post-1.9.0 fixes)"; check mainline sync status (§1b) rather than treating as a Bottle pickup.
- Skimmed titles only, not diffed: `09384077cf` "feat: height fog native for e11\vanilla",
  `283b33399b` "e11 fixes", `1e1da45856` "disable post processing default" — small-to-unknown size,
  Effects11/PostProcessing-adjacent.

**Other notable upstream changes (not feature-scoped, worth flagging):**

- **`44826d2805`** "fix(terrain-shadows): stabilize penumbrae" (#2729) — mainline-CS fix (reached
  Bottle via its own `chore: merge main into dev`), touches `TerrainShadows.hlsli`/
  `ShadowUpdate.cs.hlsl`/`SharedData.hlsli`/`TerrainShadows.cpp/h`. Confirmed local
  `TerrainShadows.hlsli` diverges 24 lines from Bottle head — Terrain Shadows isn't a Bottle-sourced
  port in Personal, so this is informational only (check mainline sync status, §1b), but it's a
  `SharedData.hlsli`-touching fix worth knowing about.
- **`25161eb4f3`** "fix(shaders): compile skin and hair under TRUE_PBR" (#2738) also appears on
  Bottle's history via its own upstream-merge commits — same fix already documented as ported
  (`2da0eb7a5`, "shared ancestor commit") and confirmed fully present (see Advanced Skin, §3). No
  new action.
- **Reverse Z (`6db6512c96`)** is the single largest hot-file-touching change in the whole audited
  range and is already ported (§3) — flagged here only as context: most apparent "unrelated churn"
  in `Lighting.hlsl`/`SharedData.hlsli` on any future Bottle diff in this window is Reverse Z, not a
  separate feature.
- The 1.9.0 release boundary (`5db085e779`/`a6e649f81a`) sits right where the Sept-23 local port
  stopped — everything before it is very likely captured, everything from `6ae45a32fa` onward
  (~30 commits through `dac6803377`) is the residual surface this review found.

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

### `jiayev/skyrim-community-shaders@compendium-clean` (`72041475c8` → `b8f93c390`, not re-surveyed 2026-09-25 — see scope note above)

No movement on the ported Post Processing or Advanced Skin paths (§1a `git log` filter came back
empty). Everything in this range is new/unported surface:

- **2026-09-17:** ported **Dynamic Cubemaps lighting-change detection** (`49c35c6ef`) — see the
  dedicated §3 section below for what was ported vs. deliberately left out.
- **Linear Lighting refactor** (`LinearLighting.cpp/h`) alongside the Physical Sky cloud work
  below — touches `package/Shaders/Lighting.hlsl`, `RunGrass.hlsl`, `Water.hlsl`, `Particle.hlsl`,
  `Effect.hlsl` (6 lines each), so any future port from this range should re-check those hunks
  against our own Linear Lighting state. **2026-09-25:** confirmed no further movement in this
  range (one-line check only, per this pass's scope limit — not re-investigated in depth).
- **Physical Sky cloud improvements** — `CloudMotion.hlsli` (new), substantial `CloudTemporal.hlsli`
  rework (+209/-lines), `CloudBlur.hlsli`/`CloudBoundary.hlsl`/`Volumetrics.cs.hlsl` changes,
  `VolumetricClouds.cpp` and `PhysicalSky.cpp/h` updates. **2026-09-25:** confirmed no further
  movement in this range (one-line check only, per this pass's scope limit).

None of the three forks' new work above was ported in this pass — recorded for the next re-sync
to triage, not acted on. The 2026-09-25 pass's actual ported-surface findings for `jiayev` (two
`ShaderCache.cpp` robustness fixes) are recorded under Post Processing in §3, not here — this
section only tracks brand-new/unported surface, which per that pass's scope was not re-surveyed.
