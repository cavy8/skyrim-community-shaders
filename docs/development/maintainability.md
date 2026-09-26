# Maintainability Model (Personal branch)

How the Personal branch relates to the Community Shaders forks it takes code from, and where
that is recorded. Read this before changing a feature that also exists upstream.

## The model

-   **Bottle-Compendium (`InTheBottle/Bottled-Shaders`) is the primary maintenance baseline.**
    The exact commit is pinned as `bottle.sha` in [`upstreams.yaml`](./upstreams.yaml). Everything
    is compared against that commit, not against whatever Bottle's branch head is.
-   **Prefer Bottle.** Personal differs from the pin only where it has to: the ported features Bottle
    does not carry (Wind and Cloud Relight from Open, Pseudo Sun Bounce and Advanced Skin from
    Jiayev) and the hunks they need in shared files, Neural Rendering and its seams, Personal's own
    work, branding, and Open's Effects11 preset hot-swap (kept by owner choice). Mainline or Open
    changes that no port needs are not kept.
-   **Provenance is independent of the baseline.** A feature can be maintained against Bottle and
    still have started in Open Shaders, Jiayev's fork, or mainline. Pinning a baseline says
    nothing about who wrote the code.
-   **Personal-original work is small and recorded.** The one Personal-original *feature* is Neural
    Rendering. The rest is: the dialogue Depth of Field menu gate in `DoF::GetInDialogue`, the
    Screenshot sRGB fix, the "Show Background Compile Overlay" setting, and Personal's default
    states (Post Processing off, Unified Water on, Snow Cover tree tint off, Advanced Skin off).
    Each is marked `personal_original: true` in `feature-provenance.yaml`. Code written only to make
    Open or Jiayev code run on Bottle (seams, adapters, compile fixes) is integration, not original
    work.
-   **Source comparison is authoritative.** When a record here disagrees with the code, the code
    wins and the record is fixed. Never infer provenance from a path, a commit author, a feature
    name, or a generic `feat:` subject.

## Where things are recorded

| File | Answers |
| --- | --- |
| [`upstreams.yaml`](./upstreams.yaml) | Which repositories exist, their remotes, and the pinned Bottle SHA. |
| [`feature-provenance.yaml`](./feature-provenance.yaml) | Where each feature (or component of one, or core piece) came from, with confidence and evidence commits. |
| [`maintenance-policy.yaml`](./maintenance-policy.yaml) | The complete catalogue of deviations from Bottle: per-feature policies, retained components and seams, external-feature adaptations, shared-file hunks, whole repository files, and the branding rule. |

The two feature files never carry each other's fields: provenance is "where from", policy is
"compare against what, and why does this differ".

### Provenance entries

Each feature has one or more `provenance` entries (`source`, `role`, `confidence`, optional
`evidence`, and a `note` when unresolved). A composite simply has several entries; `components`
is used only when parts genuinely have different sources. Non-feature code (the overlay setting,
branding, the Neural Rendering hotkeys) is listed under `core` with the same shape. Unclear origins
are written as `confidence: unresolved` with a one-line note, never guessed.

### Policies

| Policy | Meaning |
| --- | --- |
| `bottle-exact` | Must match the pinned Bottle SHA (after the branding rule). |
| `bottle-plus-components` | Bottle base plus named retained components (from another upstream, or Personal). |
| `bottle-plus-seam` | Bottle base plus a minimal documented hook seam for a feature owned elsewhere. Upscaling only: it carries the Neural Rendering seam. |
| `external-maintained` | Bottle does not carry this code; `reference` names the upstream it tracks, and `adaptations` lists every file that intentionally differs from that upstream. |
| `personal` | Personal-original implementation (Neural Rendering). |

### Branding

Branding is one rule, not hand edits: `branding` in the policy file substitutes Bottle's product
name, and `Plugin::DISPLAY_NAME` carries it to the code that prints it. Files that differ from
Bottle only by that substitution count as identical. Developer tooling and docs keep Bottle's text.

### Shared files

Files that several features touch (`Lighting.hlsl`, `RunGrass.hlsl`, `Utility.hlsl`,
`SharedData.hlsli`, `Permutation.hlsli`, `State.h`, `Hooks.cpp`, `Deferred.cpp`, the menu files, and
the Grass Optimizations culling seam, among others) are listed under `shared_integrations`. Every
hunk that differs from Bottle in such a file must contain a `match` (or `removes`) string of one of
its documented integrations, so an undocumented edit anywhere in a shared file is caught.

Append-style shared layouts follow one rule: **Bottle's entries first and verbatim, local entries
kept apart.** `State::ExtraShaderDescriptors` / `Permutation::ExtraFlags` take Bottle's
sequential low bits unchanged and allocate local bits downward from bit 31. The `FeatureData`
cbuffer (`SharedData.hlsli` ↔ `FeatureBuffer.cpp`) and `PermutationCB` take Bottle's layout and
append local fields at the end, each struct a 16-byte multiple. Feature registration
(`Feature.cpp`, `Globals`) lists Bottle's features in Bottle's order and appends the local ones.

## Workflow

1. Before modifying a shared feature, read its entries in both YAML files.
2. Compare the current source against the pinned Bottle SHA: `python tools/bottle_sync.py`. It exits
   non-zero on any undocumented difference: a drifting feature file, an unattributed hunk in a
   shared file, or a differing file nothing in the policy claims. `--catalogue` lists every file
   that differs from Bottle with its owner; `--verbose` also lists external features' differences
   from their own upstreams. `python tools/provenance_audit.py <Feature>` shows which upstream each
   file currently matches.
3. Preserve every documented component, seam, adaptation and `shared_integrations` hunk; record new
   ones in the same change.
4. Commit with the repository's conventional types. Record where imported code came from with an
   `Upstream-Source: <upstream key>@<sha>` trailer, one commit per source when a feature combines
   several.
5. Syncing to newer Bottle work is a deliberate step: bring the code up to the new commit and bump
   `bottle.sha` in the same change.

Historical port investigations (before this model existed) are archived in
[`history/upstream-port-tracking-2026-09.md`](./history/upstream-port-tracking-2026-09.md). That
file is not authoritative.
