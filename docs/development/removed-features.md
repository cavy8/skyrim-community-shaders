# Removed Features

On 2026-09-26 the Personal branch was reset to Bottle-Compendium `db5f4dd7e4`. Only Neural
Rendering, the Screenshot SDR fix and the "Show Background Compile Overlay" toggle were kept. This
file lists what was taken out, so any of it can be added back later.

Everything below still exists at commit **`84783ba220`** (local branch `Personal-pre-simplify`).
That commit's `docs/development/maintenance-policy.yaml` lists every file and shared-file hunk each
item needed. `feature-provenance.yaml` at the same commit says where each came from.

To restore an item, check out its own files from `84783ba220`. Then re-apply its hunks in shared
files by hand: Bottle has moved on since, so a straight cherry-pick will not apply cleanly. After
that, add the item back to `maintenance-policy.yaml` and `feature-provenance.yaml`.

## Features

| Feature | Source | Own files at `84783ba220` | Shared-file hunks it needed |
| --- | --- | --- | --- |
| Wind (grass spring, tree bend, transient wind from spells/shouts/impacts) | Open (`open-shaders/dev`) | `src/Features/Wind/`, `features/Wind/` (CORE), `package/Shaders/Common/{Grass,Tree,Transient}Wind*.hlsli`, `WindField*.hlsli`, `DampedSpring.hlsli`, `package/Shaders/{Grass,Tree}WindSpringCS.hlsl`, `package/Shaders/Tests/`, `package/SKSE/Plugins/CommunityShaders/WindSettings/`, `src/Utils/LazyShader.h` | Lighting, Utility, RunGrass, DeferredCompositeCS, SharedData, Permutation (TreeBend bit 29 + PerShader fields), Math (wind epsilons), State.h (`PermutationCB`), Hooks, Deferred (t18), FeatureBuffer, Grass Optimizations culling (4 float4 extras), ActorUtils, FileSystem (`GetWindSettingsPath`) |
| Cloud Relight | Open | `src/Features/CloudRelight.*`, `features/Cloud Relight/` | Sky.hlsl (`CR_CLOUDS`), SharedData, FeatureBuffer, Math (`SafePow`), Cloud Shadows t26 self-shadow, Sky Sync celestial light weights |
| Pseudo Sun Bounce | Jiayev (`compendium-clean`) | `src/Features/PseudoSunBounce.*`, `features/Pseudo Sun Bounce/` | Lighting.hlsl (bounce + faux specular), SharedData, FeatureBuffer |
| Advanced Skin (replaced Bottle's Skin) | Jiayev | `src/Features/Skin.*`, `features/Skin/`, `package/Shaders/Skin/Overrides/README.md` | none beyond the feature's own files |

## Components of Bottle features

| Item | Source | Where |
| --- | --- | --- |
| Effects11 preset hot-swap (preset locations, switch without restart) | Open | `src/Features/Effects11/PresetManager.*`, `MenuManager.*`, and `Effects11.cpp/.h`. Personal did not have Bottle's Effects 11 Editor; Bottle's editor is back now. |
| Cloud Shadows t26 self-shadow input | Personal integration for Cloud Relight | `src/Features/CloudShadows.*`, `CloudShadows.hlsli` |
| Sky Sync celestial light weights | Open | `src/Features/SkySync.*` |
| Dialogue Depth of Field menu gate | **Personal original** (`a7e142722c`) | `DoF::GetInDialogue` in `src/Features/PostProcessing/DoF.cpp`: a `menuOpen &&` gate on `lastSpeaker`. Re-added 2026-09-26 as Post Processing P2, gating on the Dialogue Menu instead (`menuOpen` failed on leave/re-enter/leave) and also requiring an active TDM lock (`maintenance-policy.yaml`). |

## Personal default states

| Item | Commit(s) |
| --- | --- |
| Post Processing: the feature and every pipeline effect off by default, `DisableVanillaTonemapping` 0. Re-added 2026-09-26 as Post Processing P1 with the feature itself on (`maintenance-policy.yaml`). | `90e57b9855` |
| Unified Water enabled by default (`IsDisabledByDefault()` returns false) | `c5f10c0666`, `9c8c8c1ea0` |
| Advanced Skin disabled by default | `9c8c8c1ea0` |
| Snow Cover `AffectTreeTint 0` in every worldspace config (Bottle sets 1 in five of them) | `0d5c196334` |

## Branding and repository files

| Item | Where |
| --- | --- |
| Product name "Cav's Unity Shaders" instead of "Bottled Shaders" | `cmake/Plugin.h.in` `DISPLAY_NAME`, plus text in `src/`, `package/`, `CONTRIBUTING.md`, `TRANSLATING.md` (`branding` rule in `maintenance-policy.yaml`) |
| The fork's own README | `README.md` |
| Russian and Chinese translations of the removed features' strings | `package/SKSE/Plugins/CommunityShaders/Translations/{ru,zh_CN}.json` |

## Removed before this reset

- **Character Rain** (Open): removed for Bottle parity, because Bottle reverted it in `3afd2b6277`.
- **Personal-only Upscaling fixes** (`WrappedResource` names, `qualityMode` clamp, Upscaling's own
  loading-screen reset): dropped when Upscaling was reset to Bottle. Neural Rendering resets DLSS
  through seam S5 instead. Streamline `motionVectorsDilated` was restored on 2026-09-26 as
  Upscaling fix F1 (`maintenance-policy.yaml`).
