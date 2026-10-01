"""Checks the hair cards to strands conversion on synthetic hairstyles (synth.py).

Builds the converter (convert.cpp + CardsToStrands.cpp) with the host C++ compiler, converts
each style and measures the strands against the known skull and the cards:

- roots: share of strands rooted on the skull (within 1 unit above it, or tucked up to 0.5 under),
  and share rooted more than 2 units off it ("floating" roots, the fault this module fixes), of
  the strands that grow from the scalp; strands growing from a tie must root within its reach;
- kept: share of the triangles kept as cards (braids, ties, the hair gathered into them; none in
  loose styles but the stray piece), chains and ties found;
- length: median strand length, share of stubs under 2 units;
- head: share of strand points more than 0.3 units inside the skull;
- fidelity: distance from strand points to the nearest card (median, 95th percentile);
- coverage: share of the painted card surface converted to strands within 0.75 units of a strand;
- stray: strands grown on the detached piece of layered_long (must be none).

Fails (exit 1) if a limit is missed. With --render DIR it also draws each style from the side,
back and top. With --compare EXE it runs a second converter (same CLI, for example a build of
the previous generator) and prints its numbers beside these.

    python tools/hair_cards_to_strands/check.py [--render DIR] [--compare EXE]

Needs numpy and scipy (and matplotlib for --render).
"""

import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
CORE = os.path.join(ROOT, "src", "Features", "HairStrands", "CardsToStrands")
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True  # no __pycache__ in the source tree

import ctsio  # noqa: E402
import synth  # noqa: E402

LIMITS = {
    "rooted": 0.97,        # at least this share rooted on the skull
    "floating": 0.01,      # at most this share rooted over 2 units off it
    "stubs": 0.05,         # at most this share under 2 units
    "inside": 0.01,        # at most this share of points over 0.3 units inside the skull
    "fidelity95": 2.0,     # 95% of points within this of a card
    "coverage": 0.85,      # at least this share of the painted cards near a strand
    "stray": 0,
    "kept": 0.01,          # at most this share of a loose style's triangles kept as cards
    "tie_roots": 1.5,      # tied strands root within their tie's radius and this much more
}

# Styles whose cards gather into a tie: the tie found, its tail growing from it. With the texture
# the cap's painted hair stops too far short of the tail (more than 2.5 units) to find it: the tail
# carries on from the cap as before.
TIED = {("ponytail", "no texture")}


def build(out_dir):
    exe = os.path.join(out_dir, "cards_to_strands")
    compiler = os.environ.get("CXX", "g++")
    subprocess.run([compiler, "-std=c++20", "-O2", "-I", CORE, os.path.join(HERE, "convert.cpp"),
                    os.path.join(CORE, "CardsToStrands.cpp"), "-o", exe], check=True)
    return exe


def card_samples(builder, alpha):
    """Points over the painted cards, from the mesh's triangles and the atlas alpha, and their triangles."""
    pos, uv, idx, _ = builder.arrays()
    tri = idx.reshape(-1, 3)
    rng = np.random.default_rng(0)
    r1 = np.sqrt(rng.random((len(tri), 12)))
    r2 = rng.random((len(tri), 12))
    w = np.stack([1 - r1, r1 * (1 - r2), r1 * r2], axis=-1)
    p = np.einsum("tsk,tkd->tsd", w, pos[tri]).reshape(-1, 3)
    t = np.einsum("tsk,tkd->tsd", w, uv[tri]).reshape(-1, 2)
    h, wd = alpha.shape
    a = alpha[np.clip((t[:, 1] * h).astype(int), 0, h - 1), np.clip((t[:, 0] * wd).astype(int), 0, wd - 1)] / 255.0
    return p, a, np.repeat(np.arange(len(tri)), 12)


def measure(result, builder, alpha, lift=0.0):
    pos = result["positions"]
    scalp = result["strands"]["scalpRooted"] == 1
    roots = pos[scalp, 0]
    root_height = synth.skull_height(roots)
    lengths = result["strands"]["length"]
    surface, painted, owner = card_samples(builder, alpha)
    tree = cKDTree(surface)
    pts = pos.reshape(-1, 3)
    dist, _ = tree.query(pts)
    # Coverage of the cards converted to strands; the ones kept as cards show themselves.
    regions = result.get("triangleRegions")
    converted = regions[owner] == 0 if regions is not None else np.ones(len(owner), bool)
    painted_pts = surface[(painted >= 0.3) & converted]
    near, _ = cKDTree(pts).query(painted_pts, distance_upper_bound=0.75 + lift)
    stray = [c for name, c in builder.cards if name == "stray"]
    stray_count = 0
    if stray:
        d, _ = cKDTree(stray[0]).query(pts)
        stray_count = int(np.any((d < 1.5).reshape(pos.shape[:2]), axis=1).sum())
    # Strands growing from a tie root round it: within its radius and a margin.
    ties = result.get("ties", [])
    tie_roots = 0.0
    tied = result["strands"]["cardGuide"][~scalp]
    for s, g in zip(np.nonzero(~scalp)[0], tied):
        tie = result["cardGuides"][g]["tie"] if g < len(result["cardGuides"]) else -1
        if 0 <= tie < len(ties):
            tie_roots = max(tie_roots, float(np.linalg.norm(pos[s, 0] - ties[tie]["centre"]) - ties[tie]["radius"]))
    return {
        "strands": len(lengths),
        "points": result["pointsPerStrand"],
        "median_length": float(np.median(lengths)),
        "stubs": float(np.mean(lengths < 2.0)),
        "rooted": float(np.mean((root_height < 1.0) & (root_height > -0.5))) if len(roots) else 1.0,
        "rooted_loose": float(np.mean((root_height < 1.5) & (root_height > -0.5))) if len(roots) else 1.0,
        "floating": float(np.mean(root_height > 2.0)) if len(roots) else 0.0,
        "tied": float(np.mean(~scalp)),
        "tie_roots": tie_roots,
        "kept": float(np.mean(regions != 0)) if regions is not None else 0.0,
        "chains": len(result.get("chains", [])),
        "ties": len(ties),
        "inside": float(np.mean(synth.skull_height(pts) < -0.3)),
        "fidelity50": float(np.median(dist)),
        "fidelity95": float(np.percentile(dist, 95)),
        "coverage": float(np.mean(np.isfinite(near))),
        "stray": stray_count,
    }


def render(path, title, results, builder):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    views = (("side", 1, 2), ("back", 0, 2), ("top", 0, 1))
    fig, axes = plt.subplots(len(results), 3, figsize=(15, 5.4 * len(results)), squeeze=False)
    for row, (label, result) in enumerate(results):
        pos = result["positions"]
        rng = np.random.default_rng(1)
        pick = rng.permutation(len(pos))[:2500]
        heights = synth.skull_height(pos[pick, 0])
        for col, (name, i, j) in enumerate(views):
            ax = axes[row][col]
            t = np.linspace(0, 2 * np.pi, 200)
            ax.plot(synth.SKULL_CENTRE[i] + synth.SKULL_RADII[i] * np.cos(t), synth.SKULL_CENTRE[j] + synth.SKULL_RADII[j] * np.sin(t),
                    color="0.6", lw=1.0)
            for _, c in builder.cards:
                ax.plot(c[:, i], c[:, j], color="#9ecae1", lw=0.6, alpha=0.6)
            for k, s in enumerate(pick):
                ax.plot(pos[s, :, i], pos[s, :, j], color="#6b3e26", lw=0.25, alpha=0.35)
            off = heights > 2.0
            ax.scatter(pos[pick[~off], 0, i], pos[pick[~off], 0, j], s=1.5, color="#2b8a3e", zorder=3)
            ax.scatter(pos[pick[off], 0, i], pos[pick[off], 0, j], s=3, color="#d6336c", zorder=4)
            ax.set_aspect("equal")
            ax.set_title(f"{label}: {name}", fontsize=10)
            ax.tick_params(labelsize=7)
    fig.suptitle(f"{title}  (green roots on the scalp, pink roots over 2 units off it; blue: card centrelines)", fontsize=11)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


# Generation settings of the in-game presets (Strands::MakePresetStyle) that differ from the defaults.
PRESETS = {
    "curly": ["density=14", "segmentLength=0.8", "clumpStrength=0.55", "clumpSize=0.8", "volume=0.4"],
    "coily": ["density=20", "segmentLength=0.75", "clumpStrength=0.1", "clumpSize=0.6", "volume=0.8", "layerJitter=0.3"],
    "locs": ["density=16", "clumpStrength=0.95", "clumpSize=0.9", "clumpTwist=0.35", "volume=0.1"],
}

# (style, label, converter arguments, with the texture, compare with --compare)
CASES = [(name, "default", [], True, True) for name in synth.STYLES] + [
    ("layered_long", preset, args, True, False) for preset, args in PRESETS.items()] + [
    ("layered_long", "no texture", [], False, False),
    ("ponytail", "no texture", [], False, False),
    ("layered_long", "area seeding", ["seeding=area"], True, False),
    ("layered_long", "wide clumps", ["clumpSize=6"], True, False),
    ("layered_long", "seed 7", ["seed=7"], True, False),
]


def check(m, name, label):
    problems = []
    if (name, label) in TIED:
        if m["ties"] < 1:
            problems.append("no tie found")
        if m["tie_roots"] > LIMITS["tie_roots"]:
            problems.append(f"tie roots {m['tie_roots']:.2f} past the tie > {LIMITS['tie_roots']}")
    elif m["kept"] > LIMITS["kept"] or m["chains"] or m["ties"]:
        problems.append(f"loose hair kept as cards {m['kept']:.3f} (chains {m['chains']}, ties {m['ties']})")
    # Area seeding grows short fuzz on purpose; only the roots matter there.
    stubs_limit = 1.01 if label == "area seeding" or m.get("seeding") == 2 else LIMITS["stubs"]
    coverage_limit = 0.0 if label == "area seeding" or m.get("seeding") == 2 else LIMITS["coverage"]
    # Area seeding roots strands on the cards themselves, up to 1.25 above the fitted scalp.
    rooted = m["rooted_loose"] if m.get("seeding") == 2 else m["rooted"]
    if rooted < LIMITS["rooted"]:
        problems.append(f"rooted {rooted:.3f} < {LIMITS['rooted']}")
    if m["floating"] > LIMITS["floating"]:
        problems.append(f"floating {m['floating']:.3f} > {LIMITS['floating']}")
    if m["stubs"] > stubs_limit:
        problems.append(f"stubs {m['stubs']:.3f} > {stubs_limit}")
    if m["inside"] > LIMITS["inside"]:
        problems.append(f"inside {m['inside']:.3f} > {LIMITS['inside']}")
    if m["fidelity95"] > LIMITS["fidelity95"]:
        problems.append(f"fidelity95 {m['fidelity95']:.2f} > {LIMITS['fidelity95']}")
    if m["coverage"] < coverage_limit:
        problems.append(f"coverage {m['coverage']:.3f} < {coverage_limit}")
    if m["stray"] > LIMITS["stray"]:
        problems.append(f"stray {m['stray']} > 0")
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--render", help="directory for PNG renders of the default cases")
    parser.add_argument("--compare", help="another converter executable to report beside this one on the default cases")
    parser.add_argument("--keep", help="directory to keep the meshes and results in")
    args = parser.parse_args()

    work = args.keep or tempfile.mkdtemp(prefix="cards_to_strands_")
    os.makedirs(work, exist_ok=True)
    exe = build(work)
    failed = False
    built = {}
    for name, label, extra, textured, compare in CASES:
        if name not in built:
            built[name] = synth.STYLES[name]()
        builder, alpha, shade = built[name]
        pos, uv, idx, nrm = builder.arrays()
        tag = f"{name}.{label.replace(' ', '_')}"
        mesh = os.path.join(work, f"{tag}.ctsm")
        ctsio.write_mesh(mesh, pos, uv, idx, normals=nrm, alpha=alpha if textured else None, shade=shade if textured else None)
        # Without a texture every part of the cards counts as hair: measure coverage against that.
        coverage_alpha = alpha if textured else np.full_like(alpha, 255)
        # Strands lifted off the cards (volume, layer jitter) on purpose count as covering them.
        settings = dict(a.split("=") for a in extra)
        lift = max(0.0, float(settings.get("volume", 0.15)) + float(settings.get("layerJitter", 0.12)) - 0.27)
        runs = []
        for which, converter in (("new", exe), ("before", args.compare if compare else None)):
            if not converter:
                continue
            out = os.path.join(work, f"{tag}.{which}.ctsr")
            log = subprocess.run([converter, mesh, out] + (extra if which == "new" else []), capture_output=True, text=True)
            if log.returncode != 0:
                print(f"{name} / {label} [{which}]: converter failed: {log.stderr.strip()}")
                failed = failed or which == "new"
                continue
            result = ctsio.read_result(out)
            m = measure(result, builder, coverage_alpha, lift)
            m["seeding"] = result["stats"]["seedingUsed"]
            runs.append((which, result))
            print(f"{name} / {label} [{which}] {log.stdout.strip()}")
            print("    " + ", ".join(f"{k} {v:.3f}" if isinstance(v, float) else f"{k} {v}" for k, v in m.items()))
            if which != "new":
                continue
            problems = check(m, name, label)
            if problems:
                failed = True
                print("    FAIL: " + "; ".join(problems))
        if args.render and runs and label == "default":
            os.makedirs(args.render, exist_ok=True)
            render(os.path.join(args.render, f"{name}.png"), name, runs, builder)
    print("FAILED" if failed else "OK")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
