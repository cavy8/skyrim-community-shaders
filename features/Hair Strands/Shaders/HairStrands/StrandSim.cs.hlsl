// Hair Strands: simulates the guide strands, one thread per guide. Every other strand follows
// its guide in StrandSkin.cs.hlsl.
//
// Each guide point chases a target: the point skinned by the head alone, blended towards its
// full skinning (SMP and other bones) by Guidance. Per frame, last frame's state is carried
// along with (1 - inertia) of what the bones did since. Then, in steps of at most 1/60 s with
// the targets moving from last frame's pose to this frame's, the points are integrated (Verlet
// with damping, gravity and wind) and pulled back into shape over a few iterations of a global
// shape constraint (towards the target, strongest at the root), a local shape constraint (each
// segment keeps its bend relative to the one before it), follow-the-leader length constraints
// and collision: with the head field (the actor's own head mesh, see Skinning.hlsli) and with
// capsules down the body. The root stays on its target.
//
// The global shape stiffness falls from the root's to the tip's over FreeLength units of strand,
// or over the whole strand if it is longer. Loads (air drag, gravity as the head tilts, wind,
// the head's own acceleration) move points by about the same distance on any strand, so with the
// fall-off spread over each strand's own length a short lock swung through a far larger angle
// than a long one, and its tip hooked over. A short lock now has the stiffness of the same
// length of long hair near its root.
//
// Stiffness is authored per 1/60 s. Steps shorter than that (high frame rates) scale it as a
// compliance (XPBD), and give back the damping the 60 Hz projections carry on the motion
// relative to the target, so the hair moves the same at any frame rate.
//
// Velocity is the step's change in position, with bending motion damped. Dynamic
// follow-the-leader (Mueller et al. 2012) also takes each length correction back out of the
// velocity of the point before it; under a steady load (tilted head, wind) those corrections
// never stop, and with the shape and bend constraints that velocity kept the tips shaking at
// 5-8 Hz. Swings are damped instead by SwingDamping (the style's damping), which only ever
// takes energy out. Air drag (VelocityKeep) acts on world velocity: it is what makes hair trail
// while running, so it stays small.
//
// The length and local shape constraints only move each segment's far point, so a swing's
// corrections run on down the strand: after the head stopped, a wave ran to the tip and back
// for about a second. Moving both ends instead stops that wave, but under a heavy load (a head on
// its side, a lock with short segments) the strand then never settles, and jitters.
// Instead, BendDamping takes out bending motion: relative to its target, each point loses that
// share of the difference from the velocity it would have if its segment turned with the one
// before it. A swing of the whole strand keeps its speed, and velocity alone is touched, so a
// strand at rest under any load stays at rest.

#include "HairStrands/Skinning.hlsli"

RWStructuredBuffer<HairStrands::GuidePoint> Guides : register(u0);

// GeneratorLimits::kMaxPointsPerStrand.
#define MAX_POINTS 32

namespace HairStrandsSim
{
	// Bending motion (each segment turning against the one before it) lost per 1/60 s: waves
	// down the strand die at once, and swings of whole locks are left alone.
	static const float BendDamping = 0.8;
	static const float MaxSpeed = 3000.0;  // units/s
	// Strand length over which the shape stiffness falls from the root's to the tip's (about
	// 28 cm, shoulder length): only hair further than this from the scalp swings fully free.
	static const float FreeLength = 20.0;  // units
	static const float SixtiethOfASecond = 1.0 / 60.0;

	// Stiffness authored for a 1/60 s step, for a step of a_step seconds (compliance form).
	float StepStiffness(float a_perSixtieth, float a_step)
	{
		if (a_perSixtieth >= 1.0)
			return 1.0;
		if (a_perSixtieth <= 0.0)
			return 0.0;
		const float compliance = SixtiethOfASecond * SixtiethOfASecond * (1.0 - a_perSixtieth) / a_perSixtieth;
		return a_step * a_step / (a_step * a_step + compliance);
	}

	float PerIteration(float a_perStep)
	{
		return 1.0 - pow(saturate(1.0 - a_perStep), 1.0 / max((float)Iterations, 1.0));
	}

	float3 ClosestOnSegment(float3 a_p, float3 a_a, float3 a_b)
	{
		const float3 ab = a_b - a_a;
		const float lengthSquared = dot(ab, ab);
		const float t = lengthSquared > 1e-8 ? saturate(dot(a_p - a_a, ab) / lengthSquared) : 0.0;
		return a_a + ab * t;
	}

	float3 Collide(float3 a_p, float3 a_target)
	{
		[loop] for (uint c = 0; c < ColliderCount; ++c)
		{
			const float3 a = Colliders[c * 2].xyz;
			const float radius = Colliders[c * 2].w;
			const float3 b = Colliders[c * 2 + 1].xyz;
			const float3 closest = ClosestOnSegment(a_p, a, b);
			const float3 away = a_p - closest;
			const float distance = length(away);
			const float targetDistance = length(a_target - ClosestOnSegment(a_target, a, b));
			const float allowed = max(min(radius, targetDistance), radius * HairStrandsSkin::MinColliderDepth);
			if (distance < allowed)
				a_p = closest + HairStrandsSkin::SafeNormalize(away, HairStrandsSkin::SafeNormalize(a_target - closest, float3(0, 0, 1))) * allowed;
		}
		return a_p;
	}

	// Slow, uneven gusts, out of step from strand to strand; in [0.1, 1].
	float Gust(float a_random)
	{
		const float phase = a_random * 6.2831853;
		return 0.55 + 0.3 * sin(Time * 1.7 + phase) + 0.15 * sin(Time * 4.3 + phase * 3.0);
	}
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const uint guide = dispatchID.x;
	if (guide >= GuideCount)
		return;
	const uint n = min(PointsPerStrand, MAX_POINTS);
	const uint base = guide * PointsPerStrand;
	const float lastIndex = max((float)n - 1.0, 1.0);
	// How far along the stiffness fall-off the tip gets: all the way on strands of FreeLength or longer.
	const float reach = saturate(Strands[guide].Length / HairStrandsSim::FreeLength);
	const float3 previousToCurrent = EyeShift - PreviousEyeShift;  // previous frame's camera to this frame's

	float3 x[MAX_POINTS];
	float3 velocity[MAX_POINTS];
	float3 target[MAX_POINTS];
	float3 startTarget[MAX_POINTS];  // where the targets start this frame's steps
	float3 start[MAX_POINTS];        // position before a step's integration
	float stiffness[MAX_POINTS];     // shape stiffness per iteration
	float relativeKeep[MAX_POINTS];  // velocity relative to the target kept per step
	uint i;

	// The head field rides the head bone: this frame's pose for every step.
	const bool collideHead = (Flags & HAIR_STRANDS_FLAG_HEAD_FIELD) != 0;
	const HairStrandsSkin::HeadFrame head = HairStrandsSkin::LoadHeadFrame(0);

	// A root that jumped (teleport, load, animation snap) restarts the whole strand.
	bool reset = (Flags & HAIR_STRANDS_FLAG_RESET) != 0;
	if (!reset) {
		float3x4 current, previous, targetCurrent, targetPrevious;
		const HairStrands::RestPoint root = RestPoints[base];
		HairStrandsSkin::Skin(root, current, previous);
		HairStrandsSkin::TargetSkin(current, previous, targetCurrent, targetPrevious);
		reset = !(distance(mul(targetCurrent, float4(root.Position, 1.0)), Guides[base].Position + EyeShift) <= TeleportDistance);
	}

	// Targets, and last frame's state carried along with (1 - inertia) of the bones' motion.
	[loop] for (i = 0; i < n; ++i)
	{
		const HairStrands::RestPoint rest = RestPoints[base + i];
		float3x4 current, previous, targetCurrent, targetPrevious;
		HairStrandsSkin::Skin(rest, current, previous);
		HairStrandsSkin::TargetSkin(current, previous, targetCurrent, targetPrevious);
		const float4 restPosition = float4(rest.Position, 1.0);
		target[i] = mul(targetCurrent, restPosition);
		const float3 previousTarget = mul(targetPrevious, restPosition);  // relative to the previous frame's camera

		const HairStrands::GuidePoint old = Guides[base + i];
		float3 previousPosition = previousTarget;
		if (reset) {
			x[i] = target[i];
			velocity[i] = 0;
			startTarget[i] = target[i];
		} else {
			const float3x3 motion = mul((float3x3)targetCurrent, HairStrandsSkin::Inverse((float3x3)targetPrevious));
			const float3 position = old.Position + EyeShift;
			const float3 lastTarget = previousTarget + previousToCurrent;
			const float3 carried = target[i] + mul(motion, position - lastTarget);
			x[i] = lerp(position, carried, Carry);
			velocity[i] = lerp(old.Velocity, mul(motion, old.Velocity), Carry);
			// The carry already moved the state that much of the way: the targets cover the rest.
			startTarget[i] = lerp(lastTarget, target[i], Carry);
			previousPosition = old.Position + PreviousEyeShift;
		}
		// Unconditional writes: fxc cannot map partial UAV writes in this branch (X4532).
		Guides[base + i].PreviousTarget = previousTarget;
		Guides[base + i].PreviousPosition = previousPosition;

		const float shape = lerp(RootStiffness, TipStiffness, i / lastIndex * reach);
		const float shapeStep = HairStrandsSim::StepStiffness(shape, StepTime);
		stiffness[i] = HairStrandsSim::PerIteration(shapeStep);
		const float total = 1.0 - (1.0 - shape) * (1.0 - BendStiffness);
		const float totalStep = 1.0 - (1.0 - shapeStep) * (1.0 - HairStrandsSim::StepStiffness(BendStiffness, StepTime));
		relativeKeep[i] = total < 1.0 ? min(pow(saturate(1.0 - total), StepTime * 60.0) / max(1.0 - totalStep, 1e-6), 1.0) : 0.0;
		relativeKeep[i] *= pow(saturate(1.0 - SwingDamping), StepTime * 60.0);
	}

	const bool dynamics = Steps > 0 && StepTime > 0.0 && !reset;
	if (dynamics) {
		const float h = StepTime;
		const float bend = HairStrandsSim::PerIteration(HairStrandsSim::StepStiffness(BendStiffness, h));
		const float bendKeep = pow(1.0 - HairStrandsSim::BendDamping, h * 60.0);  // bending motion kept per step
		const float gust = HairStrandsSim::Gust(Strands[guide].Random);
		const bool collide = (Flags & HAIR_STRANDS_FLAG_COLLIDE) != 0 && ColliderCount > 0;

		[loop] for (uint step = 0; step < Steps; ++step)
		{
			const float fromFraction = (float)step / Steps;
			const float toFraction = (float)(step + 1) / Steps;

			// Integrate. Motion relative to the target is damped by what the 60 Hz projections
			// would take out and by the swing damping; all motion loses the air drag.
			[loop] for (i = 1; i < n; ++i)
			{
				const float3 stepTarget = lerp(startTarget[i], target[i], toFraction);
				const float3 targetVelocity = (stepTarget - lerp(startTarget[i], target[i], fromFraction)) / h;
				float3 v = (targetVelocity + (velocity[i] - targetVelocity) * relativeKeep[i]) * VelocityKeep;
				// Wind pushes across the strand, harder towards the tip.
				const float3 along = HairStrandsSkin::SafeNormalize(x[i] - x[i - 1], 0);
				float3 wind = Wind * (gust * i / lastIndex);
				wind -= along * dot(wind, along);
				start[i] = x[i];
				x[i] += v * h + (Gravity + wind) * (h * h);
			}
			x[0] = lerp(startTarget[0], target[0], toFraction);

			[loop] for (uint iteration = 0; iteration < Iterations; ++iteration)
			{
				[loop] for (i = 1; i < n; ++i)
					x[i] = lerp(x[i], lerp(startTarget[i], target[i], toFraction), stiffness[i]);

				[loop] for (i = 0; i + 1 < n; ++i)
				{
					const float3 restA = lerp(startTarget[i], target[i], toFraction);
					const float3 restB = lerp(startTarget[i + 1], target[i + 1], toFraction);
					float3 desired = restB - restA;
					if (i > 0) {
						const float3 restParent = HairStrandsSkin::SafeNormalize(restA - lerp(startTarget[i - 1], target[i - 1], toFraction), 0);
						const float3 parent = HairStrandsSkin::SafeNormalize(x[i] - x[i - 1], restParent);
						if (dot(restParent, restParent) > 0.5)
							desired = HairStrandsSkin::Rotate(HairStrandsSkin::ShortestArc(restParent, parent), desired);
					}
					x[i + 1] = lerp(x[i + 1], x[i] + desired, bend);
				}

				[loop] for (i = 1; i < n; ++i)
				{
					const float3 stepTarget = lerp(startTarget[i], target[i], toFraction);
					const float3 restSegment = stepTarget - lerp(startTarget[i - 1], target[i - 1], toFraction);
					const float3 direction = HairStrandsSkin::SafeNormalize(x[i] - x[i - 1], HairStrandsSkin::SafeNormalize(restSegment, float3(0, 0, -1)));
					float3 fixedPoint = x[i - 1] + direction * length(restSegment);
					if (collide)
						fixedPoint = HairStrandsSim::Collide(fixedPoint, stepTarget);
					if (collideHead)
						fixedPoint = HairStrandsSkin::CollideHead(fixedPoint, HairStrandsSkin::HeadDepth(stepTarget, head), head);
					x[i] = fixedPoint;
				}
			}

			[loop] for (i = 1; i < n; ++i)
				velocity[i] = (x[i] - start[i]) / h;
			velocity[0] = 0;

			// Bending motion: relative to the targets (whose velocity is constant over the frame),
			// pull each point towards the velocity it would have if its segment turned with the
			// one before it. Root to tip, so each point sees its parent's damped velocity.
			const float frameTime = h * Steps;
			float3 before = 0;  // the root follows its target
			float3 relative = velocity[1] - (target[1] - startTarget[1]) / frameTime;
			[loop] for (i = 1; i + 1 < n; ++i)
			{
				const float3 parent = x[i] - x[i - 1];
				const float3 targetVelocity = (target[i + 1] - startTarget[i + 1]) / frameTime;
				float3 after = velocity[i + 1] - targetVelocity;
				const float parentSquared = dot(parent, parent);
				if (parentSquared > 1e-12) {
					const float3 turn = cross(parent, relative - before) / parentSquared;
					const float3 rigid = relative + cross(turn, x[i + 1] - x[i]);
					after = lerp(rigid, after, bendKeep);
					velocity[i + 1] = targetVelocity + after;
				}
				before = relative;
				relative = after;
			}

			[loop] for (i = 1; i < n; ++i)
			{
				const float speed = length(velocity[i]);
				if (speed > HairStrandsSim::MaxSpeed)
					velocity[i] *= HairStrandsSim::MaxSpeed / speed;
			}
		}
	}

	[loop] for (i = 0; i < n; ++i)
	{
		if (any(!isfinite(x[i])) || any(!isfinite(velocity[i]))) {
			x[i] = target[i];
			velocity[i] = 0;
		}
	}
	[loop] for (i = 0; i < n; ++i)
	{
		const uint a = i > 0 ? i - 1 : 0;
		const uint b = min(i + 1, n - 1);
		const float3 restTangent = HairStrandsSkin::SafeNormalize(target[b] - target[a], float3(0, 0, -1));
		const float3 tangent = HairStrandsSkin::SafeNormalize(x[b] - x[a], restTangent);
		Guides[base + i].Rotation = HairStrandsSkin::ShortestArc(restTangent, tangent);
		Guides[base + i].Position = x[i];
		Guides[base + i].Velocity = velocity[i];
		Guides[base + i].Target = target[i];
	}
}
