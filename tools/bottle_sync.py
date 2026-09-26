"""Compare the Personal branch against its pinned Bottle maintenance baseline.

Reads docs/development/upstreams.yaml (the pinned `bottle.sha`) and
docs/development/maintenance-policy.yaml (per-feature policies, component paths,
the Upscaling seam and shared_integrations), then reports, per feature, whether
the working tree matches Bottle, differs only by documented components/seams, or
has drifted.

Usage:
    python tools/bottle_sync.py              # report; exit 1 on DRIFT
    python tools/bottle_sync.py --rev HEAD   # compare a commit instead of the working tree
    python tools/bottle_sync.py --verbose    # list every drifting file / hunk

Exit codes: 0 no drift, 1 drift (or unassigned features with --strict), 2 config error.
"""

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
DOCS_DIR = PROJECT_ROOT / "docs" / "development"
UPSTREAMS_FILE = DOCS_DIR / "upstreams.yaml"
POLICY_FILE = DOCS_DIR / "maintenance-policy.yaml"

POLICIES = ("bottle-exact", "bottle-plus-components", "bottle-plus-seam", "external-maintained", "personal")
DEFAULT_SEAM_BUDGET = 20
RE_HUNK_HEADER = re.compile(r"^@@ -\d+(?:,\d+)? \+\d+(?:,\d+)? @@")


def load_yaml(path):
    try:
        import yaml
    except ImportError:
        sys.exit("PyYAML is required: pip install pyyaml")
    try:
        with open(path, encoding="utf-8") as handle:
            return yaml.safe_load(handle) or {}
    except (OSError, yaml.YAMLError) as error:
        print(f"error: cannot read {path.relative_to(PROJECT_ROOT)}: {error}", file=sys.stderr)
        sys.exit(2)


def git(*args, check=True):
    result = subprocess.run(["git", *args], cwd=PROJECT_ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def diff_args(base, rev):
    return [base, rev] if rev else [base]


def changed_files(base, rev, paths, exclude=()):
    """Files under `paths` that differ between `base` and `rev` (or the working tree)."""
    if not paths:
        return []
    specs = list(paths) + [f":(exclude){p}" for p in exclude]
    out = git("diff", "--name-only", "--no-renames", *diff_args(base, rev), "--", *specs)
    files = [line for line in out.splitlines() if line]
    if not rev:
        # Untracked files are invisible to `git diff` but are still drift.
        untracked = git("ls-files", "--others", "--exclude-standard", "--", *specs)
        files += [line for line in untracked.splitlines() if line]
    return sorted(set(files))


@dataclass
class Hunk:
    path: str
    lines: list = field(default_factory=list)  # changed lines, with their +/- prefix

    @property
    def changed(self):
        return len(self.lines)


def diff_hunks(base, rev, paths):
    out = git("diff", "--no-renames", "-U0", *diff_args(base, rev), "--", *paths)
    hunks, current, path = [], None, None
    for line in out.splitlines():
        if line.startswith("diff --git"):
            current = None
        elif line.startswith("+++ "):
            target = line[4:]
            path = target[2:] if target.startswith("b/") else path
        elif line.startswith("--- "):
            source = line[4:]
            if source.startswith("a/"):
                path = source[2:]
        elif RE_HUNK_HEADER.match(line):
            current = Hunk(path)
            hunks.append(current)
        elif current is not None and line and line[0] in "+-" and not line.startswith(("+++", "---")):
            current.lines.append(line)
    return hunks


@dataclass
class Result:
    feature: str
    policy: str
    status: str  # CLEAN, SEAM, COMPONENTS, DRIFT, EXTERNAL, PERSONAL, UNASSIGNED, MISSING
    summary: str
    details: list = field(default_factory=list)

    @property
    def is_drift(self):
        return self.status in ("DRIFT", "MISSING")


def check_exact(name, entry, base, rev):
    paths = entry.get("paths") or []
    if not paths:
        return Result(name, "bottle-exact", "DRIFT", "no paths listed (cannot verify)")
    files = changed_files(base, rev, paths)
    if not files:
        return Result(name, "bottle-exact", "CLEAN", "CLEAN")
    return Result(name, "bottle-exact", "DRIFT", f"DRIFT: {len(files)} file(s) differ", files)


def check_components(name, entry, base, rev):
    paths = entry.get("paths") or []
    components = entry.get("components") or {}
    component_paths = sorted({p for c in components.values() for p in (c or {}).get("paths", [])})
    if not paths:
        return Result(name, "bottle-plus-components", "DRIFT", "no paths listed (cannot verify)")
    stray = changed_files(base, rev, paths, exclude=component_paths)
    sources = sorted({(c or {}).get("source", "?") for c in components.values()})
    described = ", ".join(f"{component} ({(c or {}).get('source', '?')})" for component, c in components.items()) or "none recorded"
    if stray:
        return Result(name, "bottle-plus-components", "DRIFT",
                      f"DRIFT: {len(stray)} file(s) differ outside components [{described}]", stray)
    differing = changed_files(base, rev, component_paths) if component_paths else []
    summary = f"Bottle base, components: {described}"
    if sources:
        summary += f"; {len(differing)} component path(s) differ"
    return Result(name, "bottle-plus-components", "COMPONENTS", summary, differing)


def seam_allows(hunk, seam):
    for anchor in seam:
        if anchor.get("path") != hunk.path:
            continue
        patterns = anchor.get("match") or []
        replaces = anchor.get("replaces") or []
        if not patterns:
            continue

        def allowed(line):
            if line[1:].strip() == "":
                return True
            # Added lines must be seam code; removed lines only the Bottle code the anchor replaces.
            return any(p in line for p in (patterns if line[0] == "+" else replaces))

        if all(allowed(line) for line in hunk.lines):
            return anchor
    return None


def check_seam(name, entry, base, rev):
    paths = entry.get("paths") or []
    seam = entry.get("seam") or []
    budget = int(entry.get("budget", DEFAULT_SEAM_BUDGET))
    if not paths:
        return Result(name, "bottle-plus-seam", "DRIFT", "no paths listed (cannot verify)")
    hunks = diff_hunks(base, rev, paths)
    untracked = changed_files(base, rev, paths) if not rev else []
    tracked_changed = {h.path for h in hunks}
    new_files = [f for f in untracked if f not in tracked_changed and not git("ls-files", "--", f).strip()]
    drift = [f"{h.path}: {h.changed} changed line(s) not covered by a seam anchor: {h.lines[0][:100]}"
             for h in hunks if not seam_allows(h, seam)]
    drift += [f"{f}: untracked file" for f in new_files]
    seam_lines = sum(h.changed for h in hunks if seam_allows(h, seam))
    anchors_used = {id(a) for h in hunks if (a := seam_allows(h, seam))}
    if drift:
        return Result(name, "bottle-plus-seam", "DRIFT", f"DRIFT: {len(drift)} hunk(s) outside the seam", drift)
    if seam_lines > budget:
        return Result(name, "bottle-plus-seam", "DRIFT", f"DRIFT: seam is {seam_lines} changed lines, budget {budget}")
    return Result(name, "bottle-plus-seam", "SEAM",
                  f"seam only ({len(anchors_used)} of {len(seam)} anchors in use, {seam_lines} changed lines, budget {budget})")


def check_shared_integrations(policy, rev):
    """Every shared_integrations entry that carries a `match` must still be present in its file."""
    problems = []
    for path, entries in (policy.get("shared_integrations") or {}).items():
        try:
            text = git("show", f"{rev}:{path}") if rev else (PROJECT_ROOT / path).read_text(encoding="utf-8", errors="replace")
        except (OSError, RuntimeError):
            problems.append(f"{path}: file not found")
            continue
        for key, entry in (entries or {}).items():
            for pattern in (entry or {}).get("match") or []:
                if pattern not in text:
                    problems.append(f"{path} [{key}]: anchor text not found: {pattern!r}")
    return problems


def upstream_drift(upstream, policy):
    remote, branch, sha = upstream.get("remote"), upstream.get("branch"), upstream.get("sha")
    ref = f"{remote}/{branch}"
    if not git("rev-parse", "--verify", "--quiet", ref, check=False).strip():
        return f"{ref} not fetched; run `git fetch {remote}`"
    ahead = int(git("rev-list", "--count", f"{sha}..{ref}").strip() or 0)
    if ahead == 0:
        return f"{ref} is at the pin ({sha})"
    touched_files = git("diff", "--name-only", sha, ref).splitlines()
    touched = []
    for name, entry in (policy.get("features") or {}).items():
        paths = (entry or {}).get("paths") or []
        if any(f == p or (p.endswith("/") and f.startswith(p)) for f in touched_files for p in paths):
            touched.append(name)
    return f"{ref} is {ahead} commit(s) ahead of {sha}; touched: {', '.join(sorted(touched)) or 'no tracked feature paths'}"


def main():
    parser = argparse.ArgumentParser(description="Compare Personal against the pinned Bottle maintenance baseline.")
    parser.add_argument("--rev", help="Commit to check (default: the working tree)")
    parser.add_argument("--verbose", "-v", action="store_true", help="List every drifting file / hunk")
    parser.add_argument("--strict", action="store_true", help="Also fail when a feature has no policy assigned")
    args = parser.parse_args()

    upstreams = load_yaml(UPSTREAMS_FILE).get("upstreams") or {}
    policy = load_yaml(POLICY_FILE)
    bottle = upstreams.get("bottle") or {}
    base = bottle.get("sha")
    if not base:
        print("error: upstreams.yaml has no bottle.sha", file=sys.stderr)
        return 2
    if not git("rev-parse", "--verify", "--quiet", f"{base}^{{commit}}", check=False).strip():
        print(f"error: pinned Bottle SHA {base} is not in this clone; run `git fetch {bottle.get('remote', 'bottle')}`", file=sys.stderr)
        return 2

    checks = {"bottle-exact": check_exact, "bottle-plus-components": check_components, "bottle-plus-seam": check_seam}
    groups = {p: [] for p in POLICIES}
    groups["unassigned"] = []
    for name, entry in (policy.get("features") or {}).items():
        entry = entry or {}
        kind = entry.get("policy")
        if kind in checks:
            groups[kind].append(checks[kind](name, entry, base, args.rev))
        elif kind == "external-maintained":
            groups[kind].append(Result(name, kind, "EXTERNAL", entry.get("reference", "?")))
        elif kind == "personal":
            groups[kind].append(Result(name, kind, "PERSONAL", ""))
        elif kind is None:
            groups["unassigned"].append(Result(name, "-", "UNASSIGNED", entry.get("note", "")))
        else:
            print(f"error: {name}: unknown policy {kind!r}", file=sys.stderr)
            return 2

    titles = {
        "bottle-exact": "Bottle exact:",
        "bottle-plus-seam": "Bottle + seam:",
        "bottle-plus-components": "Bottle + components:",
        "external-maintained": "External:",
        "personal": "Personal:",
        "unassigned": "No policy yet:",
    }
    drift = False
    for key in ("bottle-exact", "bottle-plus-seam", "bottle-plus-components", "external-maintained", "personal", "unassigned"):
        results = groups[key]
        if not results:
            continue
        print(titles[key])
        for result in results:
            print(f"  {result.feature:<24} {result.summary}".rstrip())
            if args.verbose or result.is_drift:
                limit = None if args.verbose else 10
                for detail in result.details[:limit]:
                    print(f"      {detail}")
                if limit and len(result.details) > limit:
                    print(f"      ... {len(result.details) - limit} more (--verbose)")
            drift |= result.is_drift

    problems = check_shared_integrations(policy, args.rev)
    if problems:
        print("Shared integrations:")
        for problem in problems:
            print(f"  DRIFT {problem}")
        drift = True

    print(f"Upstream drift since pin:  {upstream_drift(bottle, policy)}")

    if drift:
        return 1
    if args.strict and groups["unassigned"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
