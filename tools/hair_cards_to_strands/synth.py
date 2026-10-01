"""Synthetic hair-card meshes for checking the cards to strands conversion.

Each style is built round a known skull (an ellipsoid), the way Skyrim hair meshes are laid
out: a scalp cap of cards combed back from the hairline, card layers hanging from the back of
the head, an outer layer starting above the cap (away from the scalp), under-layers starting
below the head, side locks, a fringe, and the awkward parts real meshes have: cards mapped
upside down in the atlas, double-sided cards, transparent gaps and tapered tips in the texture,
and a small detached piece that is not hair from the head at all. Units are Skyrim units, +Y is
forward, +Z up, the head bone at the origin.
"""

import math

import numpy as np

SKULL_CENTRE = np.array([0.0, -0.5, 6.0])
SKULL_RADII = np.array([6.0, 7.2, 7.6])
ATLAS = 256
STRIPS = 8  # atlas strips across U, hair along V


def skull_point(direction):
    d = np.asarray(direction, float)
    d = d / np.linalg.norm(d)
    r = 1.0 / math.sqrt(float(np.sum((d / SKULL_RADII) ** 2)))
    return SKULL_CENTRE + d * r


def skull_height(points):
    """Approximate height of points above the skull ellipsoid (exact on the surface, scaled off it)."""
    q = (np.asarray(points) - SKULL_CENTRE) / SKULL_RADII
    k = np.linalg.norm(q, axis=-1)
    d = np.asarray(points) - SKULL_CENTRE
    return np.linalg.norm(d, axis=-1) * (1.0 - 1.0 / np.maximum(k, 1e-6))


def skull_normal(point):
    n = (np.asarray(point) - SKULL_CENTRE) / SKULL_RADII ** 2
    return n / np.linalg.norm(n)


def meridian(phi, theta):
    """A direction on the head: theta runs front (0) over the top (90) to the back (180) in a
    plane tilted phi degrees to the side about the front-back axis."""
    t, p = math.radians(theta), math.radians(phi)
    y, z = math.cos(t), math.sin(t)
    return np.array([z * math.sin(p), y, z * math.cos(p)])


def scalp_path(phi, theta0, theta1, lift, samples=40):
    pts = [skull_point(meridian(phi, th)) for th in np.linspace(theta0, theta1, samples)]
    return np.array([p + skull_normal(p) * lift for p in pts])


def resample(path, step=0.5):
    seg = np.linalg.norm(np.diff(path, axis=0), axis=1)
    s = np.concatenate([[0.0], np.cumsum(seg)])
    n = max(2, int(math.ceil(s[-1] / step)) + 1)
    targets = np.linspace(0.0, s[-1], n)
    return np.stack([np.interp(targets, s, path[:, k]) for k in range(3)], axis=1)


def hang(path, length, outward=1.0, turn=4.0):
    """Continues a path downwards for `length` units, turning to vertical over `turn` units."""
    p = path[-1].copy()
    t = path[-1] - path[-2]
    t /= np.linalg.norm(t)
    horizontal = np.array([p[0] - SKULL_CENTRE[0], p[1] - SKULL_CENTRE[1], 0.0])
    if np.linalg.norm(horizontal) > 1e-6:
        horizontal /= np.linalg.norm(horizontal)
    out = []
    step = 0.5
    for i in range(int(length / step)):
        s = (i + 1) * step
        w = min(1.0, s / turn)
        d = (1.0 - w) * t + w * np.array([0.0, 0.0, -1.0]) + horizontal * outward * 0.08 * math.exp(-s / 6.0)
        d /= np.linalg.norm(d)
        p = p + d * step
        out.append(p.copy())
    return np.concatenate([path, np.array(out)]) if out else path


class Builder:
    def __init__(self, seed=1):
        self.positions, self.normals, self.uvs, self.indices = [], [], [], []
        self.rng = np.random.default_rng(seed)
        self.cards = []  # (name, centreline) for checks

    def card(self, centre, width, strip, name, flip_v=False, double_sided=False, across_hint=None, across_steps=3):
        centre = resample(np.asarray(centre, float), 0.6)
        n = len(centre)
        tangents = np.gradient(centre, axis=0)
        tangents /= np.linalg.norm(tangents, axis=1, keepdims=True)
        across = np.zeros_like(centre)
        for i in range(n):
            outward = centre[i] - SKULL_CENTRE
            if across_hint is not None:
                a = np.asarray(across_hint, float)
            else:
                a = np.cross(tangents[i], outward)
            a = a - tangents[i] * np.dot(a, tangents[i])
            if i > 0 and np.dot(a, across[i - 1]) < 0:
                a = -a
            across[i] = a / np.linalg.norm(a)
        u0, u1 = strip / STRIPS + 0.004, (strip + 1) / STRIPS - 0.004
        base = len(self.positions)
        verts = []
        for i in range(n):
            v = i / (n - 1)
            if flip_v:
                v = 1.0 - v
            for j in range(across_steps):
                f = j / (across_steps - 1)
                p = centre[i] + across[i] * (f - 0.5) * width
                verts.append(p)
                self.positions.append(p)
                self.uvs.append((u0 + (u1 - u0) * f, v))
        grid = np.array(verts).reshape(n, across_steps, 3)
        normals = np.zeros_like(grid)
        for i in range(n):
            for j in range(across_steps):
                nn = np.cross(across[i], tangents[i])
                if np.dot(nn, grid[i, j] - SKULL_CENTRE) < 0:
                    nn = -nn
                normals[i, j] = nn
        self.normals.extend(normals.reshape(-1, 3))
        tris = []
        for i in range(n - 1):
            for j in range(across_steps - 1):
                a = base + i * across_steps + j
                b, c, d = a + 1, a + across_steps, a + across_steps + 1
                tris += [(a, c, b), (b, c, d)]
        self.indices.extend(tris)
        if double_sided:
            back = len(self.positions)
            self.positions.extend(verts)
            self.uvs.extend(self.uvs[base:base + len(verts)])
            self.normals.extend(-normals.reshape(-1, 3))
            self.indices.extend([(x - base + back, z - base + back, y - base + back) for x, y, z in tris])
        self.cards.append((name, centre))

    def texture(self, gaps=()):
        """Atlas: strips of painted strands along V, tapering at the tip; `gaps` strips have a transparent band."""
        u = (np.arange(ATLAS) + 0.5) / ATLAS
        v = (np.arange(ATLAS) + 0.5) / ATLAS
        uu, vv = np.meshgrid(u, v)
        strip_u = (uu * STRIPS) % 1.0
        locks = 0.5 + 0.5 * np.sin(2 * math.pi * (strip_u * 9.0 + 0.15 * np.sin(vv * 7.0)))
        alpha = np.clip(1.6 * locks - 0.15, 0.0, 1.0)
        alpha *= np.clip(vv / 0.03, 0.0, 1.0) * np.clip((1.0 - vv) / 0.18, 0.0, 1.0)
        edge = np.minimum(strip_u, 1.0 - strip_u)
        alpha *= np.clip(edge / 0.06, 0.0, 1.0)
        for strip in gaps:
            band = (np.floor(uu * STRIPS) == strip) & (vv > 0.47) & (vv < 0.55)
            alpha[band] = 0.0
        streaks = 0.55 + 0.45 * np.sin(2 * math.pi * (strip_u * 31.0 + 0.3 * np.sin(vv * 3.0)))
        shade = alpha * streaks
        return (alpha * 255).astype(np.uint8), (shade * 255).astype(np.uint8)

    def arrays(self):
        return (np.array(self.positions, np.float32), np.array(self.uvs, np.float32),
                np.array(self.indices, np.uint32), np.array(self.normals, np.float32))


def cap(b, phis, theta_back, lift=0.25, width=2.6, strip=0):
    for phi in phis:
        front = 35.0 + abs(phi) * 0.35  # the hairline sits further back at the temples
        b.card(scalp_path(phi, front, theta_back, lift), width, strip + int(abs(phi) // 30) % 2, f"cap{phi:+.0f}",
               flip_v=phi > 50)


def layered_long(seed=1):
    """Long hair to the shoulder blades: cap, back curtain, outer layer, under-layer, side locks, fringe, a stray piece."""
    b = Builder(seed)
    cap(b, np.linspace(-80, 80, 15), 205.0)
    # Back curtain: from the back of the head, on the cap, down to the shoulder blades.
    for psi in np.linspace(-115, 115, 12):
        d = np.array([math.sin(math.radians(psi)), -math.cos(math.radians(psi)), -0.1])
        root = skull_point(d) + skull_normal(skull_point(d)) * 0.5
        start = np.array([root, root + np.array([0.0, 0.0, -0.6]) + skull_normal(root) * 0.2])
        b.card(hang(start, 30.0, outward=1.0), 3.0, 2 + int(psi > 0), f"curtain{psi:+.0f}", double_sided=True)
    # Outer layer: starts over the crown, 1.8 above the scalp, combed back then down.
    for phi in np.linspace(-60, 60, 7):
        path = scalp_path(phi, 95.0, 185.0, 1.8, 20)
        b.card(hang(path, 26.0, outward=1.5), 3.2, 4, f"outer{phi:+.0f}", flip_v=phi < -30)
    # Under-layer: starts below the head, just inside the curtain, nowhere near the scalp.
    for psi in np.linspace(-90, 90, 6):
        d = np.array([math.sin(math.radians(psi)), -math.cos(math.radians(psi)), -0.1])
        root = skull_point(d) + skull_normal(skull_point(d)) * 0.5
        top = hang(np.array([root, root + np.array([0.0, 0.0, -0.6]) + skull_normal(root) * 0.2]), 10.0, outward=1.0)[-1]
        inward = np.array([top[0] - SKULL_CENTRE[0], top[1] - SKULL_CENTRE[1], 0.0])
        inward /= -np.linalg.norm(inward)
        start = top + inward * 0.8
        b.card(hang(np.array([start, start + np.array([0.0, 0.0, -0.6])]), 22.0, outward=0.0), 2.6, 7, f"under{psi:+.0f}")
    # Side locks from the temples.
    for side in (-1, 1):
        for k, elev in enumerate((15.0, 5.0)):
            az = math.radians(70.0 + 15.0 * k)
            d = np.array([side * math.sin(az), math.cos(az), math.sin(math.radians(elev))])
            root = skull_point(d) + skull_normal(skull_point(d)) * 0.4
            b.card(hang(np.array([root - np.array([0, 0.3, -0.3]), root]), 18.0, outward=0.6), 2.2, 5, f"side{side:+d}{k}")
    # Fringe: from the hairline down over the forehead.
    for phi in (-25.0, 0.0, 25.0):
        b.card(scalp_path(phi, 45.0, -5.0, 0.6, 16), 2.4, 6, f"fringe{phi:+.0f}")
    # A stray piece floating off the side of the head (a ribbon end, a mesh error): not hair from the head.
    b.card(np.array([[12.0, 0.0, 10.0], [12.5, 0.0, 7.0]]), 1.5, 1, "stray")
    alpha, shade = b.texture(gaps=(7, 2))
    return b, alpha, shade


def ponytail(seed=2):
    """Hair combed back into a tie behind the head, and a tail hanging from the tie."""
    b = Builder(seed)
    tie = skull_point(np.array([0.0, -1.0, -0.2])) + np.array([0.0, -1.6, 0.0])
    for phi in np.linspace(-80, 80, 15):
        front = 35.0 + abs(phi) * 0.35
        path = scalp_path(phi, front, 175.0, 0.25, 30)
        # Gather into the tie over the last stretch.
        k = len(path)
        w = np.clip((np.arange(k) - 0.55 * k) / (0.45 * k), 0.0, 1.0)[:, None] ** 1.5
        path = path * (1.0 - w) + tie * w
        b.card(path, 2.6, int(abs(phi) // 30) % 2, f"cap{phi:+.0f}", flip_v=phi > 50)
    for k in range(9):
        a = 2.0 * math.pi * k / 9.0
        start = tie + np.array([0.7 * math.cos(a), 0.7 * math.sin(a) - 0.4, -0.3])
        path = np.array([start, start + np.array([0.0, -0.4, -0.6])])
        b.card(hang(path, 24.0, outward=0.5), 1.6, 3, f"tail{k}", across_hint=(-math.sin(a), math.cos(a), 0.0), double_sided=k % 2 == 0)
    alpha, shade = b.texture()
    return b, alpha, shade


def bob(seed=3):
    """Chin-length hair: a cap and short cards round the back and sides."""
    b = Builder(seed)
    cap(b, np.linspace(-80, 80, 15), 200.0)
    for psi in np.linspace(-150, 150, 16):
        d = np.array([math.sin(math.radians(psi)), -math.cos(math.radians(psi)), 0.05])
        root = skull_point(d) + skull_normal(skull_point(d)) * 0.6
        b.card(hang(np.array([root, root + np.array([0.0, 0.0, -0.5]) + skull_normal(root) * 0.2]), 9.0, outward=0.8),
               2.8, 2 + int(psi > 0), f"bob{psi:+.0f}", double_sided=True)
    alpha, shade = b.texture()
    return b, alpha, shade


def buzz(seed=4):
    """A buzz cut: small cards tiled flat over the scalp, under a unit long."""
    b = Builder(seed)
    for phi in np.linspace(-80, 80, 17):
        front = 35.0 + abs(phi) * 0.35
        for theta in np.arange(front, 200.0, 7.0):
            b.card(scalp_path(phi, theta, theta + 7.5, 0.15, 6), 1.0, int(abs(phi) // 30) % 2, f"buzz{phi:+.0f}/{theta:.0f}")
    alpha, shade = b.texture()
    return b, alpha, shade


STYLES = {"layered_long": layered_long, "ponytail": ponytail, "bob": bob, "buzz": buzz}
