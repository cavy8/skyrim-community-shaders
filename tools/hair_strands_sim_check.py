"""Checks the Hair Strands guide solver on a NumPy port of StrandSim.cs.hlsl (TressFX 4.1).

The port runs TressFX's simulation pass per 1/60 s step, on the clock StrandRenderer::BeginFrame
keeps, with the targets interpolated across each frame and the drawn offsets interpolated
between the last two steps, as the shader does. Locks hang from a head that stays still, falls
off a cliff, sprints and stops, turns, bows and turns into a shoulder capsule, at 30 to 240 fps.

Checked: hair at rest stays near its style and comes to rest (also on a tilted head or one on
its side, where earlier solvers fluttered or never settled); the motion is the same at every
frame rate; long hair streams up in a fall and trails when running; everything settles
afterwards, and once the head stops a swing dies down as a pendulum's does rather than running
up and down the strand; short scalp locks keep their shape through one step aside or a small
turn; points stay out of the capsule core, and a fringe out of the head (an ellipsoid head
field) while walking and sprinting; followers stray no further than their guide and keep their
length; wind moves hair without blowing it apart; extreme settings stay finite and bounded.

Keep the port in step with features/Hair Strands/Shaders/HairStrands/StrandSim.cs.hlsl,
StrandSkin.cs.hlsl, StrandRenderer::BeginFrame and PrepareSimulation, and the presets with
Strands::MakePresetStyle. Exit code 1 on failure.

    python tools/hair_strands_sim_check.py
"""

import math
import sys
from multiprocessing import Pool

import numpy as np

STEP = 1.0 / 60.0  # kSimStep
MAX_FRAME = 0.05  # kMaxFrameTime
MAX_STEPS = 4  # kMaxSimSteps
TELEPORT_DISTANCE = 40.0  # kTeleportDistance
TELEPORT_SPEED = 4000.0  # kTeleportSpeed
WIND_MAGNITUDE = 125.0  # kWindMagnitude
WIND_CONE = math.radians(40.0)  # kWindConeAngle
CAPSULE_FRICTION = 0.4
MIN_COLLIDER_DEPTH = 0.5
MAX_STRETCH = 1.2  # StrandSim's MaxStretch
SHORT_STRAND_LENGTH = 10.0  # StrandSim's ShortStrandLength
SKYRIM_GRAVITY = 686.7  # a falling character, units/s^2
# Body colliders (BodyField.h, HairStrandsSkin::CollideBody).
BODY_COLUMNS = 32  # kBodyFieldColumns
BODY_CAP_ROWS = 6  # kBodyFieldCapRows
BODY_SIDE_ROWS = 12  # kBodyFieldSideRows
BODY_ROWS = 2 * BODY_CAP_ROWS + BODY_SIDE_ROWS
BODY_SAMPLE_SPACING = 0.5  # kBodyFieldSampleSpacing
BODY_LAYER_GAP = 0.6  # kBodyFieldLayerGap
BODY_MIN_WEIGHT = 0.05  # kBodyFieldMinWeight
BODY_FILL_PASSES = 3  # kBodyFieldFillPasses
BODY_MARGIN = 0.35  # kBodyFieldMargin
BODY_MAX_RADIUS = 40.0  # kBodyFieldMaxRadius
BODY_REST_DEPTH = 0.25  # HairStrandsSkin::BodyRestDepth
BODY_SLIDE = 0.4  # HairStrandsSim::BodySlide, TressFX's capsule friction share

# Strands::StrandStyle motion defaults and MakePresetStyle's changes to them.
DEFAULT = dict(vsp=0.4, vsp_threshold=1.208, local=0.908, local_iterations=3, global_stiffness=0.408, global_range=0.4,
               length_iterations=10, damping=0.068, gravity=100.0, tip_separation=0.0, clamp=20.0, wind=1.0)
PRESETS = {
    "straight": dict(DEFAULT),
    "wavy": dict(DEFAULT, local=0.93, damping=0.075),
    "curly": dict(DEFAULT, vsp=0.5, local=0.95, local_iterations=4, global_stiffness=0.45, global_range=0.5, damping=0.08, gravity=75.0),
    "coily": dict(DEFAULT, vsp=0.7, local=0.95, local_iterations=4, global_stiffness=0.6, global_range=0.8, damping=0.15, gravity=50.0, wind=0.4),
    "locs": dict(DEFAULT, vsp=0.3, local=0.85, global_range=0.3, length_iterations=12, gravity=150.0, wind=0.6),
}


def safe_normalize(v, fallback):
    lsq = v @ v
    return v / math.sqrt(lsq) if lsq > 1e-12 else fallback


def quat_from_two_unit_vectors(u, v):
    r = 1.0 + u @ v
    if r < 1e-7:
        r = 0.0
        n = np.array([-u[1], u[0], 0.0]) if abs(u[0]) > abs(u[2]) else np.array([0.0, -u[2], u[1]])
    else:
        n = np.cross(u, v)
    q = np.array([n[0], n[1], n[2], r])
    lsq = q @ q
    if lsq < 1e-10:
        return np.array([q[0], q[1], q[2], 1.0])
    return q / math.sqrt(lsq)


def rotate(q, v):
    """MultQuaternionAndVector, for one vector or rows of vectors."""
    uv = np.cross(q[:3], v)
    uuv = np.cross(q[:3], uv)
    return v + uv * (2.0 * q[3]) + uuv * 2.0


def shortest_arc(a, b):
    """HairStrandsSkin::ShortestArc."""
    d = a @ b
    if d < -0.9999:
        axis = np.cross(a, [1.0, 0, 0]) if abs(a[0]) < 0.9 else np.cross(a, [0, 1.0, 0])
        return np.array([*(axis / np.linalg.norm(axis)), 0.0])
    q = np.array([*np.cross(a, b), 1.0 + d])
    return q / np.linalg.norm(q)


def closest_on_segment(p, a, b):
    ab = b - a
    lsq = ab @ ab
    return a + ab * (np.clip(((p - a) @ ab) / lsq, 0.0, 1.0) if lsq > 1e-8 else 0.0)


def capsule_collision(p, old, a, b, radius):
    """TressFX CapsuleCollision with one radius; a == b is a sphere. Returns (hit, position)."""
    up = np.array([0, 0, 1.0])
    segment = b - a
    delta0, delta1 = p - a, b - p
    dist0, dist1 = delta0 @ segment, delta1 @ segment
    if dist0 < 0.0 or segment @ segment < 1e-8:
        if delta0 @ delta0 < radius * radius:
            return True, a + radius * safe_normalize(delta0, up)
        return False, p
    if dist1 < 0.0:
        if delta1 @ delta1 < radius * radius:
            return True, b + radius * safe_normalize(-delta1, up)
        return False, p
    x = (dist0 * b + dist1 * a) / (dist0 + dist1)
    delta = p - x
    if delta @ delta < radius * radius:
        n = safe_normalize(delta, up)
        vec = p - old
        seg_n = segment / math.sqrt(segment @ segment)
        tangent = (vec @ seg_n) * seg_n
        return True, old + CAPSULE_FRICTION * tangent + (vec - tangent + radius * n - delta)
    return False, p


class HeadField:
    """The head field (StrandRenderer::BuildHeadField, HairStrandsSkin::CollideHead) of an
    ellipsoid head: per direction from the skull centre, the distance to the head's surface."""

    def __init__(self, centre=(0.0, 0.0, 110.0), axes=(8.0, 10.0, 10.0), offset=(0.0, 1.0, -1.0)):
        self.centre, self.axes, self.offset = np.array(centre), np.array(axes), np.array(offset)

    def surface(self, d):
        o = -self.offset
        a = np.sum((d / self.axes) ** 2)
        b = 2.0 * np.sum(d * o / self.axes ** 2)
        c = np.sum((o / self.axes) ** 2) - 1.0
        return (-b + math.sqrt(b * b - 4.0 * a * c)) / (2.0 * a)

    def local(self, p, head):
        return np.linalg.solve(head[:, :3], p - head[:, 3]) - self.centre

    def depth(self, p, head):
        q = self.local(p, head)
        distance = np.linalg.norm(q)
        return max(self.surface(q / distance) - distance, 0.0) if distance > 1e-6 else 0.0

    def collide(self, p, target_depth, head):
        q = self.local(p, head)
        distance = np.linalg.norm(q)
        direction = safe_normalize(q, np.array([0, 0, 1.0]))
        surface = self.surface(direction)
        allowed = max(surface - target_depth, surface * MIN_COLLIDER_DEPTH)
        if distance >= allowed:
            return p
        return head[:, 3] + head[:, :3] @ (self.centre + direction * allowed)


def body_field_axes(direction):
    """BodyField's field axes (columns): Z along the segment, X and Y a perpendicular pair."""
    z = direction / np.linalg.norm(direction)
    x = np.cross(z, [0.0, 0.0, 1.0] if abs(z[2]) < 0.9 else [1.0, 0.0, 0.0])
    x /= np.linalg.norm(x)
    return np.c_[x, np.cross(z, x), z]


def body_field_texel(q, length):
    """BodyFieldTexel / HairStrandsSkin::BodyFieldTexel: continuous (column, row) of field-space
    point q round the segment (0, 0, 0)-(0, 0, length). Rows run from the pole of the cap at the
    start, along the segment, to the pole of the cap at the end; columns round the segment."""
    rho = math.hypot(q[0], q[1])
    if q[2] < 0.0:
        row = math.atan2(rho, -q[2]) * (BODY_CAP_ROWS / (math.pi / 2))
    elif q[2] > length:
        row = BODY_ROWS - math.atan2(rho, q[2] - length) * (BODY_CAP_ROWS / (math.pi / 2))
    else:
        row = BODY_CAP_ROWS + BODY_SIDE_ROWS * q[2] / max(length, 1e-4)
    return (math.atan2(q[1], q[0]) / (2.0 * math.pi) + 1.0) * BODY_COLUMNS, row


class BodyCollider:
    """One body collider (StrandRenderer's body field, HairStrandsSkin::CollideBody): a capsule whose
    radius varies with direction, rigid on its bone, built from the body's own triangles."""

    def __init__(self, start, end):
        self.origin = np.asarray(start, float)
        self.axes = body_field_axes(np.asarray(end, float) - self.origin)
        self.length = float(np.linalg.norm(np.asarray(end, float) - self.origin))
        self.map = np.zeros((BODY_ROWS, BODY_COLUMNS))
        self.bound = 0.0
        self.samples = []

    def splat(self, vertices, triangles):
        """BuildBodyField's sampling: points no further apart than BODY_SAMPLE_SPACING over every triangle."""
        for tri in triangles:
            a, b, c = (self.axes.T @ (vertices[i] - self.origin) for i in tri)
            ab, ac = b - a, c - a
            longest = max(np.linalg.norm(ab), np.linalg.norm(ac), np.linalg.norm(c - b))
            steps = min(max(int(math.ceil(longest / BODY_SAMPLE_SPACING)), 1), 64)
            i, j = np.meshgrid(np.arange(steps + 1), np.arange(steps + 1), indexing="ij")
            keep = i + j <= steps
            for q in a + np.outer(i[keep] / steps, ab) + np.outer(j[keep] / steps, ac):
                r = math.hypot(q[0], q[1]) if 0.0 <= q[2] <= self.length else np.linalg.norm(q - [0.0, 0.0, min(max(q[2], 0.0), self.length)])
                if 1e-3 < r <= BODY_MAX_RADIUS:
                    self.samples.append((*body_field_texel(q, self.length), r))

    def finish(self):
        """BuildBodyField's map: per texel the outermost layer of samples, averaged towards the texel
        centres with bilinear weights, then small holes filled from their neighbours."""
        per = {}
        for column, row, r in self.samples:
            per.setdefault((min(max(int(row), 0), BODY_ROWS - 1), int(column) % BODY_COLUMNS), []).append(r)
        floor = np.full((BODY_ROWS, BODY_COLUMNS), np.inf)
        for (y, x), radii in per.items():
            radii.sort(reverse=True)
            lowest = radii[0]
            for k in range(1, len(radii)):
                if radii[k - 1] - radii[k] > BODY_LAYER_GAP:
                    break
                lowest = radii[k]
            floor[y, x] = lowest
        # A plane fitted per texel to the outer-layer samples round its centre (bilinear weights), read
        # at the centre: a plain weighted mean leans towards wherever samples crowd, low on steep
        # slopes. Clamped to the samples' own range.
        sums = np.zeros((BODY_ROWS, BODY_COLUMNS, 9))  # w, w dx, w dy, w dx2, w dx dy, w dy2, w r, w r dx, w r dy
        low = np.full((BODY_ROWS, BODY_COLUMNS), np.inf)
        high = np.zeros((BODY_ROWS, BODY_COLUMNS))
        for column, row, r in self.samples:
            if r < floor[min(max(int(row), 0), BODY_ROWS - 1), int(column) % BODY_COLUMNS]:
                continue
            x, y = column - 0.5, row - 0.5
            bx, by = int(math.floor(x)), int(math.floor(y))
            fx, fy = x - bx, y - by
            for ty, wy, dy in ((by, 1.0 - fy, fy), (by + 1, fy, fy - 1.0)):
                if 0 <= ty < BODY_ROWS:
                    for tx, wx, dx in ((bx % BODY_COLUMNS, 1.0 - fx, fx), ((bx + 1) % BODY_COLUMNS, fx, fx - 1.0)):
                        w = wx * wy
                        sums[ty, tx] += w * np.array([1.0, dx, dy, dx * dx, dx * dy, dy * dy, r, r * dx, r * dy])
                        low[ty, tx], high[ty, tx] = min(low[ty, tx], r), max(high[ty, tx], r)
        field = np.zeros((BODY_ROWS, BODY_COLUMNS))
        for y in range(BODY_ROWS):
            for x in range(BODY_COLUMNS):
                w, wx, wy, wxx, wxy, wyy, wr, wrx, wry = sums[y, x]
                if w <= BODY_MIN_WEIGHT:
                    continue
                value = wr / w
                a = np.array([[w, wx, wy], [wx, wxx, wxy], [wy, wxy, wyy]])
                if abs(np.linalg.det(a)) > 1e-6 * w * w * w:
                    value = np.linalg.solve(a, [wr, wrx, wry])[0]
                field[y, x] = min(max(value, low[y, x]), high[y, x])
        for _ in range(BODY_FILL_PASSES):
            filled = field.copy()
            for y in range(BODY_ROWS):
                for x in range(BODY_COLUMNS):
                    if field[y, x] > 0.0:
                        continue
                    values = [field[ny, (x + dx) % BODY_COLUMNS] for ny in range(max(y - 1, 0), min(y + 1, BODY_ROWS - 1) + 1)
                              for dx in (-1, 0, 1) if field[ny, (x + dx) % BODY_COLUMNS] > 0.0]
                    if len(values) >= 4:
                        filled[y, x] = sum(values) / len(values)
            field = filled
        field[field > 0.0] += BODY_MARGIN
        # Slopes per texel (central differences over filled neighbours), interpolated like the radius
        # so the surface normal turns smoothly: the bilinear patch's own slopes jump at every texel
        # edge, and a point resting across one was pushed back and forth.
        slopes = np.zeros((BODY_ROWS, BODY_COLUMNS, 2))
        for y in range(BODY_ROWS):
            for x in range(BODY_COLUMNS):
                if field[y, x] <= 0.0:
                    continue
                for axis, (before, after) in enumerate((((y, (x - 1) % BODY_COLUMNS), (y, (x + 1) % BODY_COLUMNS)), ((y - 1, x), (y + 1, x)))):
                    have = [0 <= t[0] < BODY_ROWS and field[t] > 0.0 for t in (before, after)]
                    if have[0] and have[1]:
                        slopes[y, x, axis] = 0.5 * (field[after] - field[before])
                    elif have[0]:
                        slopes[y, x, axis] = field[y, x] - field[before]
                    elif have[1]:
                        slopes[y, x, axis] = field[after] - field[y, x]
        self.map, self.slopes, self.bound, self.samples = field, slopes, field.max(), []

    def surface(self, q):
        """HairStrandsSkin::BodySurface: the radius and its slopes along the column and the row, each
        bilinear, wrapping round the segment."""
        column, row = body_field_texel(q, self.length)
        x, y = column - 0.5, row - 0.5
        bx, by = int(math.floor(x)), int(math.floor(y))
        fx, fy = x - bx, y - by
        c0, c1 = bx % BODY_COLUMNS, (bx + 1) % BODY_COLUMNS
        r0, r1 = min(max(by, 0), BODY_ROWS - 1), min(max(by + 1, 0), BODY_ROWS - 1)
        weights = ((r0, c0, (1.0 - fx) * (1.0 - fy)), (r0, c1, fx * (1.0 - fy)), (r1, c0, (1.0 - fx) * fy), (r1, c1, fx * fy))
        radius = sum(self.map[r, c] * w for r, c, w in weights)
        slope = sum(self.slopes[r, c] * w for r, c, w in weights)
        return radius, slope[0], slope[1]

    def frame(self, body):
        """Field to world (linear part, translation) on a body pose, and world to field as the shaders
        invert it: the transpose over the squared scale."""
        m = body[:, :3] @ self.axes
        t = body[:, :3] @ self.origin + body[:, 3]
        return m, t, m.T / (m[0] @ m[0])

    def depth_and_normal(self, q, allowed_of=None):
        """HairStrandsSkin::BodyDepth: how far field-space point q lies inside (the surface less
        allowed_of(surface), first order: over the gradient's length) and the outward normal there.
        The surface is a radius per direction from the segment (round its side, from its ends over
        the caps); its normal comes from the bilinear patch's slopes."""
        closest = np.array([0.0, 0.0, min(max(q[2], 0.0), self.length)])
        d = q - closest
        radius = np.linalg.norm(d)
        if radius >= self.bound:
            return 0.0, None, 0.0
        surface, ds_column, ds_row = self.surface(q)
        if not surface > 0.0:
            return 0.0, None, 0.0
        theta = math.atan2(q[1], q[0])
        around = np.array([-math.sin(theta), math.cos(theta), 0.0])
        ds_theta = ds_column * BODY_COLUMNS / (2.0 * math.pi)
        if 0.0 <= q[2] <= self.length:
            outward = np.array([math.cos(theta), math.sin(theta), 0.0])
            gradient = outward - around * (ds_theta / surface) - np.array([0.0, 0.0, 1.0]) * (ds_row * BODY_SIDE_ROWS / max(self.length, 1e-4))
        else:
            start = q[2] < 0.0
            rho = math.hypot(q[0], q[1])
            phi = math.atan2(rho, -q[2] if start else q[2] - self.length)  # from the cap's pole
            ds_phi = ds_row * BODY_CAP_ROWS / (math.pi / 2) * (1.0 if start else -1.0)
            sin_phi, cos_phi = math.sin(phi), math.cos(phi)
            pole = -1.0 if start else 1.0
            outward = np.array([sin_phi * math.cos(theta), sin_phi * math.sin(theta), pole * cos_phi])
            down = np.array([cos_phi * math.cos(theta), cos_phi * math.sin(theta), -pole * sin_phi])  # increasing phi
            gradient = outward - down * (ds_phi / surface) - around * (ds_theta / (surface * max(sin_phi, 0.25)))
        allowed = surface - (allowed_of(surface) if allowed_of else 0.0)
        length = max(np.linalg.norm(gradient), 1e-6)
        return (allowed - radius) / length, gradient / length, surface - radius

    def collide(self, p, target, body):
        """HairStrandsSkin::CollideBody, as TressFX's signed distance field collision: a point inside
        is put back on the surface along its normal. A styled shape up to BODY_REST_DEPTH inside
        rests as styled; deeper, it rests at that depth. Returns (hit, position, world normal,
        field-space position)."""
        m, t, inverse = self.frame(body)
        q = inverse @ (p - t)
        target_depth = max(self.depth_and_normal(inverse @ (target - t))[2], 0.0)
        rest = min(target_depth, BODY_REST_DEPTH)
        depth, normal, _ = self.depth_and_normal(q, lambda _: rest)
        if normal is None or depth <= 0.0:
            return False, p, None, None
        q = q + normal * depth
        world_normal = m @ normal
        return True, m @ q + t, world_normal / np.linalg.norm(world_normal), q


def collide_body(points, targets, body, first=2):
    """StrandSkin.cs.hlsl's body collision of drawn points, from the third point of the strand."""
    out = points.copy()
    for i in range(first, len(out)):
        for collider in body_colliders():
            out[i] = collider.collide(out[i], targets[i], body)[1]
    return out



def ellipsoid_mesh(centre, radii, spacing=0.8):
    rmax = max(radii)
    n_lat, n_lon = max(int(math.pi * rmax / spacing), 8), max(int(2.0 * math.pi * rmax / spacing), 12)
    vertices = [np.asarray(centre) + np.asarray(radii) * [math.sin(math.pi * i / n_lat) * math.cos(2 * math.pi * j / n_lon),
                                                          math.sin(math.pi * i / n_lat) * math.sin(2 * math.pi * j / n_lon), math.cos(math.pi * i / n_lat)]
                for i in range(n_lat + 1) for j in range(n_lon)]
    triangles = []
    for i in range(n_lat):
        for j in range(n_lon):
            a, b = i * n_lon + j, i * n_lon + (j + 1) % n_lon
            triangles += [(a, a + n_lon, b), (b, a + n_lon, b + n_lon)]
    return np.array(vertices), triangles


def capsule_mesh(a, b, radius, spacing=0.8):
    a, b = np.asarray(a, float), np.asarray(b, float)
    axes, length = body_field_axes(b - a), np.linalg.norm(b - a)
    n_lon, n_cap, n_side = max(int(2.0 * math.pi * radius / spacing), 12), max(int(math.pi / 2 * radius / spacing), 3), max(int(length / spacing), 1)
    rings = [(-radius * math.cos(math.pi / 2 * i / n_cap), radius * math.sin(math.pi / 2 * i / n_cap)) for i in range(1, n_cap + 1)]
    rings += [(length * i / n_side, radius) for i in range(n_side + 1)]
    rings += [(length + radius * math.cos(math.pi / 2 * (n_cap - i) / n_cap), radius * math.sin(math.pi / 2 * (n_cap - i) / n_cap)) for i in range(1, n_cap)]
    vertices = [a + axes @ [0.0, 0.0, -radius]]
    vertices += [a + axes @ [r * math.cos(2 * math.pi * j / n_lon), r * math.sin(2 * math.pi * j / n_lon), z] for z, r in rings for j in range(n_lon)]
    vertices.append(a + axes @ [0.0, 0.0, length + radius])
    triangles = [(0, 1 + (j + 1) % n_lon, 1 + j) for j in range(n_lon)]
    for i in range(len(rings) - 1):
        for j in range(n_lon):
            p, q = 1 + i * n_lon + j, 1 + i * n_lon + (j + 1) % n_lon
            triangles += [(p, q, p + n_lon), (q, q + n_lon, p + n_lon)]
    last = 1 + (len(rings) - 1) * n_lon
    triangles += [(last + j, last + (j + 1) % n_lon, len(vertices) - 1) for j in range(n_lon)]
    return np.array(vertices), triangles


# A body under the head (+Y forward, +Z up): each collider's bone segment, and the closed part
# skinned to that bone alone. Parts overlap, as a body's bone regions do.
BODY_PARTS = {
    "neck": (((0, -1.5, 96.0), (0, -1.0, 104.0)), ("capsule", ((0, -1.5, 88.0), (0, -1.0, 103.0), 4.0))),
    "chest": (((0, -2.0, 82.0), (0, -1.5, 96.0)), ("ellipsoid", ((0, -1.0, 81.0), (15.0, 8.5, 11.0)))),
    "back": (((0, -2.0, 70.0), (0, -2.0, 82.0)), ("ellipsoid", ((0, -1.0, 70.0), (13.5, 8.0, 9.0)))),
    "waist": (((0, -2.0, 58.0), (0, -2.0, 70.0)), ("ellipsoid", ((0, -1.0, 58.0), (12.0, 7.5, 8.0)))),
    "left shoulder": (((2.0, -1.5, 91.0), (15.5, -2.0, 88.0)), ("capsule", ((6.0, -1.5, 90.0), (15.5, -2.0, 88.0), 4.0))),
    "right shoulder": (((-2.0, -1.5, 91.0), (-15.5, -2.0, 88.0)), ("capsule", ((-6.0, -1.5, 90.0), (-15.5, -2.0, 88.0), 4.0))),
    "left arm": (((15.5, -2.0, 88.0), (17.5, -2.0, 70.0)), ("capsule", ((15.5, -2.0, 88.0), (17.5, -2.0, 70.0), 3.8))),
    "right arm": (((-15.5, -2.0, 88.0), (-17.5, -2.0, 70.0)), ("capsule", ((-15.5, -2.0, 88.0), (-17.5, -2.0, 70.0), 3.8))),
}


def part_distance(p, kind, params):
    """Signed distance to a body part (+ outside; the ellipsoid's is the usual approximation)."""
    if kind == "ellipsoid":
        centre, radii = np.asarray(params[0]), np.asarray(params[1])
        k0, k1 = np.linalg.norm((p - centre) / radii), np.linalg.norm((p - centre) / (radii * radii))
        return k0 * (k0 - 1.0) / k1 if k1 > 1e-9 else -min(radii)
    return np.linalg.norm(p - closest_on_segment(p, np.asarray(params[0]), np.asarray(params[1]))) - params[2]


def body_depth(p, body):
    """How far world point p lies inside the true body on pose body (0 outside)."""
    local = np.linalg.solve(body[:, :3], p - body[:, 3])
    return max(0.0, max(-part_distance(local, *part) for _, part in BODY_PARTS.values()))


_BODY = None


def body_colliders():
    """The body's colliders, built as BuildBodyField builds them (once per process: a few seconds)."""
    global _BODY
    if _BODY is None:
        _BODY = []
        for segment, (kind, params) in BODY_PARTS.values():
            collider = BodyCollider(*segment)
            collider.splat(*(ellipsoid_mesh(*params) if kind == "ellipsoid" else capsule_mesh(*params)))
            collider.finish()
            _BODY.append(collider)
    return _BODY


class Clock:
    """StrandRenderer::BeginFrame's simulation clock and weather wind."""

    def __init__(self):
        self.accumulator = 0.0
        self.step = 0

    def frame(self, dt, wind_speed=0.0, wind_angle=0.0):
        dt = min(max(dt, 0.0), MAX_FRAME)
        elapsed = self.accumulator + dt
        steps = min(int(elapsed / STEP + 1e-4), MAX_STEPS)
        first = (STEP - self.accumulator) / dt if dt > 0 else 0.0
        fraction = STEP / dt if dt > 0 else 0.0
        self.accumulator = min(max(elapsed - steps * STEP, 0.0), STEP * 0.999)
        self.step += steps
        corners = np.zeros((4, 3))
        if wind_speed > 0.0:
            magnitude = min(wind_speed, 1.0) * WIND_MAGNITUDE * (math.sin(self.step * 0.01) ** 2 + 0.5)
            c, s = math.cos(WIND_CONE), math.sin(WIND_CONE)
            heading = math.pi / 2 - wind_angle
            ch, sh = math.cos(heading), math.sin(heading)
            for k, v in enumerate(((c, 0, -s), (c, 0, s), (c, s, 0), (c, -s, 0))):
                corners[k] = np.array([v[0] * ch - v[1] * sh, v[0] * sh + v[1] * ch, v[2]]) * magnitude
        return dict(steps=steps, first=first, fraction=fraction, alpha=self.accumulator / STEP, dt=dt, wind=corners)


class Guide:
    """One guide strand of StrandSim.cs.hlsl. rest: points in skin space; index picks its wind mix."""

    def __init__(self, rest, index=0):
        self.rest = np.c_[rest, np.ones(len(rest))]
        self.index = index
        self.position = None
        self.length = np.linalg.norm(np.diff(rest, axis=0), axis=1).sum()  # StrandInfo.Length

    def frame(self, head, head_previous, clock, style, colliders=(), reset=False, head_field=None, body=None, body_previous=None):
        """body, body_previous: the body's pose this frame and last, for the body colliders (none without)."""
        n = len(self.rest)
        target_end = self.rest @ head.T
        target_start = self.rest @ head_previous.T
        teleport = max(TELEPORT_DISTANCE, TELEPORT_SPEED * clock["dt"])
        if reset or self.position is None or not np.linalg.norm(target_end[0] - self.position[0]) <= teleport:
            self.position, self.previous = target_end.copy(), target_end.copy()
            self.previous_previous1 = target_end[1].copy()
            self.step_offset = np.zeros_like(target_end)
            self.previous_step_offset = np.zeros_like(target_end)
            self.offset = np.zeros_like(target_end)
            self.previous_offset = self.offset
            self.target = target_end
            return

        h = STEP
        decay = math.exp(-style["damping"] * h * 60.0)
        gravity = np.array([0.0, 0.0, -style["gravity"]]) * h * h
        local = 0.5 * min(style["local"], 0.95)
        # A strand shorter than SHORT_STRAND_LENGTH: the global range of one that long, VSP by its length.
        length_scale = min(max(self.length / SHORT_STRAND_LENGTH, 0.0), 1.0)
        global_count = style["global_range"] * n / max(length_scale, 1e-4)
        a = (self.index % 20) / 20.0
        w = clock["wind"] * style["wind"]
        wind = a * w[0] + (1.0 - a) * w[1] + a * w[2] + (1.0 - a) * w[3]
        vsp_threshold = style["vsp_threshold"]
        clamp = style["clamp"]
        position, previous, pp1 = self.position, self.previous, self.previous_previous1
        movable0 = np.arange(n - 1) >= 2
        m0 = np.where(movable0, 0.5, 0.0)
        m1 = np.where(movable0, 0.5, np.where(np.arange(1, n) >= 2, 1.0, 0.0))

        for step in range(clock["steps"]):
            f = min(max(clock["first"] + step * clock["fraction"], 0.0), 1.0)
            t = target_start + (target_end - target_start) * f

            # IntegrationAndGlobalShapeConstraints.
            new = t.copy()
            new[2:] = position[2:] + decay * (position[2:] - previous[2:]) + gravity
            if style["global_stiffness"] > 0.0:
                held = np.arange(n) < global_count
                held[:2] = False
                new[held] += style["global_stiffness"] * (t[held] - new[held])
            pp1 = previous[1].copy()
            previous, position = position.copy(), new

            # CalculateStrandLevelData, VelocityShockPropagation.
            u = safe_normalize(previous[1] - previous[0], np.array([0, 0, -1.0]))
            v = safe_normalize(position[1] - position[0], u)
            q = quat_from_two_unit_vectors(u, v)
            translation = position[0] - rotate(q, previous[0])
            vsp = 1.0 if np.linalg.norm(position[1] - 2.0 * previous[1] + pp1) > vsp_threshold else style["vsp"] * length_scale
            position[2:] += (rotate(q, position[2:]) + translation - position[2:]) * vsp
            previous[2:] += (rotate(q, previous[2:]) + translation - previous[2:]) * vsp

            # LocalShapeConstraints.
            for _ in range(style["local_iterations"]):
                for i in range(1, n - 1):
                    last_bind = safe_normalize(t[i] - t[i - 1], np.array([0, 0, -1.0]))
                    last = safe_normalize(position[i] - position[i - 1], last_bind)
                    org = rotate(quat_from_two_unit_vectors(last_bind, last), t[i + 1] - t[i]) + position[i]
                    d = local * (org - position[i + 1])
                    if i >= 2:
                        position[i] = position[i] - d
                    position[i + 1] = position[i + 1] + d

            # Wind (on each segment's direction at its rest length), then length constraints in even
            # and odd pairs (disjoint, so vectorised), then no segment longer than MAX_STRETCH.
            rest = np.linalg.norm(np.diff(t, axis=0), axis=1)
            if np.any(wind != 0.0):
                segment = position[2:n - 1] - position[3:n]
                segment *= (rest[2:n - 1] / np.maximum(np.linalg.norm(segment, axis=1), 1e-12))[:, None]
                position[2:n - 1] += -np.cross(np.cross(segment, wind), segment) * h * h
            for _ in range(style["length_iterations"]):
                for parity in (0, 1):
                    j = np.arange(parity, n - 1, 2)
                    delta = position[j + 1] - position[j]
                    distance = np.maximum(np.linalg.norm(delta, axis=1), 1e-7)
                    delta *= (1.0 - rest[j] / distance)[:, None]
                    position[j] += m0[j, None] * delta
                    position[j + 1] -= m1[j, None] * delta
            for i in range(2, n):
                segment = position[i] - position[i - 1]
                length = np.linalg.norm(segment)
                if length > MAX_STRETCH * rest[i - 1]:
                    position[i] = position[i - 1] + segment * (MAX_STRETCH * rest[i - 1] / length)

            # Collision, then the position delta clamp, on the movable points. The body colliders ride
            # the body's pose at the step, as the targets do; body_before is its pose a step earlier.
            body_at = None if body is None else body_previous + (body - body_previous) * f
            body_before = None if body is None else body_previous + (body - body_previous) * (f - clock["fraction"])
            for i in range(2, n):
                collided = False
                for ca, cb, radius in colliders:
                    target_distance = np.linalg.norm(t[i] - closest_on_segment(t[i], ca, cb))
                    allowed = max(min(radius, target_distance), radius * MIN_COLLIDER_DEPTH)
                    hit, pushed = capsule_collision(position[i], previous[i], ca, cb, allowed)
                    if hit:
                        position[i], collided = pushed, True
                contact = None
                if body_at is not None:
                    for collider in body_colliders():
                        hit, pushed, normal, q = collider.collide(position[i], t[i], body_at)
                        if hit:
                            position[i] = pushed
                            m, tt, _ = collider.frame(body_before)
                            contact = (normal, pushed - (m @ q + tt))  # the surface's move over the step
                delta = position[i] - previous[i]
                speed_squared = delta @ delta
                if speed_squared > clamp * clamp:
                    previous[i] = position[i] - delta * (clamp * clamp / speed_squared)
                if collided:
                    previous[i] = position[i].copy()
                elif contact is not None:
                    # Resting on the body: the point moves on with the surface, keeping BODY_SLIDE of
                    # its slide along it and none of its motion into or off it.
                    normal, surface_move = contact
                    relative = position[i] - previous[i] - surface_move
                    relative -= normal * (relative @ normal)
                    previous[i] = position[i] - surface_move - BODY_SLIDE * relative

            # The head field, as TressFX's signed distance field collision, on the head's pose at the step.
            if head_field is not None:
                head_at = head_previous + (head - head_previous) * f
                for i in range(2, n):
                    pushed = head_field.collide(position[i], head_field.depth(t[i], head_at), head_at)
                    if np.any(pushed != position[i]):
                        position[i], previous[i] = pushed, pushed.copy()

            self.previous_step_offset, self.step_offset = self.step_offset, position - t

        self.position, self.previous, self.previous_previous1 = position, previous, pp1
        self.previous_offset = self.offset
        self.offset = self.previous_step_offset + (self.step_offset - self.previous_step_offset) * clock["alpha"]
        self.target = target_end

    @property
    def shown(self):
        return self.target + self.offset


def head_transform(translation=(0, 0, 0), pitch=0.0, yaw=0.0, pivot=None, roll=0.0):
    p, y, r = math.radians(pitch), math.radians(yaw), math.radians(roll)
    rx = np.array([[1, 0, 0], [0, math.cos(p), -math.sin(p)], [0, math.sin(p), math.cos(p)]])
    ry = np.array([[math.cos(r), 0, math.sin(r)], [0, 1, 0], [-math.sin(r), 0, math.cos(r)]])
    rz = np.array([[math.cos(y), -math.sin(y), 0], [math.sin(y), math.cos(y), 0], [0, 0, 1]])
    rotation = rz @ ry @ rx
    t = np.array(translation, dtype=float)
    if pivot is not None:
        t = t + np.asarray(pivot) - rotation @ np.asarray(pivot)
    return np.c_[rotation, t]


def still(_):
    return head_transform()


FALL_START, FALL_TIME = 0.5, 1.5


def cliff(s):
    """Stand 0.5 s, fall freely for 1.5 s, land."""
    u = min(max(s - FALL_START, 0.0), FALL_TIME)
    return head_transform((0, 0, -0.5 * SKYRIM_GRAVITY * u * u))


def sprint(s):
    """Up to 300 units/s along +Y in 0.3 s, run for 1 s, stop in 0.3 s."""
    if s < 0.3:
        y = 500.0 * s * s
    elif s < 1.3:
        y = 45.0 + 300.0 * (s - 0.3)
    elif s < 1.6:
        u = s - 1.3
        y = 345.0 + 300.0 * u - 500.0 * u * u
    else:
        y = 390.0
    return head_transform((0, y, 0))


def turn(s):
    return head_transform(yaw=70.0 * min(s / 0.25, 1.0))


def bow(s):
    angle = 60.0 * min(max(s / 0.5, 0.0), 1.0) - 60.0 * min(max((s - 2.0) / 0.5, 0.0), 1.0)
    return head_transform(pitch=angle, pivot=(0, 0, 110.0))


def idle(s):
    """Breathing and idle sway: a 0.6-unit bob and a 3 degree nod."""
    return head_transform((0, 0, 0.6 * math.sin(2 * math.pi * 0.4 * s)), pitch=3.0 * math.sin(2 * math.pi * 0.3 * s), pivot=(0, 0, 110.0))


def snap(s):
    """A third-person character snapping to the camera's heading: 90 degrees in 0.05 s, every second."""
    return head_transform(yaw=90.0 * (int(s) + min((s - int(s)) / 0.05, 1.0)))


def small_step(s):
    """One step aside: 15 units in 0.5 s from 0.2 s, then still."""
    u = min(max((s - 0.2) / 0.5, 0.0), 1.0)
    return head_transform((0, 15.0 * u * u * (3.0 - 2.0 * u), 0))


def small_turn(s):
    """A 15 degree turn in 0.25 s from 0.2 s, then still."""
    return head_transform(yaw=15.0 * min(max((s - 0.2) / 0.25, 0.0), 1.0))


def walk(s):
    """Walking forward (+Y) at 130 units/s after 0.3 s, with a 1.2-unit bob, surge and nod per step."""
    y = 130.0 * s - 19.5 if s > 0.3 else 216.7 * s * s
    return head_transform((0, y + math.sin(4 * math.pi * s), 1.2 * math.sin(4 * math.pi * s)), pitch=2.0 * math.sin(2 * math.pi * s), pivot=(0, 0, 100.0))


def hanging(length=20.0, points=20):
    """A lock hanging from the back of the head."""
    t = np.linspace(0.0, 1.0, points)
    return np.c_[np.zeros(points), -3.0 - 2.0 * np.sin(t * math.pi * 0.5), 120.0 - length * t]


def scalp_lock(points=14, length=3.8):
    """A short lock lying along the back of the skull, like vanilla hair cards."""
    angle = math.radians(40.0) + length / 9.0 * np.linspace(0.0, 1.0, points)
    return np.c_[np.zeros(points), -9.3 * np.sin(angle), 110.0 + 9.3 * np.cos(angle)]


def fringe(field, points=12, lift=0.3):
    """A lock over the forehead (+Y), from the hairline down to the brow, lift units off the head."""
    out = []
    for elevation in np.radians(np.linspace(60.0, 5.0, points)):
        d = np.array([0.0, math.cos(elevation), math.sin(elevation)])
        out.append(field.centre + d * (field.surface(d) + lift))
    return np.array(out)


def lock(length, offset, points=14):
    t = np.linspace(0.0, 1.0, points)
    return np.c_[np.full(points, offset[0]), offset[1] - 3.0 - 1.5 * np.sin(t * math.pi * 0.5), 112.0 + offset[2] - length * t]


def run(rest, style, fps, motion, seconds, colliders=(), wind_speed=0.0, jitter=0.0, head_field=None, body_motion=None):
    """Frames of (time, drawn points, targets, simulated points, body pose). jitter varies frame times
    by up to that fraction. body_motion gives the body's pose (body colliders on), else no body."""
    guide = Guide(rest)
    clock = Clock()
    rng = np.random.default_rng(1)
    previous = motion(0.0)
    body_previous = body_motion(0.0) if body_motion else None
    guide.frame(previous, previous, clock.frame(0.0), style)
    log, s = [], 0.0
    while s < seconds - 1e-9:
        dt = (1.0 / fps) * (1.0 + jitter * (rng.random() * 2.0 - 1.0))
        s += dt
        current = motion(s)
        body = body_motion(s) if body_motion else None
        guide.frame(current, previous, clock.frame(dt, wind_speed), style, colliders, head_field=head_field, body=body, body_previous=body_previous)
        previous, body_previous = current, body
        log.append((s, guide.shown.copy(), guide.target.copy(), guide.position.copy(), body))
    return log


def at(log, seconds):
    return min(log, key=lambda row: abs(row[0] - seconds))


def deviation(row):
    return np.linalg.norm(row[1] - row[2], axis=1).max()


def angle_from_down(row):
    """Degrees between the lock's root-to-tip chord and straight down: 0 hanging, 180 straight up."""
    chord = row[1][-1] - row[1][0]
    return math.degrees(math.acos(np.clip(-chord[2] / np.linalg.norm(chord), -1.0, 1.0)))


def settled_motion(log, last_seconds=1.0):
    """The largest move of any point between frames over the last seconds."""
    end = log[-1][0]
    rows = [r for r in log if r[0] >= end - last_seconds]
    return max(np.linalg.norm(b[1] - a[1], axis=1).max() for a, b in zip(rows, rows[1:]))


def stretch(row):
    drawn = np.linalg.norm(np.diff(row[1], axis=0), axis=1).sum()
    rest = np.linalg.norm(np.diff(row[2], axis=0), axis=1).sum()
    return drawn / rest


def follow(guide, guide_length, target, length, tip_separation, guide_target):
    """StrandSkin.cs.hlsl: a follower sits off the guide's drawn position by its rest offset from it."""
    n = len(guide.offset)
    along = np.clip(np.linspace(0.0, 1.0, len(target)) * length / guide_length, 0.0, 1.0) * (n - 1)
    j = np.minimum(along.astype(int), n - 2)
    w = (along - j)[:, None]
    followed = target + guide.offset[j] * (1.0 - w) + guide.offset[j + 1] * w
    if tip_separation > 0.0:
        guide_at = guide_target[j] * (1.0 - w) + guide_target[j + 1] * w
        followed += (tip_separation * np.linspace(0.0, 1.0, len(target)) * (n - 1) / n)[:, None] * (target - guide_at)
    return followed


# --- Checks, each returning (name, ok, detail) -----------------------------------------------


def check_rest(name):
    style = PRESETS[name]
    results = []
    for rest_name, rest in (("20-unit", hanging(20.0, 20)), ("40-unit", hanging(40.0, 32))):
        log = run(rest, style, 60, still, 4.0)
        dev, moving, grow = deviation(log[-1]), settled_motion(log), stretch(log[-1])
        limit = 1.5 if rest_name == "20-unit" else 3.0
        results.append((f"{name} {rest_name} lock rests near its style", dev < limit and moving < 0.005 and grow < 1.07,
                        f"off style {dev:.2f} (limit {limit}), moving {moving:.4f}/frame, length x{grow:.3f}"))
    return results


def check_frame_rates(name):
    style = PRESETS[name]
    rest = hanging(20.0, 20)
    results = []
    for motion_name, motion, when in (("sprint", sprint, 1.0), ("turn", turn, 0.4), ("cliff fall", cliff, FALL_START + 1.0)):
        values = {fps: deviation(at(run(rest, style, fps, motion, when + 0.05), when)) for fps in (30, 60, 144, 240)}
        spread = max(abs(v / values[60] - 1.0) for v in values.values()) if values[60] > 1e-3 else 0.0
        results.append((f"{name} {motion_name} is the same at 30-240 fps", spread < 0.1, ", ".join(f"{fps}: {v:.2f}" for fps, v in values.items())))
    log = run(rest, style, 60, sprint, 1.0, jitter=0.3)
    log_steady = run(rest, style, 60, sprint, 1.0)
    diff = abs(deviation(at(log, 1.0)) - deviation(at(log_steady, 1.0)))
    results.append((f"{name} uneven frame times do not change the motion", diff < 0.25, f"sprint at 1 s differs by {diff:.3f} with 30% frame jitter"))
    return results


def check_motion(name):
    style = PRESETS[name]
    results = []
    long_lock = hanging(40.0, 32)
    log = run(long_lock, style, 60, cliff, FALL_START + FALL_TIME + 5.0)
    angles = [angle_from_down(at(log, FALL_START + x)) for x in (0.5, 1.0, 1.5)]
    settle = settled_motion(log)
    rises = name in ("straight", "wavy", "locs")
    longest = max(stretch(r) for r in log)
    ok = settle < 0.01 and (angles[2] > 90.0 if rises else True) and longest <= MAX_STRETCH + 0.01
    results.append((f"{name} long hair streams up in a fall{'' if rises else ' (not required)'} and settles after landing", ok,
                    f"angle from down {angles[0]:.0f} / {angles[1]:.0f} / {angles[2]:.0f} deg at 0.5 / 1 / 1.5 s, length up to x{longest:.2f}, moving {settle:.4f}/frame 5 s after landing"))

    for motion_name, motion, when, seconds in (("sprint", sprint, 1.0, 6.5), ("turn", turn, None, 3.5), ("bow", bow, 1.9, 5.5)):
        log = run(hanging(20.0, 20), style, 60, motion, seconds)
        peak = at(log, when) if when else max(log, key=deviation)
        rest_dev = deviation(run(hanging(20.0, 20), style, 60, still, seconds)[-1])
        settle = settled_motion(log)
        # A coily cloud holds its shape by design: it need not visibly fall when the head bows.
        moved = deviation(peak) > rest_dev + 0.3 or (name == "coily" and motion_name == "bow")
        results.append((f"{name} {motion_name} moves the hair and settles", moved and settle < 0.01,
                         f"{deviation(peak):.2f} off target {'at ' + str(when) + ' s' if when else 'at peak'} (rest {rest_dev:.2f}), moving {settle:.4f}/frame at the end"))
    return results


def check_held(name):
    """A loaded lock held still (a tilted head) must come to rest rather than flutter."""
    style = PRESETS[name]
    worst = 0.0
    for rest in (scalp_lock(length=1.5), scalp_lock(), hanging(20.0, 20)):
        for pitch, roll in ((-20.0, 0.0), (-60.0, 0.0), (-90.0, 0.0), (0.0, 70.0)):
            head = head_transform(pitch=pitch, roll=roll, pivot=(0, 0, 110.0))
            log = run(rest, style, 60, lambda _: head, 5.0)
            worst = max(worst, settled_motion(log))
    return [(f"{name} locks come to rest on a tilted head or one on its side", worst < 0.005, f"worst move {worst:.4f} per frame after 4 s")]


def check_collision():
    shoulder = [(np.array([4.0, -3.0, 104.0]), np.array([14.0, -3.0, 104.0]), 3.5)]
    head_sphere = [(np.array([0.0, 0.0, 115.0]), np.array([0.0, 0.0, 115.0]), 6.0)]
    results = []
    for label, colliders in (("shoulder capsule", shoulder), ("head sphere", head_sphere)):
        log = run(hanging(20.0, 20), PRESETS["straight"], 60, turn, 3.0, colliders)
        a, b, radius = colliders[0]
        # Simulated points must stay out; drawn ones are interpolated between steps and carried by
        # this frame's skinning, so they may dip in by about a step's motion.
        simulated = min(min(np.linalg.norm(p - closest_on_segment(p, a, b)) for p in row[3][2:]) for row in log)
        drawn = min(min(np.linalg.norm(p - closest_on_segment(p, a, b)) for p in row[1][2:]) for row in log)
        floor = radius * MIN_COLLIDER_DEPTH
        results.append((f"collision keeps points out of the {label} core", simulated >= floor - 1e-2 and drawn >= floor - 0.5 and all(np.isfinite(r[1]).all() for r in log),
                        f"closest simulated {simulated:.2f}, drawn {drawn:.2f} (floor {floor:.2f})"))
    return results


def check_head_field():
    """The head sphere lies inside the forehead (it is fitted inside the hair), so a fringe pressed
    back went into the head before it stopped. The head field is the head's own surface."""
    field = HeadField()
    rest = fringe(field)
    results = []

    def deepest(log, head_of):
        return max(max(field.depth(p, head_of(r[0])) for p in r[3]) for r in log if r[0] > 0.3)

    log = run(rest, PRESETS["straight"], 60, still, 4.0, head_field=field)
    results.append(("fringe held still rests on its style, out of the head", deviation(log[-1]) < 0.5 and settled_motion(log) < 0.005 and deepest(log, still) <= 0.05,
                    f"{deviation(log[-1]):.2f} off target, moving {settled_motion(log):.4f}/frame, {deepest(log, still):.2f} deep"))
    for motion_name, motion in (("walking", walk), ("sprinting", sprint)):
        for style_name in ("straight", "locs"):
            log = run(rest, PRESETS[style_name], 60, motion, 4.0, head_field=field)
            depth = deepest(log, motion)
            results.append((f"{style_name} fringe stays out of the head {motion_name}", depth <= 0.05, f"{depth:.2f} units deep"))
    return results


def body_still(_):
    return head_transform()


def body_idle(s):
    """Breathing: the body rises and falls 0.6 units with the head's idle bob."""
    return head_transform((0, 0, 0.6 * math.sin(2 * math.pi * 0.4 * s)))


def body_walk(s):
    """walk's body: the same path, bob and surge, without the head's nod."""
    y = 130.0 * s - 19.5 if s > 0.3 else 216.7 * s * s
    return head_transform((0, y + math.sin(4 * math.pi * s), 1.2 * math.sin(4 * math.pi * s)))


def body_turn(s):
    """The whole character turning 90 degrees in 0.25 s."""
    return head_transform(yaw=90.0 * min(s / 0.25, 1.0))


def head_tilt(s):
    """The head tipping 35 degrees towards the right shoulder in 0.4 s, about the neck."""
    return head_transform(roll=-35.0 * min(s / 0.4, 1.0), pivot=(0, 0, 97.0))


def through_shoulder(points=20, length=30.0):
    """A lock from the side of the head styled straight down through the shoulder and chest: what a
    hairstyle made on a bare head, or for a slimmer body, does on this one."""
    t = np.linspace(0.0, 1.0, points)
    return np.c_[np.full(points, 6.5), np.full(points, -4.0), 108.0 - length * t]


def down_the_back(points=20, length=30.0):
    """A lock from the back of the head styled lying on the neck and back, 0.05 units off them."""
    lock = np.c_[np.zeros(points), np.full(points, -6.0), 106.0 - length * np.linspace(0.0, 1.0, points)]
    for p in lock:
        while body_depth(p, body_still(0.0)) > 0.0:
            p[1] -= 0.02
        p[1] -= 0.05
    return lock


def drawn_on_body(log):
    """What StrandSkin.cs.hlsl draws: each frame's drawn points kept out of the body once more."""
    return [collide_body(r[1], r[2], r[4]) for r in log]


def vibration(frames, start):
    """The worst move in a run of three or more frames whose accelerations each reverse the last
    (a point shaking), from frame start; 0 if there is none."""
    x = np.array(frames[start:])
    a = x[2:] - 2.0 * x[1:-1] + x[:-2]
    size = np.linalg.norm(a, axis=2)
    flip = (np.einsum("fpk,fpk->fp", a[1:], a[:-1]) < 0.0) & (size[1:] > 0.005) & (size[:-1] > 0.005)
    worst = 0.0
    for p in range(flip.shape[1]):
        run = 0
        for k in range(flip.shape[0]):
            run = run + 1 if flip[k, p] else 0
            if run >= 3:
                worst = max(worst, size[k - 2:k + 2, p].max())
    return worst


def check_body_field():
    """The body colliders, built from each part's triangles, lie on the part's surface plus the margin
    (a radius per direction round each bone, the outermost layer fitted per texel)."""
    rng = np.random.default_rng(3)
    results = []
    for (name, (_, (kind, params))), collider in zip(BODY_PARTS.items(), body_colliders()):
        errors = []
        while len(errors) < 200:
            d = rng.normal(size=3)
            q = np.array([0.0, 0.0, rng.uniform(-3.0, collider.length + 3.0)]) + d / np.linalg.norm(d) * 2.0
            closest = np.array([0.0, 0.0, min(max(q[2], 0.0), collider.length)])
            direction = (q - closest) / np.linalg.norm(q - closest)
            distance = lambda r: part_distance(collider.origin + collider.axes @ (closest + direction * r), kind, params)  # noqa: E731
            if distance(0.0) > 0.0:
                continue  # the segment leaves the part here
            low, high = 0.0, BODY_MAX_RADIUS
            for _ in range(40):
                low, high = ((low + high) / 2, high) if distance((low + high) / 2) < 0.0 else (low, (low + high) / 2)
            errors.append(distance(collider.surface(closest + direction * low)[0] - BODY_MARGIN))
        low, high = min(errors), max(errors)
        results.append((f"body collider {name} lies on the body", -0.35 <= low and high <= 0.3, f"{low:+.2f} to {high:+.2f} units from the surface (margin {BODY_MARGIN} aside)"))
    return results


def check_body_collision():
    """Hair keeps out of the body's colliders (the body's own shape, not bone capsules), comes to
    rest on it, and does not shake against it while the body breathes, walks, runs or turns."""
    results = []
    style = PRESETS["straight"]

    def deepest(frames, log):
        return max(max(body_depth(p, r[4]) for p in drawn[2:]) for drawn, r in zip(frames[5:], log[5:]))

    log = run(through_shoulder(), style, 60, still, 5.0, body_motion=body_still)
    drawn = drawn_on_body(log)
    depth, moving = deepest(drawn, log), settled_motion(log)
    results.append(("a lock styled through the shoulder lies over it and comes to rest", depth <= 0.3 and moving < 0.005 and deviation(log[-1]) > 2.0,
                    f"{depth:.2f} deep at most, pushed {deviation(log[-1]):.1f} off its style, moving {moving:.4f}/frame"))

    rest = down_the_back()
    free = deviation(run(rest, style, 60, still, 5.0)[-1])
    log = run(rest, style, 60, still, 5.0, body_motion=body_still)
    moving = settled_motion(log)
    results.append(("a lock styled on the back rests as styled", deviation(log[-1]) <= free + 0.1 and moving < 0.005,
                    f"{deviation(log[-1]):.2f} off target (without the body {free:.2f}), moving {moving:.4f}/frame"))

    for label, rest, motion in (("turning", down_the_back(), turn), ("tilting", through_shoulder(), head_tilt)):
        log = run(rest, style, 60, motion, 4.0, body_motion=body_still)
        depth, moving = deepest(drawn_on_body(log), log), settled_motion(log)
        results.append((f"{label} the head presses hair onto the shoulder: it stays out and comes to rest", depth <= 0.3 and moving < 0.005,
                        f"{depth:.2f} deep at most, moving {moving:.4f}/frame at the end"))

    for fps in (30, 60, 144):
        worst_depth, worst_shake, worst_free = 0.0, 0.0, 0.0
        for rest in (down_the_back(), through_shoulder()):
            for motion, body in ((idle, body_idle), (walk, body_walk), (sprint, sprint), (body_turn, body_turn)):
                log = run(rest, style, fps, motion, 4.0, body_motion=body)
                drawn = drawn_on_body(log)
                worst_depth = max(worst_depth, deepest(drawn, log))
                worst_shake = max(worst_shake, vibration(drawn, fps))
                worst_free = max(worst_free, vibration([r[1] for r in run(rest, style, fps, motion, 4.0)], fps))
        results.append((f"breathing, walking, running and turning at {fps} fps: hair stays out of the body without shaking", worst_depth <= 0.35 and worst_shake <= 0.35,
                        f"{worst_depth:.2f} deep at most; shakes up to {worst_shake:.3f} units (without the body {worst_free:.3f})"))
    return results


def check_short_locks():
    """Short scalp locks (vanilla-style cards) keep their shape through one step aside or a small turn."""
    results = []
    for rest_name, rest in (("3.8-unit scalp lock", scalp_lock()), ("1.5-unit scalp lock", scalp_lock(length=1.5))):
        length = np.linalg.norm(np.diff(rest, axis=0), axis=1).sum()
        for motion_name, motion in (("small step", small_step), ("small turn", small_turn)):
            worst = max(max(deviation(r) for r in run(rest, PRESETS["straight"], fps, motion, 2.0)) for fps in (60, 144)) / length
            results.append((f"{rest_name} holds its shape through a {motion_name}", worst <= 0.1, f"{worst:.2f} of its length off target at most"))
    return results


def swings(rest, motion, still_from, fps, jitter, seconds=4.0):
    """The tip's swings once the head is still, with frame times jittered as in game: when it
    turns back (moves of 0.005 units or more) and how far it went since it last turned. Also how
    much the lock still moves over the last half second."""
    log = run(rest, PRESETS["straight"], fps, motion, seconds, jitter=jitter)
    offset, move, last, travel, turns = None, np.zeros(3), None, 0.0, []
    for s, shown, target, *_ in log:
        tip = shown[-1] - target[-1]
        if s > still_from and offset is not None:
            move += tip - offset
            if np.linalg.norm(move) >= 0.005:
                if last is not None and move @ last < 0.0:
                    turns.append((s, travel))
                    travel = 0.0
                travel += np.linalg.norm(move)
                last, move = move, np.zeros(3)
        offset = tip
    return turns, settled_motion(log, 0.5)


def check_swings():
    """Once the head is still, a swing dies down as a pendulum's does rather than running up and
    down the strand. A wave running to the tip and back turned it back 12 times within 1.5 s of
    one step aside; a 20-unit lock swings back about every 0.4 s, each swing well under the last."""
    results = []
    for rest_name, rest in (("20-unit lock", hanging(20.0, 20)), ("3.8-unit scalp lock", scalp_lock())):
        for fps in (60, 144):
            quickest, decay, most, moving = math.inf, 0.0, 0, 0.0
            for motion in (small_step, small_turn):
                turns, still_moving = swings(rest, motion, 1.0, fps, 0.1)
                most, moving = max(most, len(turns)), max(moving, still_moving)
                quickest = min([quickest] + [b[0] - a[0] for a, b in zip(turns, turns[1:])])
                # The head stopping cuts the first swing short: compare whole swings of 0.05 units or more.
                decay = max([decay] + [b[1] / a[1] for a, b in zip(turns[1:], turns[2:]) if a[1] >= 0.05])
            ok = quickest >= 0.25 and decay <= 0.6 and moving < 0.005
            apart = f"at least {quickest:.2f} s apart" if math.isfinite(quickest) else "never twice"
            results.append((f"{rest_name} swing dies down once the head stops, {fps} fps", ok,
                            f"tip turned back {most} times, {apart}, each swing at most {decay:.2f} of the one before, moving {moving:.4f}/frame after 3.5 s"))
    return results


def check_wind():
    results = []
    style = PRESETS["straight"]
    for speed in (0.3, 1.0):
        log = run(hanging(20.0, 20), style, 60, still, 8.0, wind_speed=speed)
        calm = deviation(run(hanging(20.0, 20), style, 60, still, 8.0)[-1])
        worst = max(deviation(r) for r in log[len(log) // 2:])
        ok = worst > calm + 0.2 and worst < 12.0 and all(np.isfinite(r[1]).all() for r in log) and max(stretch(r) for r in log) < 1.2
        results.append((f"wind at {speed:.0%} of full moves a 20-unit lock, within bounds", ok, f"up to {worst:.2f} off target (calm {calm:.2f})"))
    return results


def check_extremes():
    """Settings at the ends of their sliders must stay finite and bounded."""
    results = []
    extremes = {
        "all minimum": dict(vsp=0.0, vsp_threshold=0.0, local=0.0, local_iterations=0, global_stiffness=0.0, global_range=0.0,
                            length_iterations=1, damping=0.0, gravity=0.0, tip_separation=0.0, clamp=0.5, wind=0.0),
        "all maximum": dict(vsp=1.0, vsp_threshold=100.0, local=1.0, local_iterations=8, global_stiffness=1.0, global_range=1.0,
                            length_iterations=16, damping=1.0, gravity=1400.0, tip_separation=2.0, clamp=200.0, wind=3.0),
        "loose and heavy": dict(DEFAULT, vsp=0.0, local=0.0, global_stiffness=0.0, damping=0.0, gravity=1400.0, length_iterations=1, clamp=200.0),
    }
    for name, style in extremes.items():
        worst = 0.0
        finite = True
        for motion in (cliff, sprint, turn):
            log = run(hanging(40.0, 32), style, 60, motion, 3.0, wind_speed=1.0)
            finite &= all(np.isfinite(r[1]).all() for r in log)
            worst = max(worst, max(deviation(r) for r in log))
        results.append((f"{name} settings stay finite and bounded", finite and worst < 200.0, f"up to {worst:.1f} off target"))
    return results


def run_follower(motion, fps, seconds, length, offset, tip_separation=0.0):
    """A short guide (vanilla-style cards: 3.8 units, 14 points) and one follower beside it."""
    style = dict(PRESETS["straight"], tip_separation=tip_separation)
    guide = Guide(lock(3.8, (0, 0, 0)))
    rest = np.c_[lock(length, offset), np.ones(14)]
    guide_rest = np.c_[lock(3.8, (0, 0, 0)), np.ones(14)]
    clock = Clock()
    previous = motion(0.0)
    guide.frame(previous, previous, clock.frame(0.0), style)
    worst_guide, worst_follower, lengths = 0.0, 0.0, []
    for k in range(1, int(seconds * fps) + 1):
        current = motion(k / fps)
        guide.frame(current, previous, clock.frame(1.0 / fps), style)
        previous = current
        target = rest @ current.T
        x = follow(guide, 3.8, target, length, tip_separation, guide_rest @ current.T)
        worst_guide = max(worst_guide, np.linalg.norm(guide.offset, axis=1).max())
        worst_follower = max(worst_follower, np.linalg.norm(x - target, axis=1).max())
        lengths.append(np.linalg.norm(np.diff(x, axis=0), axis=1).sum() / np.linalg.norm(np.diff(target, axis=0), axis=1).sum())
    return worst_guide, worst_follower, min(lengths), max(lengths)


def check_followers():
    results = []
    # A snap turn (90 degrees in 0.05 s) stretches TressFX's strands for a moment; idle sway must not.
    for motion_name, motion, stretch_limit in (("idle", idle, 1.1), ("snap turn", snap, 1.15)):
        for length, offset in ((3.8, (0, 2.5, 0)), (8.0, (1.5, 0, 0))):
            guide_dev, follower_dev, shortest, longest = run_follower(motion, 60, 3.0, length, offset)
            ok = follower_dev <= guide_dev + 1e-3 and 0.9 <= shortest and longest <= stretch_limit
            results.append((f"{motion_name}: {length}-unit follower tracks its 3.8-unit guide", ok,
                            f"guide {guide_dev:.2f}, follower {follower_dev:.2f} off target, length x{shortest:.2f}-{longest:.2f}"))
    # TressFX's tip separation fans a follower out from its guide towards the tip, and only there.
    _, spread_dev, _, _ = run_follower(still, 60, 1.0, 3.8, (0, 2.5, 0), tip_separation=1.0)
    results.append(("tip separation fans followers out towards the tip", spread_dev > 1.0, f"follower {spread_dev:.2f} off its own target at separation 1"))
    return results


def job(task):
    kind, arg = task
    if kind == "rest":
        return check_rest(arg)
    if kind == "rates":
        return check_frame_rates(arg)
    if kind == "motion":
        return check_motion(arg)
    if kind == "held":
        return check_held(arg)
    return {"collision": check_collision, "head field": check_head_field, "body field": check_body_field, "body collision": check_body_collision,
            "short locks": check_short_locks, "swings": check_swings,
            "wind": check_wind, "extremes": check_extremes, "followers": check_followers}[kind]()


def main():
    tasks = [(kind, name) for name in PRESETS for kind in ("rest", "motion", "held")]
    tasks += [("rates", "straight"), ("rates", "locs")]
    tasks += [(kind, None) for kind in ("body collision", "collision", "head field", "body field", "short locks", "swings", "wind", "extremes", "followers")]
    failures = []
    with Pool() as pool:
        for results in pool.imap(job, tasks):
            for name, ok, detail in results:
                print(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}", flush=True)
                if not ok:
                    failures.append(name)
    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
