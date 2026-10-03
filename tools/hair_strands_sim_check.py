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
field) while walking and sprinting; hair above an open collar's rim is not taken for its inside; followers stray no further than their guide and keep their
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
FULL_GRAVITY_LENGTH = 20.0  # StrandSim's FullGravityLength
SKYRIM_GRAVITY = 686.7  # a falling character, units/s^2
# The body's distance field (BodySdf.cpp, BodySdf.cs.hlsl, HairStrandsSkin::SampleBody).
SDF_CELL = 1.25  # kCellSize (actor scale 1)
SDF_OUTSIDE = 2.0  # kOutsideBand, cells
SDF_MAX_INSIDE = 6.0  # kMaxInsideBand, cells
SDF_INSIDE_COS = -0.25  # kInsideCos
SDF_INSIDE_BAND_COS = -0.7  # kInsideBandCos
SDF_STEPS = 4095.0  # BODY_SDF_DISTANCE_STEPS
SDF_BAND_STEPS = 32  # kInsideBandSteps
SDF_MIN_WEIGHT = 0.25  # HairStrandsSkin::MinBodyWeight
THICKNESS_TILT = 0.7  # kThicknessTilt, radians
RAY_TOLERANCE = 0.02  # kRayTolerance
CLUSTER_SIZE = 1.25  # kClusterSize: the collision mesh's vertex spacing
BODY_MIN_CLEARANCE = 0.15  # kBodyMinClearance
BODY_MAX_CLEARANCE = 0.35  # kBodyMaxClearance
BODY_SLIDE = 0.4  # HairStrandsSim::BodySlide, TressFX's capsule friction share
CONTACT_FRICTION = 0.25  # HairStrandsSim::ContactFriction

# Strands::StrandStyle motion defaults and MakePresetStyle's changes to them.
DEFAULT = dict(vsp=0.4, vsp_threshold=1.208, local=0.5, local_iterations=3, global_stiffness=0.408, global_range=0.4,
               length_iterations=16, damping=0.107, gravity=250.0, tip_separation=0.0, clamp=20.0, wind=1.0)
PRESETS = {
    "straight": dict(DEFAULT),
    "wavy": dict(DEFAULT, local=0.512, damping=0.118),
    "curly": dict(DEFAULT, vsp=0.5, local=0.523, local_iterations=4, global_stiffness=0.45, global_range=0.5, damping=0.126, gravity=250.0),
    "coily": dict(DEFAULT, vsp=0.7, local=0.523, local_iterations=4, global_stiffness=0.6, global_range=0.8, damping=0.236, gravity=166.667, wind=0.4),
    "locs": dict(DEFAULT, vsp=0.3, local=0.468, global_range=0.3, gravity=291.667, wind=0.6),
}


def contact_move(delta, normal, surface_move, shift, correction):
    """HairStrandsSim::ContactMove: velocity response, independent of positional push-out."""
    slide = delta + shift - surface_move
    slide = slide - normal * (slide @ normal)
    slip = np.clip(1.0 - CONTACT_FRICTION * max(correction, 0.0) / max(np.linalg.norm(slide), 1e-7), 0.0, 1.0)
    return surface_move - shift + BODY_SLIDE * slip * slide


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


def dot(a, b):
    return np.einsum("...k,...k->...", a, b)


def closest_on_triangles(p, a, b, c):
    """HairStrandsBody::ClosestOnTriangle (Ericson, 5.1.5), vectorised over rows. Returns the closest
    points and their barycentric weights."""
    ab, ac = b - a, c - a
    ap, bp, cp = p - a, p - b, p - c
    d1, d2 = dot(ab, ap), dot(ac, ap)
    d3, d4 = dot(ab, bp), dot(ac, bp)
    d5, d6 = dot(ab, cp), dot(ac, cp)
    va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2
    denominator = 1.0 / np.maximum(va + vb + vc, 1e-12)
    v, w = vb * denominator, vc * denominator
    weights = np.stack([1.0 - v - w, v, w], axis=-1)

    def put(mask, values):
        weights[mask] = values[mask] if values.ndim == weights.ndim else values

    # The shader's tests in reverse order, so the earlier ones win.
    t = (d4 - d3) / np.maximum((d4 - d3) + (d5 - d6), 1e-12)
    put((va <= 0.0) & (d4 - d3 >= 0.0) & (d5 - d6 >= 0.0), np.stack([np.zeros_like(t), 1.0 - t, t], axis=-1))
    t = d2 / np.maximum(d2 - d6, 1e-12)
    put((vb <= 0.0) & (d2 >= 0.0) & (d6 <= 0.0), np.stack([1.0 - t, np.zeros_like(t), t], axis=-1))
    put((d6 >= 0.0) & (d5 <= d6), np.array([0.0, 0.0, 1.0]))
    t = d1 / np.maximum(d1 - d3, 1e-12)
    put((vc <= 0.0) & (d1 >= 0.0) & (d3 <= 0.0), np.stack([1.0 - t, t, np.zeros_like(t)], axis=-1))
    put((d3 >= 0.0) & (d4 <= d3), np.array([0.0, 1.0, 0.0]))
    put((d1 <= 0.0) & (d2 <= 0.0), np.array([1.0, 0.0, 0.0]))
    closest = weights[..., :1] * a + weights[..., 1:2] * b + weights[..., 2:] * c
    return closest, weights


def inside_bands(vertices, triangles):
    """BodySdf.cpp: each triangle's inside band, how far behind it the field reaches: half as far as
    the mesh is solid there, the shortest of five rays from its centroid (straight in and four tilted
    by THICKNESS_TILT) to the face each leaves through (the TriangleGrid's Moller-Trumbore with a
    little slack), at least a cell, at most the most; quantised as the GPU reads it. A ray that
    leaves the mesh's box through no face is open and does not count; with none closed, nothing is
    solid behind the triangle and its band is 0: no inside, the field the distance either side of
    it. (BodySdf.cpp also gives a sheet, one face of a thin pair or a two-sided material, 0; the
    meshes here have none.) In cells."""
    a, b, c = (vertices[triangles[:, k]] for k in range(3))
    normals = np.cross(b - a, c - a)
    normals /= np.linalg.norm(normals, axis=1)[:, None]
    centroids = (a + b + c) / 3.0
    e1, e2 = b - a, c - a
    max_units = SDF_MAX_INSIDE * SDF_CELL
    used = vertices[np.unique(triangles)]
    longest = np.linalg.norm(used.max(axis=0) - used.min(axis=0)) + 1.0
    # Triangles by the cells of a coarse grid their boxes overlap, as the TriangleGrid.
    cell = 2.0
    origin = vertices.min(axis=0) - cell
    low = np.floor((np.minimum(np.minimum(a, b), c) - origin) / cell).astype(int)
    high = np.floor((np.maximum(np.maximum(a, b), c) - origin) / cell).astype(int)
    buckets = {}
    for t in range(len(triangles)):
        for x in range(low[t, 0], high[t, 0] + 1):
            for y in range(low[t, 1], high[t, 1] + 1):
                for z in range(low[t, 2], high[t, 2] + 1):
                    buckets.setdefault((x, y, z), []).append(t)
    thickness = np.full(len(triangles), longest)
    solid = np.zeros(len(triangles), bool)
    tilt_cos, tilt_sin = math.cos(THICKNESS_TILT), math.sin(THICKNESS_TILT)
    steps = np.arange(0.0, longest + cell, cell * 0.5)
    for r in range(len(triangles)):
        n = normals[r]
        o = centroids[r] - n * 0.01
        u = np.cross(n, [0.0, 0.0, 1.0] if abs(n[2]) < 0.9 else [1.0, 0.0, 0.0])
        u /= np.linalg.norm(u)
        v = np.cross(n, u)
        for d in (-n, -n * tilt_cos + u * tilt_sin, -n * tilt_cos - u * tilt_sin, -n * tilt_cos + v * tilt_sin, -n * tilt_cos - v * tilt_sin):
            keys = {tuple(k) for k in np.floor((o + steps[:, None] * d - origin) / cell).astype(int)}
            near = [t for key in keys for t in buckets.get(key, ())]
            if not near:
                continue
            k = np.unique(np.array(near))
            k = k[(k != r) & (normals[k] @ d > 0.1)]
            if len(k) == 0:
                continue
            p = np.cross(d, e2[k])
            det = dot(e1[k], p)
            ok = np.abs(det) > 1e-12
            inverse = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
            sv = o - a[k]
            uu = dot(sv, p) * inverse
            q = np.cross(sv, e1[k])
            vv = (q @ d) * inverse
            t = dot(e2[k], q) * inverse
            hit = ok & (uu >= -RAY_TOLERANCE) & (uu <= 1.0 + RAY_TOLERANCE) & (vv >= -RAY_TOLERANCE) & (uu + vv <= 1.0 + RAY_TOLERANCE) & (t > 0.0) & (t <= thickness[r])
            if hit.any():
                thickness[r] = t[hit].min()
                solid[r] = True
    band = np.where(solid, np.clip(0.5 * thickness, SDF_CELL, max_units), 0.0)
    quantised = np.minimum(np.round(band / SDF_CELL * SDF_BAND_STEPS), 255)
    return quantised / SDF_BAND_STEPS  # cells


CORNERS = np.array([(x, y, z) for x in (0, 1) for y in (0, 1) for z in (0, 1)])


class BodySdf:
    """The body's narrow-band signed distance field, as BodySdf.cs.hlsl builds it: per cell the nearest
    triangle (by quantised distance, then index) of those reaching it (in front, out to the outside
    band; behind, out to the triangle's inside band, or the outside band where nothing is solid
    behind it); then the distance to it, signed by its vertex normals at the closest point (cells
    nearly level with the surface outside, and every cell of a triangle with nothing solid behind
    it), the outward normal, and the closest point. Built once in the body's own frame: the bodies
    here move rigidly, so the field the GPU builds every frame on the actor's axes is this one,
    carried along."""

    def __init__(self, vertices, normals, triangles, sources=None):
        """sources: each triangle's source mesh (BodySdf.cpp finds each one's inside bands in its own
        mesh); one source without."""
        self.vertices, self.normals, self.triangles = np.asarray(vertices, float), np.asarray(normals, float), np.asarray(triangles)
        sources = np.zeros(len(self.triangles), int) if sources is None else np.asarray(sources)
        bands = np.zeros(len(self.triangles))
        for source in np.unique(sources):
            mine = sources == source
            bands[mine] = inside_bands(self.vertices, self.triangles[mine])
        margin = (SDF_MAX_INSIDE + SDF_OUTSIDE + 1.0) * SDF_CELL
        self.origin = np.floor((self.vertices.min(axis=0) - margin) / SDF_CELL) * SDF_CELL
        self.size = np.ceil((self.vertices.max(axis=0) + margin - self.origin) / SDF_CELL).astype(int)
        cells = np.full(int(np.prod(self.size)), 0xFFFFFFFF, dtype=np.uint64)
        max_band = max(SDF_OUTSIDE, 255.0 / SDF_BAND_STEPS)
        grid = lambda p: (p - self.origin) / SDF_CELL  # noqa: E731
        for t, (i0, i1, i2) in enumerate(self.triangles):
            a, b, c = grid(self.vertices[i0]), grid(self.vertices[i1]), grid(self.vertices[i2])
            n = np.cross(b - a, c - a)
            n /= np.linalg.norm(n)
            inside = bands[t]
            # A triangle with nothing solid behind it reaches the outside band on both sides.
            reach = inside if inside > 0.0 else SDF_OUTSIDE
            low = np.minimum(np.minimum(a, b), c)
            high = np.maximum(np.maximum(a, b), c)
            low = np.minimum(low, low - n * reach) - SDF_OUTSIDE
            high = np.maximum(high, high - n * reach) + SDF_OUTSIDE
            first = np.maximum(np.ceil(low - 0.5), 0).astype(int)
            last = np.minimum(np.floor(high - 0.5), self.size - 1).astype(int)
            if np.any(first > last):
                continue
            axes = [np.arange(first[k], last[k] + 1) for k in range(3)]
            index = np.stack(np.meshgrid(*axes, indexing="ij"), axis=-1).reshape(-1, 3)
            centre = index + 0.5
            side = (centre - a) @ n
            keep = (side <= SDF_OUTSIDE) & (side >= -reach)
            index, centre, side = index[keep], centre[keep], side[keep]
            closest, w = closest_on_triangles(centre, a, b, c)
            offset = centre - closest
            distance = np.linalg.norm(offset, axis=1)
            # The inside band only for cells Finalize would put inside; beside it, the outside band.
            vertex_normal = w[:, :1] * self.normals[i0] + w[:, 1:2] * self.normals[i1] + w[:, 2:] * self.normals[i2]
            behind = (inside > 0.0) & (dot(offset, vertex_normal) < SDF_INSIDE_BAND_COS * distance * np.linalg.norm(vertex_normal, axis=1))
            keep = distance <= np.where(behind, inside, SDF_OUTSIDE)
            key = (np.floor(np.clip(distance[keep] / max_band, 0.0, 1.0) * SDF_STEPS).astype(np.uint64) << np.uint64(20)) | np.uint64(t)
            flat = (index[keep] * [self.size[1] * self.size[2], self.size[2], 1]).sum(axis=1)
            np.minimum.at(cells, flat, key)

        # Finalize.
        valid = cells != 0xFFFFFFFF
        self.weight = valid.astype(float).reshape(self.size)
        self.distance = np.zeros(self.size)
        self.normal = np.zeros((*self.size, 3))
        self.closest = np.zeros((*self.size, 3))
        flat = np.nonzero(valid)[0]
        winner = (cells[flat] & np.uint64(0xFFFFF)).astype(int)
        tri = self.triangles[winner]
        index = np.stack(np.unravel_index(flat, self.size), axis=-1)
        centre = index + 0.5
        a, b, c = (grid(self.vertices[tri[:, k]]) for k in range(3))
        closest, weights = closest_on_triangles(centre, a, b, c)
        offset = centre - closest
        length = np.linalg.norm(offset, axis=1)
        face = np.cross(b - a, c - a)
        face /= np.maximum(np.linalg.norm(face, axis=1), 1e-12)[:, None]
        vertex_normal = sum(weights[:, k:k + 1] * self.normals[tri[:, k]] for k in range(3))
        vertex_normal = np.where(np.linalg.norm(vertex_normal, axis=1)[:, None] > 1e-6, vertex_normal, face)
        vertex_normal /= np.linalg.norm(vertex_normal, axis=1)[:, None]
        inside = (bands[winner] > 0.0) & (length > 1e-4) & (dot(offset, vertex_normal) < SDF_INSIDE_COS * length)
        gradient = np.where((length > 1e-3)[:, None], offset * (np.where(inside, -1.0, 1.0) / np.maximum(length, 1e-12))[:, None], vertex_normal)
        i, j, k = index.T
        self.distance[i, j, k] = np.where(inside, -length, length) * SDF_CELL
        self.normal[i, j, k] = gradient / np.linalg.norm(gradient, axis=1)[:, None]
        self.closest[i, j, k] = self.origin + closest * SDF_CELL

    def sample(self, p, pose, previous_pose):
        """HairStrandsSkin::SampleBody at world point p, the body on (rigid) pose this frame and
        previous_pose last: (distance, outward normal, the surface's move over the frame) or None where
        the field has no value. Filtered as the GPU filters the premultiplied textures."""
        rotation = pose[:, :3]
        cell = (rotation.T @ (p - pose[:, 3]) - self.origin) / SDF_CELL
        if np.any(cell < 0.5) or np.any(cell > self.size - 0.5):
            return None
        x = cell - 0.5
        base = np.floor(x).astype(int)
        f = x - base
        i, j, k = np.minimum(base + CORNERS, self.size - 1).T
        w = np.where(CORNERS, f, 1.0 - f).prod(axis=1) * self.weight[i, j, k]
        total = w.sum()
        if total <= SDF_MIN_WEIGHT:
            return None
        normal = rotation @ (w @ self.normal[i, j, k])
        length = np.linalg.norm(normal)
        if length < 1e-6:
            return None
        q = self.closest[i, j, k]
        move = w @ (q @ (rotation - previous_pose[:, :3]).T + (pose[:, 3] - previous_pose[:, 3]))
        return (w @ self.distance[i, j, k]) / total, normal / length, move / total

    def ahead(self, p, f, pose, previous_pose, snap_move=0.0):
        """HairStrandsSkin::BodyAhead: how far the surface near world point p, as it is f of the way
        through the frame, moves by the frame's end. The actor's root (here the whole body) carries
        p first; the surface's own move over the frame is read where that puts it."""
        root = (pose[:, :3] @ np.linalg.solve(previous_pose[:, :3], p - snap_move - previous_pose[:, 3]) + pose[:, 3] - p) * (1.0 - f)
        hit = self.sample(p + root, pose, previous_pose)
        return root if hit is None else (hit[2] - snap_move) * (1.0 - f)

    def limits(self, target, pose, previous_pose):
        """HairStrandsSkin::BodyLimits: (how far off the body the point is kept, the deepest it is believed to lie)."""
        hit = self.sample(target, pose, previous_pose)
        trust = SDF_OUTSIDE * SDF_CELL
        if hit is None:
            return BODY_MAX_CLEARANCE, -trust
        return min(max(hit[0], BODY_MIN_CLEARANCE), BODY_MAX_CLEARANCE), min(hit[0], 0.0) - trust

    def collide(self, p, ahead, limits, pose, previous_pose):
        """HairStrandsSkin::CollideBody: (hit, position, normal, the surface's move over the frame,
        the distance read)."""
        hit = self.sample(p + ahead, pose, previous_pose)
        if hit is None or not hit[0] < limits[0] or hit[0] < limits[1]:
            return False, p, None, None, None
        distance, normal, move = hit
        return True, p + normal * (limits[0] - distance), normal, move, distance


# --- Bodies: parts with exact (or near) signed distances, meshed as the collision mesh would be ---

ARMOUR = 2.5  # how far the cuirass stands off the torso


def frame_axes(direction):
    z = direction / np.linalg.norm(direction)
    x = np.cross(z, [0.0, 0.0, 1.0] if abs(z[2]) < 0.9 else [1.0, 0.0, 0.0])
    x /= np.linalg.norm(x)
    return np.c_[x, np.cross(z, x), z]


class Part:
    """A closed solid: kind 'ellipsoid' (centre, radii), 'capsule' (a, b, radius) or 'disc' (centre,
    axis, radius, thickness: a capped cylinder)."""

    def __init__(self, kind, *params):
        self.kind, self.params = kind, [np.asarray(p, float) if isinstance(p, (tuple, list)) else p for p in params]

    def distance(self, p):
        if self.kind == "ellipsoid":
            centre, radii = self.params
            k0, k1 = np.linalg.norm((p - centre) / radii), np.linalg.norm((p - centre) / (radii * radii))
            return k0 * (k0 - 1.0) / k1 if k1 > 1e-9 else -min(radii)
        if self.kind == "capsule":
            a, b, radius = self.params
            return np.linalg.norm(p - closest_on_segment(p, a, b)) - radius
        centre, axis, radius, thickness = self.params
        axis = axis / np.linalg.norm(axis)
        d = p - centre
        along = d @ axis
        across = np.linalg.norm(d - axis * along)
        q = np.array([across - radius, abs(along) - thickness / 2.0])
        return min(max(q[0], q[1]), 0.0) + np.linalg.norm(np.maximum(q, 0.0))

    def mesh(self):
        if self.kind == "ellipsoid":
            centre, radii = self.params
            rmax = max(radii)
            n_lat, n_lon = max(int(math.pi * rmax / CLUSTER_SIZE), 8), max(int(2.0 * math.pi * rmax / CLUSTER_SIZE), 12)
            vertices = [centre + radii * [math.sin(math.pi * i / n_lat) * math.cos(2 * math.pi * j / n_lon), math.sin(math.pi * i / n_lat) * math.sin(2 * math.pi * j / n_lon),
                                          math.cos(math.pi * i / n_lat)] for i in range(n_lat + 1) for j in range(n_lon)]
            triangles = []
            for i in range(n_lat):
                for j in range(n_lon):
                    a, b = i * n_lon + j, i * n_lon + (j + 1) % n_lon
                    triangles += [(a, a + n_lon, b), (b, a + n_lon, b + n_lon)]
            return np.array(vertices), np.array(triangles)
        if self.kind == "capsule":
            a, b, radius = self.params
            axes, length = frame_axes(b - a), np.linalg.norm(b - a)
            n_lon, n_cap, n_side = max(int(2.0 * math.pi * radius / CLUSTER_SIZE), 12), max(int(math.pi / 2 * radius / CLUSTER_SIZE), 3), max(int(length / CLUSTER_SIZE), 1)
            rings = [(-radius * math.cos(math.pi / 2 * i / n_cap), radius * math.sin(math.pi / 2 * i / n_cap)) for i in range(1, n_cap + 1)]
            rings += [(length * i / n_side, radius) for i in range(1, n_side + 1)]
            rings += [(length + radius * math.sin(math.pi / 2 * i / n_cap), radius * math.cos(math.pi / 2 * i / n_cap)) for i in range(1, n_cap)]
            vertices = [a + axes @ [0.0, 0.0, -radius]]
            vertices += [a + axes @ [r * math.cos(2 * math.pi * j / n_lon), r * math.sin(2 * math.pi * j / n_lon), z] for z, r in rings for j in range(n_lon)]
            vertices.append(a + axes @ [0.0, 0.0, length + radius])
            triangles = [(0, 1 + j, 1 + (j + 1) % n_lon) for j in range(n_lon)]
            for i in range(len(rings) - 1):
                for j in range(n_lon):
                    p, q = 1 + i * n_lon + j, 1 + i * n_lon + (j + 1) % n_lon
                    triangles += [(p, p + n_lon, q), (q, p + n_lon, q + n_lon)]
            last = 1 + (len(rings) - 1) * n_lon
            triangles += [(last + j, len(vertices) - 1, last + (j + 1) % n_lon) for j in range(n_lon)]
            return np.array(vertices), np.array(triangles)
        centre, axis, radius, thickness = self.params
        axes = frame_axes(axis)
        n_lon, n_ring = max(int(2.0 * math.pi * radius / CLUSTER_SIZE), 12), max(int(radius / CLUSTER_SIZE), 2)
        vertices, triangles = [], []
        for side in (-1.0, 1.0):
            base = len(vertices)
            vertices.append(centre + axes @ [0.0, 0.0, side * thickness / 2.0])
            for i in range(1, n_ring + 1):
                r = radius * i / n_ring
                vertices += [centre + axes @ [r * math.cos(2 * math.pi * j / n_lon), r * math.sin(2 * math.pi * j / n_lon), side * thickness / 2.0] for j in range(n_lon)]
            triangles += [(base, base + 1 + j, base + 1 + (j + 1) % n_lon) for j in range(n_lon)]
            for i in range(n_ring - 1):
                for j in range(n_lon):
                    p, q = base + 1 + i * n_lon + j, base + 1 + i * n_lon + (j + 1) % n_lon
                    triangles += [(p, p + n_lon, q), (q, p + n_lon, q + n_lon)]
        rim0, rim1 = 1 + (n_ring - 1) * n_lon, len(vertices) - n_lon
        for j in range(n_lon):
            p, q = rim0 + j, rim0 + (j + 1) % n_lon
            triangles += [(p, rim1 + j, q), (q, rim1 + j, rim1 + (j + 1) % n_lon)]
        return np.array(vertices), np.array(triangles)


def gradient(distance, p, h=1e-3):
    g = np.array([distance(p + e) - distance(p - e) for e in np.eye(3) * h])
    return g / max(np.linalg.norm(g), 1e-12)


# A body under the head (+Y forward, +Z up), parts overlapping as a body's regions do.
NECK = Part("capsule", (0, -1.5, 88.0), (0, -1.0, 103.0), 4.0)
TORSO = [Part("ellipsoid", (0, -1.0, 81.0), (15.0, 8.5, 11.0)), Part("ellipsoid", (0, -1.0, 70.0), (13.5, 8.0, 9.0)), Part("ellipsoid", (0, -1.0, 58.0), (12.0, 7.5, 8.0))]
SHOULDERS = [Part("capsule", (6.0, -1.5, 90.0), (15.5, -2.0, 88.0), 4.0), Part("capsule", (-6.0, -1.5, 90.0), (-15.5, -2.0, 88.0), 4.0)]
ARMS = [Part("capsule", (15.5, -2.0, 88.0), (17.5, -2.0, 70.0), 3.8), Part("capsule", (-15.5, -2.0, 88.0), (-17.5, -2.0, 70.0), 3.8)]
# A cuirass: the torso's shape ARMOUR out, open at the neck and the waist, single-sided (as Skyrim
# armour replaces the body under it). Its inside counts as solid.
CUIRASS = [Part("ellipsoid", p.params[0], p.params[1] + ARMOUR) for p in TORSO]
CUIRASS_TOP, CUIRASS_BOTTOM = 93.0, 52.0
# A shield on the back, 4 units off the cuirass, 2 thick.
SHIELD = Part("disc", (0, -16.0, 80.0), (0, 1.0, 0), 15.0, 2.0)


def field(parts, points):
    """The union's signed distance (each part's, the least), vectorised over points."""
    out = np.full(len(points), np.inf)
    for part in parts:
        if part.kind == "ellipsoid":
            centre, radii = part.params
            k0 = np.linalg.norm((points - centre) / radii, axis=1)
            k1 = np.linalg.norm((points - centre) / (radii * radii), axis=1)
            d = np.where(k1 > 1e-9, k0 * (k0 - 1.0) / np.maximum(k1, 1e-9), -min(radii))
        elif part.kind == "capsule":
            a, b, radius = part.params
            ab = b - a
            t = np.clip(((points - a) @ ab) / (ab @ ab), 0.0, 1.0)
            d = np.linalg.norm(points - (a + t[:, None] * ab), axis=1) - radius
        else:
            centre, axis, radius, thickness = part.params
            axis = axis / np.linalg.norm(axis)
            rel = points - centre
            along = rel @ axis
            across = np.linalg.norm(rel - along[:, None] * axis, axis=1)
            q = np.stack([across - radius, np.abs(along) - thickness / 2.0], axis=1)
            d = np.minimum(np.maximum(q[:, 0], q[:, 1]), 0.0) + np.linalg.norm(np.maximum(q, 0.0), axis=1)
        out = np.minimum(out, d)
    return out


# Six tetrahedra per cube round its main diagonal (corner index: x + 2y + 4z).
CUBE = np.array([(x, y, z) for z in (0, 1) for y in (0, 1) for x in (0, 1)])
TETRAHEDRA = [(0, 1, 3, 7), (0, 3, 2, 7), (0, 2, 6, 7), (0, 6, 4, 7), (0, 4, 5, 7), (0, 5, 1, 7)]


def isosurface(parts, spacing=CLUSTER_SIZE):
    """A watertight mesh of the union's surface (marching tetrahedra), wound outwards, with the
    union's gradient as vertex normals: one closed skin, as a body mesh is."""
    points = np.concatenate([np.stack([p.mesh()[0].min(axis=0), p.mesh()[0].max(axis=0)]) for p in parts])
    origin = points.min(axis=0) - 2.0 * spacing
    shape = np.ceil((points.max(axis=0) + 2.0 * spacing - origin) / spacing).astype(int) + 1
    grid = np.stack(np.meshgrid(*[np.arange(n) for n in shape], indexing="ij"), axis=-1).reshape(-1, 3)
    values = field(parts, origin + grid * spacing).reshape(shape)
    flat = lambda i: (i[..., 0] * shape[1] + i[..., 1]) * shape[2] + i[..., 2]  # noqa: E731
    cubes = np.stack(np.meshgrid(*[np.arange(n - 1) for n in shape], indexing="ij"), axis=-1).reshape(-1, 3)
    edges, vertices, triangles = {}, [], []

    def vertex(i, j, vi, vj):
        key = (min(i, j), max(i, j))
        if key not in edges:
            gi, gj = np.array(np.unravel_index(i, shape)), np.array(np.unravel_index(j, shape))
            t = vi / (vi - vj)
            edges[key] = len(vertices)
            vertices.append(origin + (gi + (gj - gi) * t) * spacing)
        return edges[key]

    for tet in TETRAHEDRA:
        corner = cubes[:, None, :] + CUBE[list(tet)][None, :, :]
        index = flat(corner)
        v = values.reshape(-1)[index]
        inside = v < 0.0
        count = inside.sum(axis=1)
        for c in np.nonzero((count > 0) & (count < 4))[0]:
            ins = [k for k in range(4) if inside[c, k]]
            out = [k for k in range(4) if not inside[c, k]]
            ids, vals = index[c], v[c]
            if len(ins) == 1 or len(out) == 1:
                lone, others = (ins[0], out) if len(ins) == 1 else (out[0], ins)
                triangles.append([vertex(ids[lone], ids[o], vals[lone], vals[o]) for o in others])
            else:
                a, b = ins
                e, f = out
                p0, p1, p2, p3 = vertex(ids[a], ids[e], vals[a], vals[e]), vertex(ids[a], ids[f], vals[a], vals[f]), vertex(ids[b], ids[f], vals[b], vals[f]), vertex(ids[b], ids[e], vals[b], vals[e])
                triangles += [[p0, p1, p2], [p0, p2, p3]]
    vertices, triangles = np.array(vertices), np.array(triangles)
    h = 1e-3
    normals = np.stack([field(parts, vertices + e) - field(parts, vertices - e) for e in np.eye(3) * h], axis=1)
    normals /= np.maximum(np.linalg.norm(normals, axis=1), 1e-12)[:, None]
    a, b, c = (vertices[triangles[:, k]] for k in range(3))
    face = np.cross(b - a, c - a)
    area = np.linalg.norm(face, axis=1)
    keep = area > 1e-6
    flip = np.einsum("ij,ij->i", face, normals[triangles].sum(axis=1)) < 0.0
    triangles[flip] = triangles[flip][:, [0, 2, 1]]
    return vertices, normals, triangles[keep]


class Scene:
    """Solids (for the true distance) and open shells (a cuirass: its inside counts as solid within
    cut), with the collision mesh: the solids' union as one closed skin, minus what lies inside a
    shell, and the shells' surfaces where cut keeps them."""

    def __init__(self, solids, shells=(), cut=None):
        self.solids, self.shells, self.cut = list(solids), list(shells), cut
        parts = []
        # Rounded parts as one skin; flat plates (a shield) as their own closed meshes, sharp rims kept.
        groups = [([p for p in self.solids if p.kind != "disc"], False)] + [([p], False) for p in self.solids if p.kind == "disc"]
        groups += [(self.shells, True)] if self.shells else []
        for group, is_shell in groups:
            if group[0].kind == "disc":
                v, t = group[0].mesh()
                n = np.array([gradient(group[0].distance, p) for p in v])
                face = np.cross(v[t[:, 1]] - v[t[:, 0]], v[t[:, 2]] - v[t[:, 0]])
                flip = np.einsum("ij,ij->i", face, n[t].sum(axis=1)) < 0.0
                t[flip] = t[flip][:, [0, 2, 1]]
                t = t[np.linalg.norm(face, axis=1) > 1e-6]
            else:
                v, n, t = isosurface(group)
            centroid = v[t].mean(axis=1)
            if is_shell:
                keep = np.array([cut(p) for p in centroid])
            elif self.shells:
                keep = ~((field(self.shells, centroid) < -0.02) & np.array([cut(p) for p in centroid]))
            else:
                keep = np.ones(len(t), bool)
            parts.append((v, n, t[keep]))
        base, vertices, normals, triangles = 0, [], [], []
        for v, n, t in parts:
            vertices.append(v)
            normals.append(n)
            triangles.append(t + base)
            base += len(v)
        self.vertices, self.normals, self.triangles = np.concatenate(vertices), np.concatenate(normals), np.concatenate(triangles)

    def depth(self, local):
        """How far a point (in the body's frame) lies inside the solids, or a shell's inside."""
        depth = -field(self.solids, local[None])[0]
        if self.shells and self.cut(local):
            depth = max(depth, -field(self.shells, local[None])[0])
        return max(depth, 0.0)

    def distance(self, local):
        """The true signed distance to the union (exact outside)."""
        d = field(self.solids, local[None])[0]
        if self.shells and self.cut(local):
            d = min(d, field(self.shells, local[None])[0])
        return d


def body():
    return Scene([NECK, *TORSO, *SHOULDERS, *ARMS])


def armoured(shield=False):
    return Scene([NECK, *SHOULDERS, *ARMS] + ([SHIELD] if shield else []), CUIRASS, cut=lambda p: CUIRASS_BOTTOM <= p[2] <= CUIRASS_TOP)


def collar_mesh(segments=24):
    """An open, single-sided collar standing off the back of the neck, as on an iron cuirass: a band
    flaring from 6.5 units off the neck's axis to 8.5 as it rises from z 96 to 102, then a lip out to
    10 facing down (its upper face, under 0.6 units away, is the one BodySdf.cpp's sheet rule drops).
    Both face out, and neither has a face behind it: no ray from them leaves through anything."""
    axis = lambda z: np.array([0.0, -1.5 + 0.5 * (z - 88.0) / 15.0, z])  # noqa: E731  NECK's axis
    rings = [(96.0, 6.5), (98.0, 7.17), (100.0, 7.83), (102.0, 8.5), (102.0, 9.25), (102.0, 10.0)]
    angles = np.linspace(math.pi * 1.05, math.pi * 1.95, segments + 1)  # the back half (-Y)
    vertices = np.array([axis(z) + [r * math.cos(a), r * math.sin(a), 0.0] for z, r in rings for a in angles])
    triangles = []
    for i in range(len(rings) - 1):
        for j in range(segments):
            p, q = i * (segments + 1) + j, i * (segments + 1) + j + 1
            triangles += [(p, q, p + segments + 1), (q, q + segments + 1, p + segments + 1)]
    triangles = np.array(triangles)
    # Vertex normals: out and down on the band (its slope), straight down on the lip.
    normals = []
    for i, (z, r) in enumerate(rings):
        for a in angles:
            radial = np.array([math.cos(a), math.sin(a), 0.0])
            n = np.array([0.0, 0.0, -1.0]) if i >= 3 else radial - np.array([0.0, 0.0, 1.0]) * (2.0 / 6.0)
            normals.append(n / np.linalg.norm(n))
    normals = np.array(normals)
    face = np.cross(vertices[triangles[:, 1]] - vertices[triangles[:, 0]], vertices[triangles[:, 2]] - vertices[triangles[:, 0]])
    flip = np.einsum("ij,ij->i", face, normals[triangles].sum(axis=1)) < 0.0
    triangles[flip] = triangles[flip][:, [0, 2, 1]]
    return vertices, normals, triangles


def collared():
    """The bare body with the open collar, a source of its own."""
    made = body()
    v, n, t = collar_mesh()
    made.sources = np.r_[np.zeros(len(made.triangles), int), np.ones(len(t), int)]
    made.triangles = np.concatenate([made.triangles, t + len(made.vertices)])
    made.vertices, made.normals = np.concatenate([made.vertices, v]), np.concatenate([made.normals, n])
    return made


_SCENES = {}
SCENE_MAKERS = {"body": body, "armour": lambda: armoured(False), "shield": lambda: armoured(True), "collar": collared}


def scene(name):
    """A body and its distance field (built once per process; main builds them before forking)."""
    if name not in _SCENES:
        made = SCENE_MAKERS[name]()
        made.sdf = BodySdf(made.vertices, made.normals, made.triangles, getattr(made, "sources", None))
        _SCENES[name] = made
    return _SCENES[name]


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

    def frame(self, head, head_previous, clock, style, colliders=(), reset=False, head_field=None, body=None, body_previous=None, sdf=None):
        """body, body_previous: the body's pose this frame and last; sdf: its distance field (none without)."""
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
        gravity_scale = 0.25 + 0.75 * np.clip((self.length - 4.0) / (FULL_GRAVITY_LENGTH - 4.0), 0.0, 1.0)
        gravity = np.array([0.0, 0.0, -style["gravity"]]) * gravity_scale * h * h
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
        # How far off the body each point is kept, and the deepest it is believed to lie (from its
        # target at the frame's end, where the field is).
        limits = [sdf.limits(target_end[i], body, body_previous) for i in range(n)] if sdf is not None else None
        movable0 = np.arange(n - 1) >= 2
        m0 = np.where(movable0, 0.5, 0.0)
        m1 = np.where(movable0, 0.5, np.where(np.arange(1, n) >= 2, 1.0, 0.0))

        snap_move = np.zeros(3)
        if clock["fraction"] > 0.0:
            frame_move = target_end[0, 2] - target_start[0, 2]
            expected_move = (position[0, 2] - previous[0, 2]) / clock["fraction"]
            excess = frame_move - expected_move
            if excess * frame_move > 0.0 and abs(excess) * clock["fraction"] > vsp_threshold:
                snap_move[2] = np.sign(frame_move) * min(abs(excess), abs(frame_move))
                position += snap_move
                previous += snap_move
                target_start += snap_move
                pp1 += snap_move

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

            # Collision, then the position delta clamp, on the movable points. The body's field is the
            # body at the frame's end: a point goes ahead with the surface to then and back.
            for i in range(2, n):
                collided = False
                for ca, cb, radius in colliders:
                    target_distance = np.linalg.norm(t[i] - closest_on_segment(t[i], ca, cb))
                    allowed = max(min(radius, target_distance), radius * MIN_COLLIDER_DEPTH)
                    hit, pushed = capsule_collision(position[i], previous[i], ca, cb, allowed)
                    if hit:
                        position[i], collided = pushed, True
                contact = None
                contact_delta = position[i] - previous[i]
                contact_speed_squared = contact_delta @ contact_delta
                if contact_speed_squared > clamp * clamp:
                    contact_delta = contact_delta * (clamp * clamp / contact_speed_squared)
                if sdf is not None:
                    # VSP moved the point and its previous position with the root this step.
                    shift = (rotate(q, position[i]) + translation - position[i]) * vsp
                    ahead = sdf.ahead(position[i], f, body, body_previous, snap_move)
                    hit, pushed, normal, frame_move, distance = sdf.collide(position[i], ahead, limits[i], body, body_previous)
                    if hit:
                        surface_move = (frame_move - snap_move) * clock["fraction"]  # the surface's move over the step
                        position[i] = pushed
                        # Into a thin part from the other side in one step (inside it, the field is the
                        # far side's): back where it began on the surface, on the side it came from.
                        start = previous[i] - shift + surface_move
                        before = sdf.sample(start + ahead, body, body_previous)
                        if distance < 0.0 and before is not None and before[1] @ normal < 0.0:
                            position[i] = start + before[1] * max(limits[i][0] - before[0], 0.0)
                            normal = before[1]
                        contact = (normal, surface_move, shift)
                        contact_delta = contact_move(contact_delta, normal, surface_move, shift, limits[i][0] - distance)
                delta = position[i] - previous[i]
                speed_squared = delta @ delta
                if speed_squared > clamp * clamp:
                    previous[i] = position[i] - delta * (clamp * clamp / speed_squared)
                if collided:
                    previous[i] = position[i].copy()
                elif contact is not None:
                    previous[i] = position[i] - contact_delta

            # The head field, as TressFX's signed distance field collision, on the head's pose at the step.
            if head_field is not None:
                head_at = head_previous + (head - head_previous) * f
                head_at[:, 3] += snap_move * (1.0 - f)
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


def run(rest, style, fps, motion, seconds, colliders=(), wind_speed=0.0, jitter=0.0, head_field=None, body_motion=None, body_scene="body"):
    """Frames of (time, drawn points, targets, simulated points, body pose, last frame's body pose).
    jitter varies frame times by up to that fraction. body_motion gives the body's pose (body
    collision on, with body_scene's field), else no body."""
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
        guide.frame(current, previous, clock.frame(dt, wind_speed), style, colliders, head_field=head_field, body=body, body_previous=body_previous,
                    sdf=scene(body_scene).sdf if body_motion else None)
        log.append((s, guide.shown.copy(), guide.target.copy(), guide.position.copy(), body, body_previous))
        previous, body_previous = current, body
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


def on_the_back(points=20, length=30.0, x=0.0):
    """A lock from the back of the head styled lying on the neck and back, 0.05 units off the bare
    body. On the cuirass it lies ARMOUR inside it."""
    lock = np.c_[np.full(points, x), np.full(points, -6.0), 106.0 - length * np.linspace(0.0, 1.0, points)]
    bare = scene("body")
    for p in lock:
        while bare.depth(p) > 0.0:
            p[1] -= 0.02
        p[1] -= 0.05
    return lock


def down_the_back(points=20, length=30.0):
    return on_the_back(points, length)


def to_the_waist(points=32):
    """Long hair: a 45-unit lock styled down the back to the waist."""
    return on_the_back(points, 45.0, x=3.0)


def above_collar(points=12):
    """A short lock at the nape, hanging behind the neck to 1.5 units above the collar's lip: hair
    that never reaches the collar."""
    return np.c_[np.zeros(points), np.full(points, -8.0), np.linspace(109.0, 103.5, points)]


def body_depth(p, pose, name):
    """How far world point p lies inside the true body (or the cuirass's inside) on pose (0 outside)."""
    return scene(name).depth(np.linalg.solve(pose[:, :3], p - pose[:, 3]))


def drawn_on_body(log, name):
    """What StrandSkin.cs.hlsl draws: each frame's drawn points kept off the body once more, from the
    third point of the strand, as far as each one's target lies."""
    sdf = scene(name).sdf
    frames = []
    for r in log:
        points = r[1].copy()
        for i in range(2, len(points)):
            points[i] = sdf.collide(points[i], np.zeros(3), sdf.limits(r[2][i], r[4], r[5]), r[4], r[5])[1]
        frames.append(points)
    return frames


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
    """The body's distance field, built as the GPU builds it from each body's collision mesh, against
    the true distance: close outside the body (where hair rests), never inside where it is outside
    (a push through a limb), and inside where hair styled into armour lies, however deep."""
    results = []
    rng = np.random.default_rng(5)
    pose = head_transform()
    for name in ("body", "armour", "shield"):
        body = scene(name)
        low, high = body.vertices.min(axis=0) - 3.0, body.vertices.max(axis=0) + 3.0
        errors, false_inside, outside, missed, inside = [], 0, 0, 0, 0
        while outside < 3000 or inside < 1000:
            p = rng.uniform(low, high)
            truth = body.distance(p)
            if 0.0 < truth <= 3.0 and outside < 3000:
                outside += 1
                hit = body.sdf.sample(p, pose, pose)
                if hit is not None:
                    if truth <= 1.5:
                        errors.append(hit[0] - truth)
                    false_inside += hit[0] < -0.2
            elif -6.0 <= truth < -0.3 and inside < 1000:
                inside += 1
                hit = body.sdf.sample(p, pose, pose)
                missed += hit is None or hit[0] > 0.0
        errors = np.array(errors)
        p1, p99 = np.percentile(errors, 1), np.percentile(errors, 99)
        ok = -0.45 <= p1 and p99 <= 0.45 and false_inside <= 30 and missed <= 60
        results.append((f"{name} field lies on the body", ok, f"outside within 1.5: {p1:+.2f} to {p99:+.2f} (1st-99th percentile), {false_inside} of 3000 points outside read inside, "
                        f"{missed} of 1000 points up to 6 deep not inside; {len(body.triangles)} triangles"))
    return results


def deepest(frames, log, name):
    """How far the drawn hair went into the body (or the cuirass's inside), after the first frames."""
    return max(max(body_depth(p, r[4], name) for p in drawn[2:]) for drawn, r in zip(frames[5:], log[5:]))


def check_contact_response():
    """Resting contact sticks, sliding dissipates energy, and overlapping contacts cannot turn
    positional corrections into velocity. The shader applies this response to body and card contacts."""
    zero = np.zeros(3)
    up = np.array([0., 0., 1.])
    side = np.array([1., 0., 0.])
    results = []

    # A stationary point pushed out of two intersecting surfaces has no incoming velocity.
    # Previously the first push (X) survived as tangent velocity at the second contact (Z).
    delta = contact_move(zero, side, zero, zero, 1.0)
    delta = contact_move(delta, up, zero, zero, 1.0)
    results.append(("overlapping body and card contacts do not generate sideways velocity", np.linalg.norm(delta) < 1e-8,
                    f"{np.linalg.norm(delta):.6f} units/step after two push-outs"))

    delta = contact_move(np.array([0.01, 0., -0.1]), up, zero, zero, 0.1)
    results.append(("a small resting slide sticks under contact load", np.linalg.norm(delta) < 1e-8,
                    f"{np.linalg.norm(delta):.6f} units/step retained"))

    incoming = np.array([1., 0., -0.1])
    delta = contact_move(incoming, up, zero, zero, 0.1)
    results.append(("a loaded contact allows larger slides and dissipates energy", 0.0 < delta[0] < incoming[0] and abs(delta[2]) < 1e-8,
                    f"{delta[0]:.3f} tangential, {delta[2]:.3f} normal units/step"))

    # Surface carry and the root's VSP share must survive sticking, including pure normal motion.
    for label, surface_move in (("tangential", np.array([0.3, -0.2, 0.])), ("normal", np.array([0., 0., 0.3]))):
        shift = surface_move * 0.4
        delta = contact_move(surface_move - shift, up, surface_move, shift, 0.1)
        error = np.linalg.norm(delta + shift - surface_move)
        results.append((f"sticking keeps {label} surface motion without duplicating VSP", error < 1e-8, f"carry error {error:.6f} units/step"))
    return results


def check_body_collision():
    """Hair keeps off the body's distance field (the body's own shape and what it wears) and comes
    to rest on it."""
    results = []
    style = PRESETS["straight"]

    log = run(through_shoulder(), style, 60, still, 5.0, body_motion=body_still)
    depth, moving = deepest(drawn_on_body(log, "body"), log, "body"), settled_motion(log)
    results.append(("a lock styled through the shoulder lies over it and comes to rest", depth <= 0.3 and moving < 0.005 and deviation(log[-1]) > 2.0,
                    f"{depth:.2f} deep at most, pushed {deviation(log[-1]):.1f} off its style, moving {moving:.4f}/frame"))

    rest = down_the_back()
    free = deviation(run(rest, style, 60, still, 5.0)[-1])
    log = run(rest, style, 60, still, 5.0, body_motion=body_still)
    moving = settled_motion(log)
    results.append(("a lock styled on the back rests as styled", deviation(log[-1]) <= free + 0.1 and moving < 0.005,
                    f"{deviation(log[-1]):.2f} off target (without the body {free:.2f}), moving {moving:.4f}/frame"))

    log = run(rest, style, 60, still, 5.0, body_motion=body_still, body_scene="armour")
    depth, moving = deepest(drawn_on_body(log, "armour"), log, "armour"), settled_motion(log)
    results.append((f"the same lock in a cuirass {ARMOUR} units off the body lies on the cuirass", depth <= 0.3 and moving < 0.005 and deviation(log[-1]) >= ARMOUR,
                    f"{depth:.2f} into the cuirass at most, {deviation(log[-1]):.2f} off its style, moving {moving:.4f}/frame"))

    rest = above_collar()
    sdf, pose = scene("collar").sdf, head_transform()
    deepest_read = min((hit[0] for p in rest if (hit := sdf.sample(p, pose, pose)) is not None), default=np.inf)
    free = deviation(run(rest, style, 60, idle, 4.0)[-1])
    log = run(rest, style, 60, idle, 4.0, body_motion=body_idle, body_scene="collar")
    shake = vibration([r[1] for r in log], 60)
    results.append(("a lock above an open collar is not taken for its inside: it rests as styled", deepest_read > 0.0 and deviation(log[-1]) <= free + 0.1 and shake <= 0.05,
                    f"reads {deepest_read:+.2f} at its deepest, {deviation(log[-1]):.2f} off target (without the body {free:.2f}), shakes up to {shake:.3f}"))

    for label, rest, motion in (("turning", down_the_back(), turn), ("tilting", through_shoulder(), head_tilt)):
        log = run(rest, style, 60, motion, 4.0, body_motion=body_still)
        depth, moving = deepest(drawn_on_body(log, "body"), log, "body"), settled_motion(log)
        results.append((f"{label} the head presses hair onto the shoulder: it stays out and comes to rest", depth <= 0.3 and moving < 0.005,
                        f"{depth:.2f} deep at most, moving {moving:.4f}/frame at the end"))

    return results


def check_body_motion(fps):
    """Hair does not go into the body or shake against it while the body breathes, walks, runs or
    turns: locks on the bare body, and long hair in a cuirass with a shield on the back."""
    results = []
    style = PRESETS["straight"]
    cases = ((("body", down_the_back()), ("body", through_shoulder())), (("shield", to_the_waist()),))
    for label, group in zip(("hair stays out of the body", "hair to the waist stays out of a cuirass and a shield on the back"), cases):
        worst_depth, worst_shake, worst_free = 0.0, 0.0, 0.0
        for name, rest in group:
            for motion, body in ((idle, body_idle), (walk, body_walk), (sprint, sprint), (body_turn, body_turn)):
                log = run(rest, style, fps, motion, 4.0, body_motion=body, body_scene=name)
                drawn = drawn_on_body(log, name)
                worst_depth = max(worst_depth, deepest(drawn, log, name))
                worst_shake = max(worst_shake, vibration(drawn, fps))
                worst_free = max(worst_free, vibration([r[1] for r in run(rest, style, fps, motion, 4.0)], fps))
        results.append((f"breathing, walking, running and turning at {fps} fps: {label} without shaking", worst_depth <= 0.35 and worst_shake <= 0.35,
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


def check_stair_snaps():
    """Repeated stair translations must not kick resting hair or erase an existing swing.

    Compare with the same motion on level ground: stair height is carried with the root,
    while the strand's offsets keep evolving under the same gravity and constraints.
    """
    results = []
    for fps in (30, 60, 144, 240):
        for label, rest in (("scalp", scalp_lock()), ("long", hanging(36.0, 32))):
            for direction in (-1.0, 1.0):
                for motion in (still, small_step):
                    def stairs(s):
                        pose = motion(s).copy()
                        pose[2, 3] += direction * 8.0 * sum(s >= t for t in (0.6, 0.85, 1.1, 1.35))
                        return pose
                    baseline = run(rest, DEFAULT, fps, motion, 2.0, jitter=0.1)
                    log = run(rest, DEFAULT, fps, stairs, 2.0, jitter=0.1)
                    error = max(np.linalg.norm((a[1] - a[2]) - (b[1] - b[2]), axis=1).max() for a, b in zip(log, baseline))
                    results.append((f"{label} hair {'up' if direction > 0 else 'down'} stairs during {motion.__name__}, {fps} fps",
                                    error < 0.1, f"stair snaps change the normal motion by at most {error:.4f} units"))
    return results


def check_stair_contacts():
    """A carried snap must not be carried again by a moving head or body collider."""
    results = []
    field = HeadField()
    for fps in (30, 144):
        for direction in (-1.0, 1.0):
            def stairs(s):
                return head_transform((0, 0, direction * 8.0 * sum(s >= t for t in (0.6, 0.85, 1.1, 1.35))))
            for label, rest, options in (("head", fringe(field), dict(head_field=field)),
                                          ("body", through_shoulder(), dict(body_motion=still))):
                baseline = run(rest, DEFAULT, fps, still, 2.0, jitter=0.1, **options)
                if label == "body":
                    options = dict(body_motion=stairs)
                log = run(rest, DEFAULT, fps, stairs, 2.0, jitter=0.1, **options)
                error = max(np.linalg.norm((a[1] - a[2]) - (b[1] - b[2]), axis=1).max() for a, b in zip(log, baseline))
                results.append((f"{label} contact follows stairs {'up' if direction > 0 else 'down'}, {fps} fps",
                                error < 0.1, f"stair snaps change colliding hair's normal motion by at most {error:.4f} units"))
    return results


def check_running_stop():
    """A long loose lock must return promptly after a sprint, rather than drift in slow motion.

    At gravity 100 and ten length passes, the 36-unit straight lock is still 10.8 units
    from rest 0.4 s after stopping. Raising VSP to 0.5 only reduces that to 9.7 units.
    """
    results = []
    for name in ("straight", "wavy"):
        for fps in (30, 60, 144):
            rest = hanging(36.0, 32)
            log = run(rest, PRESETS[name], fps, sprint, 4.0, jitter=0.1)
            resting_tip = log[-1][1][-1] - log[-1][2][-1]
            def distance_from_rest(row):
                return np.linalg.norm(row[1][-1] - row[2][-1] - resting_tip)
            returning = distance_from_rest(at(log, 2.0))
            recovered = max(distance_from_rest(row) for row in log if row[0] >= 2.7)
            ok = returning < 8.0 and recovered < 1.0
            results.append((f"{name} long lock returns promptly after a running stop, {fps} fps", ok,
                            f"{returning:.2f} units from rest after 0.4 s, at most {recovered:.2f} after 1.1 s"))
    return results


def check_wind():
    results = []
    style = PRESETS["straight"]
    for speed in (0.3, 1.0):
        log = run(hanging(20.0, 20), style, 60, still, 8.0, wind_speed=speed)
        calm_row = run(hanging(20.0, 20), style, 60, still, 8.0)[-1]
        calm = deviation(calm_row)
        worst = max(deviation(r) for r in log[len(log) // 2:])
        # Measure actual movement from calm hair: sideways wind can move a sagging lock
        # substantially without increasing its scalar distance from the styled target much.
        blown = max(np.linalg.norm(r[1] - calm_row[1], axis=1).max() for r in log[len(log) // 2:])
        ok = blown > 0.2 and worst < 12.0 and all(np.isfinite(r[1]).all() for r in log) and max(stretch(r) for r in log) < 1.2
        results.append((f"wind at {speed:.0%} of full moves a 20-unit lock, within bounds", ok,
                        f"up to {worst:.2f} off target (calm {calm:.2f}), {blown:.2f} from calm hair"))
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
    if kind == "body motion":
        return check_body_motion(arg)
    return {"contact response": check_contact_response, "collision": check_collision, "head field": check_head_field, "body field": check_body_field, "body collision": check_body_collision,
            "short locks": check_short_locks, "swings": check_swings, "stair snaps": check_stair_snaps, "stair contacts": check_stair_contacts, "running stop": check_running_stop,
            "wind": check_wind, "extremes": check_extremes, "followers": check_followers}[kind]()


def main():
    # The bodies' fields, once, before the workers fork (about half a minute each).
    for name in SCENE_MAKERS:
        scene(name)
    tasks = [(kind, name) for name in PRESETS for kind in ("rest", "motion", "held")]
    tasks += [("rates", "straight"), ("rates", "locs")]
    tasks += [("body motion", fps) for fps in (144, 60, 30)]
    tasks += [(kind, None) for kind in ("contact response", "body collision", "collision", "head field", "body field", "short locks", "swings", "stair snaps", "stair contacts", "running stop", "wind", "extremes", "followers")]
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
