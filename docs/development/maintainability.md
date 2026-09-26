# Maintainability Model (Personal branch)

How the Personal branch relates to the Community Shaders forks it takes code from, and where
that is recorded. Read this before changing a feature that also exists upstream.

## The model

-   **Bottle-Compendium (`InTheBottle/Bottled-Shaders`) is the primary maintenance baseline.**
    The exact commit is pinned as `bottle.sha` in [`upstreams.yaml`](./upstreams.yaml). Shared
    functionality is compared against that commit, not against whatever Bottle's branch head is.
-   **Provenance is independent of the baseline.** A feature can be maintained against Bottle and
    still have started in Open Shaders, Jiayev's fork, or mainline. Pinning a baseline says
    nothing about who wrote the code.
-   **Most Personal features are ports, and some are composites** of Bottle, Open, Jiayev and
    mainline work. Living in Personal, or differing from Bottle, does not make code
    Personal-original.
-   **Only two things are known to be Personal-original:** the Neural Rendering feature, and the
    dialogue Depth of Field menu gate in `DoF::GetInDialogue`.
-   **Source comparison is authoritative.** When a record here disagrees with the code, the code
    wins and the record is fixed. Never infer provenance from a path, a commit author, a feature
    name, or a generic `feat:` subject.

## Where things are recorded

| File | Answers |
| --- | --- |
| [`upstreams.yaml`](./upstreams.yaml) | Which repositories exist, their remotes, and the pinned Bottle SHA. |
| [`feature-provenance.yaml`](./feature-provenance.yaml) | Where each feature (or component of one) came from, with confidence and evidence commits. |
| [`maintenance-policy.yaml`](./maintenance-policy.yaml) | What each feature is compared against, which non-Bottle components and seams are retained, deferred decisions, and the per-hunk `shared_integrations` of hot files. |

The two feature files never carry each other's fields: provenance is "where from", policy is
"compare against what".

### Provenance entries

Each feature has one or more `provenance` entries (`source`, `role`, `confidence`, optional
`evidence`, and a `note` when unresolved). A composite simply has several entries; `components`
is used only when parts genuinely have different sources. A `design-reference` entry (the idea
came from elsewhere, the code did not) does not by itself make `personal_original` false.
Unclear origins are written as `confidence: unresolved` with a one-line note, never guessed.

### Policies

| Policy | Meaning |
| --- | --- |
| `bottle-exact` | Must match the pinned Bottle SHA. |
| `bottle-plus-components` | Bottle base plus named retained components (from another upstream, or Personal). |
| `bottle-plus-seam` | Bottle base plus a minimal documented hook seam for a feature owned elsewhere. Upscaling only: it carries the Neural Rendering seam. |
| `external-maintained` | Bottle does not contain this; the named upstream is tracked directly (Wind → Open, Pseudo Sun Bounce → Jiayev). |
| `personal` | Personal-original implementation (Neural Rendering). |

"Composite" is a provenance fact, not a policy. When no single base fits yet, pick the closest
real policy, list the components, and converge toward `bottle-plus-components`.

### Hot files

`Lighting.hlsl`, `RunGrass.hlsl`, `SharedData.hlsli`, `Permutation.hlsli`, `State.h`,
`State.cpp`, `Hooks.cpp` and `Deferred.cpp` mix hunks from several sources, so path ownership
does not work for them. Their important, conflict-prone hunks are listed under
`shared_integrations` in `maintenance-policy.yaml`, anchored to a function, define or struct
field rather than a line number.

Append-style shared layouts follow one rule: **Bottle's entries first and verbatim, local entries
kept apart.** `State::ExtraShaderDescriptors` / `Permutation::ExtraFlags` take Bottle's
sequential low bits unchanged and allocate local bits downward from bit 31. The `FeatureData`
cbuffer (`SharedData.hlsli` ↔ `FeatureBuffer.cpp`) takes Bottle's order and appends local
structs at the end, each a 16-byte multiple.

## Workflow

1. Before modifying a shared feature, read its entries in both YAML files.
2. Compare the current source against the pinned Bottle SHA: `python tools/bottle_sync.py`
   (exits non-zero on undocumented drift). `python tools/provenance_audit.py <Feature>` shows
   which upstream each file currently matches.
3. Preserve every documented component, seam and `shared_integrations` hunk.
4. Commit with the repository's conventional types. Record where imported code came from with an
   `Upstream-Source: <upstream key>@<sha>` trailer, one commit per source when a feature combines
   several.
5. Syncing to newer Bottle work is a deliberate step: bring the code up to the new commit and bump
   `bottle.sha` in the same change.

Historical port investigations (before this model existed) are archived in
[`history/upstream-port-tracking-2026-09.md`](./history/upstream-port-tracking-2026-09.md). That
file is not authoritative.
