"""Show where a feature's current source matches each upstream.

For one feature (its paths from docs/development/maintenance-policy.yaml), reports:
  - the Personal commit that introduced each file and its subject;
  - which upstream(s) each file currently matches byte-for-byte (the pinned Bottle
    SHA plus every fetched upstream branch from upstreams.yaml);
  - the provenance evidence recorded in feature-provenance.yaml;
  - Upstream-Source: trailers on Personal commits that touched the feature.

It is an aid to the audit, not an authority: a file matching an upstream today
says nothing about where it was first written.

Usage:
    python tools/provenance_audit.py FoliageLighting
    python tools/provenance_audit.py FoliageLighting --paths src/Features/Foo.cpp package/Shaders/Bar.hlsl
"""

import argparse
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
DOCS_DIR = PROJECT_ROOT / "docs" / "development"


def load_yaml(path):
    try:
        import yaml
    except ImportError:
        sys.exit("PyYAML is required: pip install pyyaml")
    with open(path, encoding="utf-8") as handle:
        return yaml.safe_load(handle) or {}


def git(*args):
    result = subprocess.run(["git", *args], cwd=PROJECT_ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    return result.stdout if result.returncode == 0 else ""


def expand(paths, ref=None):
    """Tracked files under `paths` (directories end in '/'), at `ref` or in the index."""
    if not paths:
        return []
    out = git("ls-tree", "-r", "--name-only", ref, "--", *paths) if ref else git("ls-files", "--", *paths)
    return [line for line in out.splitlines() if line]


def blob(ref, path):
    return git("rev-parse", "--verify", "--quiet", f"{ref}:{path}").strip()


def working_blob(path):
    full = PROJECT_ROOT / path
    if not full.is_file():
        return ""
    return git("hash-object", "--", path).strip()


def references(upstreams):
    """(label, ref) pairs: the pinned Bottle SHA first, then each fetched upstream branch."""
    refs = []
    bottle = upstreams.get("bottle") or {}
    if bottle.get("sha"):
        refs.append((f"bottle@{bottle['sha']}", bottle["sha"]))
    for key, entry in upstreams.items():
        entry = entry or {}
        remote, branch = entry.get("remote"), entry.get("branch")
        if not remote or not branch:
            continue
        ref = f"{remote}/{branch}"
        if git("rev-parse", "--verify", "--quiet", ref).strip():
            refs.append((key if key != "bottle" else f"bottle ({ref})", ref))
    return refs


def main():
    parser = argparse.ArgumentParser(description="Show where a feature's current source matches each upstream.")
    parser.add_argument("feature", help="Feature key as used in maintenance-policy.yaml (its GetShortName())")
    parser.add_argument("--paths", nargs="+", help="Audit these paths instead of the feature's policy paths")
    args = parser.parse_args()

    upstreams = load_yaml(DOCS_DIR / "upstreams.yaml").get("upstreams") or {}
    policy = (load_yaml(DOCS_DIR / "maintenance-policy.yaml").get("features") or {}).get(args.feature) or {}
    provenance = (load_yaml(DOCS_DIR / "feature-provenance.yaml").get("features") or {}).get(args.feature) or {}

    paths = args.paths or policy.get("paths") or []
    for component in (policy.get("components") or {}).values():
        paths += [p for p in (component or {}).get("paths", []) if p not in paths]
    if not paths:
        print(f"{args.feature}: no paths in maintenance-policy.yaml; pass --paths", file=sys.stderr)
        return 2

    refs = references(upstreams)
    files = sorted(set(expand(paths)) | {f for _, ref in refs for f in expand(paths, ref)})

    print(f"Feature: {args.feature}   policy: {policy.get('policy', 'unassigned')}")
    print(f"Compared against: {', '.join(label for label, _ in refs)}")
    print()

    matches, divergent = [], []
    for path in files:
        current = working_blob(path)
        found = [label for label, ref in refs if current and blob(ref, path) == current]
        absent = not current
        if absent:
            present_in = [label for label, ref in refs if blob(ref, path)]
            divergent.append(f"{path} -> absent here; present in {', '.join(present_in) or 'none'}")
        elif found:
            matches.append(f"{path} -> {', '.join(found)}")
        else:
            present_in = [label for label, ref in refs if blob(ref, path)]
            divergent.append(f"{path} -> differs from {', '.join(present_in) or 'every upstream (not present upstream)'}")

    print("Current exact matches:")
    for line in matches or ["(none)"]:
        print(f"  {line}")
    print("Current divergent:")
    for line in divergent or ["(none)"]:
        print(f"  {line}")

    print("Personal introduction:")
    for path in files:
        if not working_blob(path):
            continue
        # --follow pairs an empty marker file (e.g. CORE) with any other empty file ever added.
        follow = ["--follow"] if (PROJECT_ROOT / path).stat().st_size else []
        added = git("log", "--diff-filter=A", *follow, "--format=%h %ad %s", "--date=short", "--", path).splitlines()
        if added:
            print(f"  {path}: {added[-1]}")

    print("Recorded provenance (feature-provenance.yaml):")
    entries = list(provenance.get("provenance") or [])
    for name, component in (provenance.get("components") or {}).items():
        entries += [dict(e, component=name) for e in (component or {}).get("provenance") or []]
    for entry in entries or [{"source": "(none recorded)"}]:
        scope = f"[{entry['component']}] " if entry.get("component") else ""
        evidence = f" evidence {', '.join(map(str, entry['evidence']))}" if entry.get("evidence") else ""
        print(f"  {scope}{entry.get('source')} {entry.get('role', '')} ({entry.get('confidence', '?')}){evidence}")

    trailers = git("log", "--format=%h %(trailers:key=Upstream-Source,valueonly,separator=%x2C )", "--", *paths)
    tagged = [line for line in trailers.splitlines() if len(line.split(" ", 1)) == 2 and line.split(" ", 1)[1].strip()]
    print("Upstream-Source trailers:")
    for line in tagged or ["(none)"]:
        print(f"  {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
