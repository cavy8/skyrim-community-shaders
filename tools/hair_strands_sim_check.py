"""Checks the Hair Strands guide solver on a NumPy port of StrandSim.cs.hlsl.

One 20-unit lock hangs from a head that stays still, sprints and stops, turns fast, bows,
and turns into a shoulder capsule, at 30, 60, 144 and 240 fps. The solver must stay exactly
on its target while still, never stretch, lag the same at every frame rate, and settle.
Then followers beside and longer than a short guide, through idle sway and snap turns, must
stray no further than their guide and keep their length.

Keep the port in step with features/Hair Strands/Shaders/HairStrands/StrandSim.cs.hlsl,
StrandSkin.cs.hlsl and StrandRenderer::PrepareSimulation; pass --verbose for the time series.
Exit code 1 on failure.

    python tools/hair_strands_sim_check.py
"""

import math
import sys

import numpy as np

ITERATIONS = 3  # kSimIterations
STEP = 1.0 / 60.0  # kSimStep
MAX_FRAME = 1.0 / 30.0  # kMaxFrameTime
GRAVITY = 687.0  # kGravity
LENGTH_DAMPING = 0.9
MAX_SPEED = 3000.0
MIN_COLLIDER_DEPTH = 0.5
TELEPORT = 40.0

STRAIGHT = dict(root=0.5, tip=0.03, bend=0.35, damping=0.06, gravity=1.0, inertia=0.85)
LOCS = dict(root=0.45, tip=0.02, bend=0.3, damping=0.04, gravity=1.2, inertia=0.9)


def normalize(v, fallback):
    lsq = v @ v
    return v / math.sqrt(lsq) if lsq > 1e-12 else fallback


def rotate(q, v):
    u = q[:3]
    return v + 2.0 * np.cross(u, np.cross(u, v) + q[3] * v)


def shortest_arc(a, b):
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


def collide(p, target, colliders):
    for a, b, radius in colliders:
        closest = closest_on_segment(p, a, b)
        away = p - closest
        target_distance = np.linalg.norm(target - closest_on_segment(target, a, b))
        allowed = max(min(radius, target_distance), radius * MIN_COLLIDER_DEPTH)
        if np.linalg.norm(away) < allowed:
            p = closest + normalize(away, normalize(target - closest, np.array([0, 0, 1.0]))) * allowed
    return p


def step_stiffness(per_sixtieth, step):
    if per_sixtieth >= 1.0:
        return 1.0
    if per_sixtieth <= 0.0:
        return 0.0
    compliance = STEP * STEP * (1.0 - per_sixtieth) / per_sixtieth
    return step * step / (step * step + compliance)


def per_iteration(per_step):
    return 1.0 - (1.0 - per_step) ** (1.0 / ITERATIONS)


class Guide:
    def __init__(self, rest):
        self.rest = np.c_[rest, np.ones(len(rest))]
        self.x = None
        self.v = np.zeros((len(rest), 3))

    def frame(self, head, head_previous, dt, style, colliders=()):
        """One frame for one guide: head and head_previous are 3x4 skin-to-world transforms."""
        n = len(self.rest)
        last = n - 1
        steps = math.ceil(dt / STEP - 1e-3) if dt > 0 else 0
        h = dt / steps if steps else 0.0
        target = self.rest @ head.T
        previous_target = self.rest @ head_previous.T
        if self.x is None or not np.linalg.norm(target[0] - self.x[0]) <= TELEPORT:
            self.x, self.v, self.target = target.copy(), np.zeros_like(target), target
            return

        # Carry, then the per-point stiffness and the damping the 60 Hz projections give.
        carry = 1.0 - style["inertia"]
        motion = head[:, :3] @ np.linalg.inv(head_previous[:, :3])
        x = np.empty_like(target)
        v = np.empty_like(target)
        start_target = np.empty_like(target)
        stiffness = np.empty(n)
        relative_keep = np.empty(n)
        for i in range(n):
            carried = target[i] + motion @ (self.x[i] - previous_target[i])
            x[i] = self.x[i] + (carried - self.x[i]) * carry
            v[i] = self.v[i] + (motion @ self.v[i] - self.v[i]) * carry
            start_target[i] = previous_target[i] + (target[i] - previous_target[i]) * carry
            shape = style["root"] + (style["tip"] - style["root"]) * i / last
            shape_step = step_stiffness(shape, h)
            stiffness[i] = per_iteration(shape_step)
            total = 1.0 - (1.0 - shape) * (1.0 - style["bend"])
            total_step = 1.0 - (1.0 - shape_step) * (1.0 - step_stiffness(style["bend"], h))
            relative_keep[i] = min((1.0 - total) ** (h * 60.0) / max(1.0 - total_step, 1e-6), 1.0) if total < 1.0 else 0.0

        down = -head[:, 2] / np.linalg.norm(head[:, 2])
        gravity = (np.array([0, 0, -1.0]) - down) * GRAVITY * style["gravity"]
        keep = (1.0 - style["damping"]) ** (h * 60.0)
        bend = per_iteration(step_stiffness(style["bend"], h))
        for k in range(steps):
            at = lambda f: start_target + (target - start_target) * f
            step_from, step_target = at(k / steps), at((k + 1) / steps)
            start = x.copy()
            for i in range(1, n):
                target_velocity = (step_target[i] - step_from[i]) / h
                vi = (target_velocity + (v[i] - target_velocity) * relative_keep[i]) * keep
                x[i] = x[i] + vi * h + gravity * h * h
            x[0] = step_target[0]
            correction = np.zeros_like(x)
            for _ in range(ITERATIONS):
                for i in range(1, n):
                    x[i] += (step_target[i] - x[i]) * stiffness[i]
                for i in range(n - 1):
                    desired = step_target[i + 1] - step_target[i]
                    if i > 0:
                        rest_parent = normalize(step_target[i] - step_target[i - 1], np.zeros(3))
                        parent = normalize(x[i] - x[i - 1], rest_parent)
                        if rest_parent @ rest_parent > 0.5:
                            desired = rotate(shortest_arc(rest_parent, parent), desired)
                    x[i + 1] += (x[i] + desired - x[i + 1]) * bend
                for i in range(1, n):
                    segment = step_target[i] - step_target[i - 1]
                    fixed = x[i - 1] + normalize(x[i] - x[i - 1], normalize(segment, np.array([0, 0, -1.0]))) * np.linalg.norm(segment)
                    if colliders:
                        fixed = collide(fixed, step_target[i], colliders)
                    correction[i - 1] += fixed - x[i]
                    x[i] = fixed
            for i in range(1, n):
                vi = (x[i] - start[i] - LENGTH_DAMPING * correction[i]) / h
                speed = np.linalg.norm(vi)
                v[i] = vi * (MAX_SPEED / speed) if speed > MAX_SPEED else vi
            v[0] = 0
        self.x, self.v, self.target = x, v, target


def head_transform(translation=(0, 0, 0), pitch=0.0, yaw=0.0, pivot=None):
    p, y = math.radians(pitch), math.radians(yaw)
    rx = np.array([[1, 0, 0], [0, math.cos(p), -math.sin(p)], [0, math.sin(p), math.cos(p)]])
    rz = np.array([[math.cos(y), -math.sin(y), 0], [math.sin(y), math.cos(y), 0], [0, 0, 1]])
    r = rz @ rx
    t = np.array(translation, dtype=float)
    if pivot is not None:
        t = t + np.asarray(pivot) - r @ np.asarray(pivot)
    return np.c_[r, t]


def still(_):
    return head_transform()


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


def run(style, fps, motion, seconds, colliders=()):
    t = np.linspace(0.0, 1.0, 20)
    rest = np.c_[np.zeros(20), -3.0 - 2.0 * np.sin(t * math.pi * 0.5), 120.0 - 20.0 * t]
    guide = Guide(rest)
    dt = min(1.0 / fps, MAX_FRAME)
    previous = motion(0.0)
    guide.frame(previous, previous, dt, style)
    log = []
    for k in range(1, int(seconds * fps) + 1):
        current = motion(k / fps)
        guide.frame(current, previous, dt, style, colliders)
        previous = current
        deviation = np.linalg.norm(guide.x - guide.target, axis=1).max()
        stretch = np.abs(np.linalg.norm(np.diff(guide.x, axis=0), axis=1) - np.linalg.norm(np.diff(guide.target, axis=0), axis=1)).max()
        log.append((k / fps, deviation, stretch, guide.x.copy()))
    return log


def at(log, seconds):
    return min(log, key=lambda row: abs(row[0] - seconds))


def idle(s):
    """Breathing and idle sway: a 0.6-unit bob and a 3 degree nod."""
    return head_transform((0, 0, 0.6 * math.sin(2 * math.pi * 0.4 * s)), pitch=3.0 * math.sin(2 * math.pi * 0.3 * s), pivot=(0, 0, 110.0))


def snap(s):
    """A third-person character snapping to the camera's heading: 90 degrees in 0.05 s, every second."""
    return head_transform(yaw=90.0 * (int(s) + min((s - int(s)) / 0.05, 1.0)))


def lock(length, offset, points=14):
    t = np.linspace(0.0, 1.0, points)
    return np.c_[np.full(points, offset[0]), offset[1] - 3.0 - 1.5 * np.sin(t * math.pi * 0.5), 112.0 + offset[2] - length * t]


def follow(guide, guide_length, target, length):
    """StrandSkin.cs.hlsl: a follower moves off its target as far as its guide has at the same distance from the root."""
    n = len(guide.x)
    along = np.clip(np.linspace(0.0, 1.0, len(target)) * length / guide_length, 0.0, 1.0) * (n - 1)
    j = np.minimum(along.astype(int), n - 2)
    w = (along - j)[:, None]
    displacement = guide.x - guide.target
    return target + displacement[j] * (1.0 - w) + displacement[j + 1] * w


def run_follower(motion, fps, seconds, length, offset):
    """A short guide (vanilla-style cards: 3.8 units, 14 points) and one follower beside it."""
    guide = Guide(lock(3.8, (0, 0, 0)))
    rest = np.c_[lock(length, offset), np.ones(14)]
    dt = 1.0 / fps
    previous = motion(0.0)
    guide.frame(previous, previous, dt, STRAIGHT)
    worst_guide, worst_follower, lengths = 0.0, 0.0, []
    for k in range(1, int(seconds * fps) + 1):
        current = motion(k / fps)
        guide.frame(current, previous, dt, STRAIGHT)
        previous = current
        target = rest @ current.T
        x = follow(guide, 3.8, target, length)
        worst_guide = max(worst_guide, np.linalg.norm(guide.x - guide.target, axis=1).max())
        worst_follower = max(worst_follower, np.linalg.norm(x - target, axis=1).max())
        lengths.append(np.linalg.norm(np.diff(x, axis=0), axis=1).sum() / np.linalg.norm(np.diff(target, axis=0), axis=1).sum())
    return worst_guide, worst_follower, min(lengths), max(lengths)


def main():
    verbose = "--verbose" in sys.argv
    failures = []

    def check(name, ok, detail):
        print(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}")
        if not ok:
            failures.append(name)

    log = run(STRAIGHT, 60, still, 2.0)
    check("still hair stays on target", max(r[1] for r in log) < 1e-4, f"max deviation {max(r[1] for r in log):.2e}")

    for name, style in (("straight", STRAIGHT), ("locs", LOCS)):
        lags, settled = {}, {}
        for fps in (30, 60, 144, 240):
            log = run(style, fps, sprint, 4.0)
            lags[fps] = at(log, 1.0)[1]
            settled[fps] = at(log, 4.0)[1]
            stretch = max(r[2] for r in log)
            check(f"{name} sprint {fps} fps does not stretch", stretch < 1e-3, f"max stretch {stretch:.2e}")
            if verbose:
                print("      " + "  ".join(f"t={m:.1f} {at(log, m)[1]:5.2f}" for m in (0.3, 1.0, 1.6, 2.0, 2.5, 3.0, 4.0)))
        spread = max(abs(lag / lags[60] - 1.0) for lag in lags.values())
        check(f"{name} running lag is the same at 30-240 fps", spread < 0.25, ", ".join(f"{fps}: {lag:.2f}" for fps, lag in lags.items()))
        check(f"{name} settles after stopping", max(settled.values()) < 0.1, ", ".join(f"{fps}: {d:.3f}" for fps, d in settled.items()))

    for fps in (60, 144):
        log = run(STRAIGHT, fps, turn, 3.0)
        check(f"fast turn {fps} fps settles", at(log, 3.0)[1] < 0.1, f"deviation {max(r[1] for r in log):.2f} peak, {at(log, 3.0)[1]:.3f} at 3 s")
        log = run(STRAIGHT, fps, bow, 5.0)
        check(f"bow {fps} fps swings and recovers", at(log, 1.9)[1] > 0.5 and at(log, 5.0)[1] < 0.1, f"{at(log, 1.9)[1]:.2f} while bowed, {at(log, 5.0)[1]:.3f} at 5 s")

    shoulder = [(np.array([4.0, -3.0, 104.0]), np.array([14.0, -3.0, 104.0]), 3.5)]
    log = run(STRAIGHT, 60, turn, 3.0, shoulder)
    closest = min(min(np.linalg.norm(p - closest_on_segment(p, shoulder[0][0], shoulder[0][1])) for p in r[3]) for r in log)
    check("collision keeps points out of the capsule core", closest >= 3.5 * MIN_COLLIDER_DEPTH - 1e-3, f"closest {closest:.2f} (floor {3.5 * MIN_COLLIDER_DEPTH:.2f})")

    # Followers must not amplify their guide: turning the offset from the guide with the
    # guide's bend made a follower beside a 3.8-unit guide stray 12x as far and stretch 35%.
    for motion_name, motion in (("idle", idle), ("snap turn", snap)):
        for length, offset in ((3.8, (0, 2.5, 0)), (8.0, (1.5, 0, 0))):
            guide_dev, follower_dev, shortest, longest = run_follower(motion, 60, 3.0, length, offset)
            ok = follower_dev <= guide_dev + 1e-3 and 0.9 <= shortest and longest <= 1.1
            check(f"{motion_name}: {length}-unit follower tracks its 3.8-unit guide", ok,
                  f"guide {guide_dev:.2f}, follower {follower_dev:.2f} off target, length x{shortest:.2f}-{longest:.2f}")

    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
