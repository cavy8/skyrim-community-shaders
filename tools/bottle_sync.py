"""Compare the Personal branch against its pinned Bottle maintenance baseline.

Reads docs/development/upstreams.yaml (the pinned `bottle.sha` and the other upstreams) and
docs/development/maintenance-policy.yaml, then checks that every file differing from Bottle is
accounted for:

- features            per-feature policy (bottle-exact, bottle-plus-components, bottle-plus-seam,
                      external-maintained, personal) over the feature's own paths;
- shared_integrations files several features touch; every hunk that differs from Bottle must
                      contain a `match` string of one of the file's documented integrations;
- repository          whole files outside the features (fork docs, build glue, tooling);
- branding            one text rule applied to Bottle's side before comparing, so files that
                      differ only by the product name count as identical.

A differing file that none of these claims is reported as UNCLAIMED drift.

Usage:
    python tools/bottle_sync.py               # report; exit 1 on drift
    python tools/bottle_sync.py --rev HEAD    # check a commit instead of the working tree
    python tools/bottle_sync.py --verbose     # list every drifting file / hunk
    python tools/bottle_sync.py --catalogue   # list every file that differs from Bottle, by owner

Exit codes: 0 no drift, 1 drift, 2 config error.
"""

import argparse
import difflib
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
HUNK_CONTEXT = 3


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


def git(*args, check=True, binary=False):
    result = subprocess.run(["git", *args], cwd=PROJECT_ROOT, capture_output=True)
    if check and result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout if binary else result.stdout.decode("utf-8", errors="replace")


def under(path, prefixes):
    return any(path == p or (p.endswith("/") and path.startswith(p)) for p in prefixes)


class Objects:
    """Cached `git ls-tree -r` listings and a persistent `git cat-file --batch` reader."""

    _listings = {}
    _reader = None

    @classmethod
    def listing(cls, ref):
        if ref not in cls._listings:
            entries = {}
            for line in git("ls-tree", "-r", "-z", ref).split("\0"):
                if line:
                    meta, path = line.split("\t", 1)
                    mode, _, sha = meta.split()
                    entries[path] = (mode, sha)
            cls._listings[ref] = entries
        return cls._listings[ref]

    @classmethod
    def blob(cls, ref, path):
        entry = cls.listing(ref).get(path)
        if entry is None:
            return None
        mode, sha = entry
        if mode == "160000":  # submodule: compare the pointer
            return sha.encode()
        if cls._reader is None:
            cls._reader = subprocess.Popen(["git", "cat-file", "--batch"], cwd=PROJECT_ROOT,
                                           stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        cls._reader.stdin.write(sha.encode() + b"\n")
        cls._reader.stdin.flush()
        size = int(cls._reader.stdout.readline().split()[2])
        data = cls._reader.stdout.read(size)
        cls._reader.stdout.read(1)
        return data


class Tree:
    """Bottle (`base`) vs the working tree or `rev`, with CRLF and branding normalization."""

    def __init__(self, base, rev, branding):
        self.base, self.rev = base, rev
        self.brand_from = branding.get("from")
        self.brand_to = branding.get("to")
        self.brand_paths = branding.get("paths") or []
        self._diff = None

    def _blob(self, ref, path):
        return Objects.blob(ref, path)

    def base_blob(self, path):
        blob = self._blob(self.base, path)
        if blob is not None and self.brand_from and under(path, self.brand_paths):
            blob = blob.replace(self.brand_from.encode(), self.brand_to.encode())
        return blob

    def current_blob(self, path):
        if self.rev:
            return self._blob(self.rev, path)
        full = PROJECT_ROOT / path
        if full.is_dir():  # submodule checkout: the pointer is what the index records
            staged = git("ls-files", "-s", "--", path).split()
            return staged[1].encode() if staged else None
        return full.read_bytes() if full.is_file() else None

    @staticmethod
    def _norm(blob):
        return None if blob is None else blob.replace(b"\r\n", b"\n")

    def differing(self, paths=()):
        """Every path (optionally under `paths`) whose normalized content differs from `base` (cached)."""
        if self._diff is None:
            spec = ["--", *paths] if paths else []
            args = ["diff", "--name-only", "--no-renames", self.base] + ([self.rev] if self.rev else []) + spec
            candidates = set(git(*args).splitlines())
            if not self.rev:
                candidates |= set(git("ls-files", "--others", "--exclude-standard", *spec).splitlines())
            self._diff = sorted(p for p in candidates if p and self._norm(self.base_blob(p)) != self._norm(self.current_blob(p)))
        return self._diff

    def text_pair(self, path):
        a, b = self._norm(self.base_blob(path)), self._norm(self.current_blob(path))
        decode = lambda blob: [] if blob is None else blob.decode("utf-8", errors="replace").split("\n")
        return decode(a), decode(b)

    def hunks(self, path, context=HUNK_CONTEXT):
        """Changed-line groups ('+'/'-' prefixed), merged when closer than `context` lines."""
        a, b = self.text_pair(path)
        groups = []
        for group in difflib.SequenceMatcher(None, a, b, autojunk=False).get_grouped_opcodes(context):
            lines = []
            for tag, i1, i2, j1, j2 in group:
                if tag in ("replace", "delete"):
                    lines += ["-" + line for line in a[i1:i2]]
                if tag in ("replace", "insert"):
                    lines += ["+" + line for line in b[j1:j2]]
            if lines:
                groups.append(lines)
        return groups


@dataclass
class Result:
    name: str
    policy: str
    status: str  # CLEAN, SEAM, COMPONENTS, EXTERNAL, PERSONAL, DRIFT
    summary: str
    details: list = field(default_factory=list)

    @property
    def is_drift(self):
        return self.status == "DRIFT"


def check_exact(name, entry, files):
    if not files:
        return Result(name, "bottle-exact", "CLEAN", "CLEAN")
    return Result(name, "bottle-exact", "DRIFT", f"DRIFT: {len(files)} file(s) differ", files)


def check_components(name, entry, files):
    components = entry.get("components") or {}
    component_paths = [p for c in components.values() for p in (c or {}).get("paths", [])]
    described = ", ".join(f"{key} ({(c or {}).get('source', '?')})" for key, c in components.items()) or "none recorded"
    stray = [f for f in files if not under(f, component_paths)]
    if stray:
        return Result(name, "bottle-plus-components", "DRIFT", f"DRIFT: {len(stray)} file(s) differ outside components [{described}]", stray)
    return Result(name, "bottle-plus-components", "COMPONENTS", f"Bottle base, components: {described}", files)


def seam_allows(path, lines, seam):
    for anchor in seam:
        if anchor.get("path") != path or not anchor.get("match"):
            continue
        patterns, replaces = anchor.get("match") or [], anchor.get("replaces") or []

        def allowed(line):
            if line[1:].strip() == "":
                return True
            return any(p in line for p in (patterns if line[0] == "+" else replaces))

        if all(allowed(line) for line in lines):
            return anchor
    return None


def check_seam(name, entry, files, tree):
    seam = entry.get("seam") or []
    budget = int(entry.get("budget", DEFAULT_SEAM_BUDGET))
    drift, seam_lines, used = [], 0, set()
    for path in files:
        for lines in tree.hunks(path, context=0):
            anchor = seam_allows(path, lines, seam)
            if anchor is None:
                drift.append(f"{path}: {len(lines)} changed line(s) not covered by a seam anchor: {lines[0][:100]}")
            else:
                seam_lines += len(lines)
                used.add(id(anchor))
    if drift:
        return Result(name, "bottle-plus-seam", "DRIFT", f"DRIFT: {len(drift)} hunk(s) outside the seam", drift)
    if seam_lines > budget:
        return Result(name, "bottle-plus-seam", "DRIFT", f"DRIFT: seam is {seam_lines} changed lines, budget {budget}")
    return Result(name, "bottle-plus-seam", "SEAM", f"seam only ({len(used)} of {len(seam)} anchors in use, {seam_lines} changed lines, budget {budget})")


def check_external(name, entry, upstreams, rev):
    """Informational: files that differ from the tracked upstream and are not listed as adaptations."""
    reference = entry.get("reference", "?")
    upstream = upstreams.get(reference) or {}
    ref = f"{upstream.get('remote')}/{upstream.get('branch')}"
    if not upstream or not git("rev-parse", "--verify", "--quiet", ref, check=False).strip():
        return Result(name, "external-maintained", "EXTERNAL", f"tracks {reference} ({ref} not fetched)")
    upstream_tree = Tree(ref, rev, {})
    paths = entry.get("paths") or []
    adaptations = entry.get("adaptations") or {}
    differing = [f for f in upstream_tree.differing(paths) if under(f, paths)]
    undocumented = [f for f in differing if not under(f, list(adaptations))]
    summary = f"tracks {ref}: {len(differing) - len(undocumented)} documented adaptation file(s)"
    if undocumented:
        summary += f", {len(undocumented)} undocumented (local change or upstream moved)"
    return Result(name, "external-maintained", "EXTERNAL", summary, [f"differs from {ref}: {f}" for f in undocumented])


def check_integrations(policy, tree):
    """Every hunk of every shared-integration file must be attributed to a documented entry."""
    problems, attributed = [], {}
    for path, entries in (policy.get("shared_integrations") or {}).items():
        entries = entries or {}
        _, text_lines = tree.text_pair(path)
        text = "\n".join(text_lines)
        if not text_lines or text == "":
            problems.append(f"{path}: file not found")
            continue
        for key, entry in entries.items():
            for pattern in (entry or {}).get("match") or []:
                if pattern not in text:
                    problems.append(f"{path} [{key}]: anchor text not found: {pattern!r}")
        if path not in tree.differing():
            continue
        for lines in tree.hunks(path):
            owners = [key for key, entry in entries.items()
                      if any(p in line for line in lines
                             for p in ((entry or {}).get("match") or []) + ((entry or {}).get("removes") or []))]
            if owners:
                for owner in owners:
                    attributed.setdefault(path, set()).add(owner)
            else:
                first = next((line for line in lines if line[1:].strip()), lines[0])
                problems.append(f"{path}: unattributed hunk ({len(lines)} changed line(s)): {first[:110]}")
    return problems, attributed


def upstream_drift(upstream, policy):
    remote, branch, sha = upstream.get("remote"), upstream.get("branch"), upstream.get("sha")
    ref = f"{remote}/{branch}"
    if not git("rev-parse", "--verify", "--quiet", ref, check=False).strip():
        return f"{ref} not fetched; run `git fetch {remote}`"
    ahead = int(git("rev-list", "--count", f"{sha}..{ref}").strip() or 0)
    if ahead == 0:
        return f"{ref} is at the pin ({sha})"
    touched_files = git("diff", "--name-only", sha, ref).splitlines()
    touched = sorted(name for name, entry in (policy.get("features") or {}).items()
                     if any(under(f, (entry or {}).get("paths") or []) for f in touched_files))
    return f"{ref} is {ahead} commit(s) ahead of {sha}; touched: {', '.join(touched) or 'no tracked feature paths'}"


def main():
    parser = argparse.ArgumentParser(description="Compare Personal against the pinned Bottle maintenance baseline.")
    parser.add_argument("--rev", help="Commit to check (default: the working tree)")
    parser.add_argument("--verbose", "-v", action="store_true", help="List every drifting file / hunk")
    parser.add_argument("--catalogue", action="store_true", help="List every file that differs from Bottle, by owner")
    parser.add_argument("--strict", action="store_true", help="Also fail on undocumented differences from an external feature's upstream")
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

    tree = Tree(base, args.rev, policy.get("branding") or {})
    differing = tree.differing()
    integration_files = set((policy.get("shared_integrations") or {}).keys())
    features = policy.get("features") or {}
    repository = policy.get("repository") or {}

    groups = {p: [] for p in POLICIES}
    owner = {}
    for name, entry in features.items():
        entry = entry or {}
        kind = entry.get("policy")
        paths = entry.get("paths") or []
        files = [f for f in differing if under(f, paths)]
        for f in files:
            owner.setdefault(f, f"feature {name}")
        own_files = [f for f in files if f not in integration_files]
        if kind == "bottle-exact":
            result = check_exact(name, entry, own_files)
        elif kind == "bottle-plus-components":
            result = check_components(name, entry, own_files)
        elif kind == "bottle-plus-seam":
            result = check_seam(name, entry, own_files, tree)
        elif kind == "external-maintained":
            result = check_external(name, entry, upstreams, args.rev)
            if not args.strict:
                result.details = [d for d in result.details if args.verbose]
        elif kind == "personal":
            result = Result(name, kind, "PERSONAL", "")
        else:
            print(f"error: {name}: unknown policy {kind!r}", file=sys.stderr)
            return 2
        shared = sorted(set(files) & integration_files)
        if shared:
            result.summary += f"  [+{len(shared)} shared-integration file(s)]"
        if args.strict and kind == "external-maintained" and "undocumented" in result.summary:
            result.status = "DRIFT"
        groups[kind].append(result)

    for f in differing:
        if f in integration_files:
            owner[f] = "shared integrations"
        elif f not in owner:
            for name, entry in repository.items():
                if under(f, (entry or {}).get("paths") or []):
                    owner[f] = f"repository {name}"
                    break
    unclaimed = [f for f in differing if f not in owner]

    titles = {"bottle-exact": "Bottle exact:", "bottle-plus-seam": "Bottle + seam:",
              "bottle-plus-components": "Bottle + components:", "external-maintained": "External:", "personal": "Personal:"}
    drift = False
    for key in ("bottle-exact", "bottle-plus-seam", "bottle-plus-components", "external-maintained", "personal"):
        if not groups[key]:
            continue
        print(titles[key])
        for result in groups[key]:
            print(f"  {result.name:<24} {result.summary}".rstrip())
            if args.verbose or result.is_drift:
                limit = None if args.verbose else 10
                for detail in result.details[:limit]:
                    print(f"      {detail}")
                if limit and len(result.details) > limit:
                    print(f"      ... {len(result.details) - limit} more (--verbose)")
            drift |= result.is_drift

    problems, attributed = check_integrations(policy, tree)
    print("Shared integrations:")
    for path in sorted(integration_files):
        state = "differs" if path in differing else "matches Bottle"
        owners = ", ".join(sorted(attributed.get(path, []))) or "-"
        print(f"  {path:<58} {state}; hunks: {owners}")
    for problem in problems:
        print(f"  DRIFT {problem}")
    drift |= bool(problems)

    print("Repository:")
    for name, entry in repository.items():
        files = [f for f, o in owner.items() if o == f"repository {name}"]
        print(f"  {name:<24} {len(files)} file(s) differ ({(entry or {}).get('source', '?')})")
    if unclaimed:
        print("Unclaimed (differs from Bottle, owned by nothing in the policy):")
        for f in unclaimed:
            print(f"  DRIFT {f}")
        drift = True

    if args.catalogue:
        print(f"Catalogue ({len(differing)} file(s) differ from {base}):")
        for f in differing:
            print(f"  {owner.get(f, 'UNCLAIMED'):<40} {f}")

    print(f"Upstream drift since pin:  {upstream_drift(bottle, policy)}")
    return 1 if drift else 0


if __name__ == "__main__":
    sys.exit(main())
