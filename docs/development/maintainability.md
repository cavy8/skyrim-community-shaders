# Maintainability Model (Personal branch)

How the Personal branch relates to the Community Shaders forks it takes code from, and where
that is recorded. Read this before changing a feature that also exists upstream.

## The model

-   **Bottle-Compendium (`InTheBottle/Bottled-Shaders`) is the primary maintenance baseline.**
    The exact commit is pinned as `bottle.sha` in [`upstreams.yaml`](./upstreams.yaml). Everything
    is compared against that commit, not against whatever Bottle's branch head is.
-   **Bottle plus three things.** Since the 2026-09-26 simplification Personal differs from the
    pin only by: the Neural Rendering feature and its seams, the Screenshot SDR fix, the "Show
    Background Compile Overlay" toggle, and Personal's own docs, tooling and `BuildPersonal.bat`.
    One owner-approved exception sits on top: Upscaling fix F1 (Streamline is told DLSS motion
    vectors are pre-dilated, which Bottle's `EncodeTexturesCS` already does).
    Everything else that used to differ (Wind, Cloud Relight, Pseudo Sun Bounce, Advanced Skin,
    Effects11 preset hot-swap, default states, branding, ...) was removed and is listed in
    [`removed-features.md`](./removed-features.md) for later re-adding.
-   **Provenance is independent of the baseline.** A feature can be maintained against Bottle and
    still have started in Open Shaders, Jiayev's fork, or mainline. Pinning a baseline says
    nothing about who wrote the code.
-   **Personal-original work is small and recorded.** Neural Rendering (with its hotkeys and the
    Screenshot comparison capture), the Screenshot sRGB fix, and the "Show Background Compile
    Overlay" setting. Each is marked `personal_original: true` in `feature-provenance.yaml`.
-   **Source comparison is authoritative.** When a record here disagrees with the code, the code
    wins and the record is fixed. Never infer provenance from a path, a commit author, a feature
    name, or a generic `feat:` subject.

## Where things are recorded

| File | Answers |
| --- | --- |
| [`upstreams.yaml`](./upstreams.yaml) | Which repositories exist, their remotes, and the pinned Bottle SHA. |
| [`feature-provenance.yaml`](./feature-provenance.yaml) | Where each feature (or component of one, or core piece) came from, with confidence and evidence commits. |
| [`maintenance-policy.yaml`](./maintenance-policy.yaml) | The complete catalogue of deviations from Bottle: per-feature policies, retained components and seams, shared-file hunks, and whole repository files. |
| [`removed-features.md`](./removed-features.md) | What the 2026-09-26 simplification removed, and the commit to restore it from. |

The two feature files never carry each other's fields: provenance is "where from", policy is
"compare against what, and why does this differ".

### Provenance entries

Each feature has one or more `provenance` entries (`source`, `role`, `confidence`, optional
`evidence`, and a `note` when unresolved). A composite simply has several entries; `components`
is used only when parts genuinely have different sources. Non-feature code (the overlay setting,
the Neural Rendering hotkeys) is listed under `core` with the same shape. Unclear origins are
written as `confidence: unresolved` with a one-line note, never guessed.

### Policies

| Policy | Meaning |
| --- | --- |
| `bottle-plus-components` | Bottle base plus named retained components (Screenshot: the sRGB fix and NR's comparison capture). |
| `bottle-plus-seam` | Bottle base plus a minimal documented hook seam for a feature owned elsewhere. Upscaling only: it carries the Neural Rendering seam and fix F1. |
| `personal` | Personal-original implementation (Neural Rendering). |

A feature with no entry must match Bottle exactly: `bottle_sync.py` reports any differing file that
no entry claims. `bottle_sync.py` also still understands `bottle-exact`, `external-maintained`
(with `reference`/`adaptations`) and a `branding` rule, for when a removed item is re-added.

### Shared files

Files that several features touch (`Lighting.hlsl`, `RunGrass.hlsl`, `Permutation.hlsli`,
`State.h`, `Hooks.cpp`, `Deferred.cpp`, the menu and shader-cache files, among others) are listed
under `shared_integrations`. Every
hunk that differs from Bottle in such a file must contain a `match` (or `removes`) string of one of
its documented integrations, so an undocumented edit anywhere in a shared file is caught.

Append-style shared layouts follow one rule: **Bottle's entries first and verbatim, local entries
kept apart.** `State::ExtraShaderDescriptors` / `Permutation::ExtraFlags` take Bottle's
sequential low bits unchanged and allocate local bits downward from bit 31. If a local feature
ever needs `FeatureData` (`SharedData.hlsli` ↔ `FeatureBuffer.cpp`) or `PermutationCB` fields, append
them after Bottle's, each struct a 16-byte multiple. Feature registration (`Feature.cpp`, `Globals`)
lists Bottle's features in Bottle's order and appends the local ones.

## Workflow

1. Before modifying a shared feature, read its entries in both YAML files.
2. Compare the current source against the pinned Bottle SHA: `python tools/bottle_sync.py`. It exits
   non-zero on any undocumented difference: a drifting feature file, an unattributed hunk in a
   shared file, or a differing file nothing in the policy claims. `--catalogue` lists every file
   that differs from Bottle with its owner. `python tools/provenance_audit.py <Feature>` shows which upstream each
   file currently matches.
3. Preserve every documented component, seam and `shared_integrations` hunk; record new ones in
   the same change. When re-adding a removed item, remove its line from `removed-features.md`.
4. Commit with the repository's conventional types. Record where imported code came from with an
   `Upstream-Source: <upstream key>@<sha>` trailer, one commit per source when a feature combines
   several.
5. Syncing to newer Bottle work is a deliberate step: bring the code up to the new commit and bump
   `bottle.sha` in the same change.

Historical port investigations (before this model existed) are archived in
[`history/upstream-port-tracking-2026-09.md`](./history/upstream-port-tracking-2026-09.md). That
file is not authoritative.
