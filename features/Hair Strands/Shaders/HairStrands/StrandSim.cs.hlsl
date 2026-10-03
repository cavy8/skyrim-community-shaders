// Hair Strands: simulates the guide strands, one thread per guide, as TressFX 4.1 does. Every
// other strand follows its guide in StrandSkin.cs.hlsl, as TressFX's follow hairs follow theirs.
// Based on https://github.com/GPUOpen-Effects/TressFX/blob/master/src/Shaders/TressFXSimulation.hlsl
//
// Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// A step is TressFX's simulation pass for one strand, in TressFX's order and with its maths:
//  1. IntegrationAndGlobalShapeConstraints: Verlet integration with gravity and exponential
//     damping, the first two points pinned to their targets (the skinned rest positions), then the
//     points within the global range of the root pulled towards their targets.
//  2. CalculateStrandLevelData and VelocityShockPropagation: the rotation and translation that
//     took the root segment from the last step to this one moves the rest of the strand, current
//     and previous positions alike, by the VSP coefficient (1 when the second point's
//     pseudo-acceleration passes the threshold, as on teleports).
//  3. LocalShapeConstraints: each segment is pulled towards its rest direction relative to the
//     segment before it, turned by the shortest arc from that segment's rest direction to its
//     current one.
//  4. LengthConstriantsWindAndCollision: wind across each segment, distance constraints in even
//     then odd pairs, capsule collision (which stops the point), and the position delta clamp.
//  5. TressFX's collision with the body's signed distance field (CollideHairVerticesWithSdf):
//     here the body's distance field, built this frame from what the actor wears
//     (BodySdf.cs.hlsl), and the head field, the actor's own head mesh (see Skinning.hlsli). A
//     point inside is put back on the surface. TressFX runs it after the simulation on every point
//     but the first two, followers included; StrandSkin.cs.hlsl does the followers.
// TressFX runs these as separate dispatches over every vertex or strand; one thread runs a whole
// strand here, and each pass is a loop with the same result.
//
// Where this differs from TressFX:
//  - TressFX runs one pass per frame of whatever length, so its motion changes with frame rate.
//    Here steps are a fixed 1/60 s from a clock shared by all hair (TressFX's own samples ran
//    at 60 Hz). The targets move from last frame's pose to this frame's across the frame, and
//    each step takes them at the time it ends. The strands are drawn with their offset from the
//    target interpolated between the last two steps, so the hair moves smoothly at any frame rate
//    while its roots stay on this frame's skinning.
//  - Targets are the hair's own skinning (the head, blended towards SMP bones by Guidance), per
//    point rather than TressFX's one skinning per strand; the local shape constraints and rest
//    lengths are taken from them. With Guidance 0 that is TressFX's rigid per-strand skinning.
//  - TressFX's constraints act per point whatever a strand's length, so a load moves points about
//    the same distance on any strand, and a converted hairstyle mixes 1-unit scalp strands with
//    40-unit locks. On a strand shorter than ShortStrandLength the global range reaches as far as
//    on one that long (GlobalRange x ShortStrandLength units from the root), and VSP is scaled by
//    its length over ShortStrandLength. VSP moves the strand by the root's motion over the step
//    after the global pull has put the held part on its targets, so it carries that part past
//    them; that bent a short strand's short segments into kinks. Longer strands are TressFX's.
//  - Wind acts on each segment at its rest length, not its current one, and no segment ends a
//    step longer than MaxStretch of its rest length. Without these, a strand stretched by a hard
//    stop caught more wind the longer it got, and settings far from the defaults blew it apart.
//  - Collision keeps a styled shape that already lies inside a capsule or the head from being
//    pushed out (see Collide and HairStrandsSkin::CollideHead), and the position delta clamp
//    leaves the two pinned points alone.
//  - TressFX builds its distance field at each pass's pose. The body's is built once a frame, at
//    the frame's pose, and stores how the surface moved over the frame: a step part-way through
//    the frame takes the point ahead (with the actor's root, then the surface) to the frame's end,
//    collides it there, and takes it back. It runs with the capsules, before the clamp, rather
//    than after the whole pass.
//  - The body keeps a point as far off it as the point's target lies (between BodyMinClearance and
//    BodyMaxClearance), so a styled shape resting on the body rests as styled, and one styled into
//    armour lies on the armour. A point pushed out moves on with the surface: it keeps the
//    surface's move over the step and BodySlide of its own slide along it, none of its motion into
//    or off it, less the move VSP gives it with the root. Stopping it dead, as TressFX does, left
//    hair on a moving body to be caught up and shoved every step, and it shook. With VSP's move on
//    top, hair on the back of a running body was driven into it a little every step.
//  - A point that went past the middle of a thin part (a shield, a plate) in one step reads inside
//    it, from the far side's surface. If where it began the step reads the other way, it goes back
//    there, on the surface's side it came from, rather than out through the part.
//  - Not in TressFX at all: the cards the hair keeps (braids, ties, buns, CardField.hlsli) collide
//    like the body, after it: a point is kept as far off them as its target lies (between
//    CardMinClearance and CardMaxClearance), moves on with them (a swinging braid carries it
//    aside) and keeps BodySlide of its slide along them. A card is a sheet with no inside, so a
//    point that went through one in a step goes back to where it began, on the side it came from.

#include "HairStrands/CardField.hlsli"
#include "HairStrands/Skinning.hlsli"

RWStructuredBuffer<HairStrands::GuidePoint> Guides : register(u0);

// GeneratorLimits::kMaxPointsPerStrand.
#define MAX_POINTS 32

namespace HairStrandsSim
{
	// TressFX ResolveCapsuleCollisions: the share of a colliding point's move along the capsule kept.
	static const float CapsuleFriction = 0.4;
	// The share of a point's slide along the body (relative to it) kept: TressFX's capsule friction.
	static const float BodySlide = CapsuleFriction;
	// Longest a segment may end a step, relative to its rest length.
	static const float MaxStretch = 1.2;
	// Units. A shorter strand has the global range of one this long, and VSP by its length over it.
	static const float ShortStrandLength = 10.0;
	// Longer locks take the full gravity load; shorter scalp/fringe strands resist sag.
	static const float FullGravityLength = 20.0;

	float4 NormalizeQuaternion(float4 q)
	{
		const float n = dot(q, q);
		if (n < 1e-10) {
			q.w = 1.0;
			return q;
		}
		return q * rsqrt(n);
	}

	// TressFX QuatFromTwoUnitVectors: the rotation taking unit vector u to unit vector v.
	float4 QuatFromTwoUnitVectors(float3 u, float3 v)
	{
		float r = 1.0 + dot(u, v);
		float3 n;
		if (r < 1e-7) {
			// u and v are opposite
			r = 0.0;
			n = abs(u.x) > abs(u.z) ? float3(-u.y, u.x, 0.0) : float3(0.0, -u.z, u.y);
		} else {
			n = cross(u, v);
		}
		return NormalizeQuaternion(float4(n, r));
	}

	float3 MultQuaternionAndVector(float4 q, float3 v)
	{
		const float3 uv = cross(q.xyz, v);
		const float3 uuv = cross(q.xyz, uv);
		return v + uv * (2.0 * q.w) + uuv * 2.0;
	}

	// TressFX ApplyDistanceConstraint. The first two points of a strand are immovable
	// (inverse mass 0 in TressFX's assets), so a pair with one of them moves the other alone.
	void ApplyDistanceConstraint(inout float3 a_pos0, inout float3 a_pos1, uint a_index0, float a_targetDistance)
	{
		float3 delta = a_pos1 - a_pos0;
		const float distance = max(length(delta), 1e-7);
		delta *= 1.0 - a_targetDistance / distance;
		const bool movable0 = a_index0 >= 2;
		const bool movable1 = a_index0 + 1 >= 2;
		const float2 multiplier = movable0 ? (movable1 ? float2(0.5, 0.5) : float2(1.0, 0.0)) : (movable1 ? float2(0.0, 1.0) : float2(0.0, 0.0));
		a_pos0 += multiplier.x * delta;
		a_pos1 -= multiplier.y * delta;
	}

	// TressFX CapsuleCollision with one radius at both ends (a_p0 == a_p1 is a sphere). A point
	// inside is put on the surface; on the cylinder, only CapsuleFriction of its move since the
	// last step along the axis is kept.
	bool CapsuleCollision(float3 a_position, float3 a_oldPosition, float3 a_p0, float3 a_p1, float a_radius, out float3 o_position)
	{
		o_position = a_position;
		const float3 segment = a_p1 - a_p0;
		const float3 delta0 = a_position - a_p0;
		const float3 delta1 = a_p1 - a_position;
		const float dist0 = dot(delta0, segment);
		const float dist1 = dot(delta1, segment);
		const float radiusSquared = a_radius * a_radius;

		// colliding with sphere 0 (or the whole sphere)
		if (dist0 < 0.0 || dot(segment, segment) < 1e-8) {
			if (dot(delta0, delta0) < radiusSquared) {
				o_position = a_p0 + a_radius * HairStrandsSkin::SafeNormalize(delta0, float3(0, 0, 1));
				return true;
			}
			return false;
		}

		// colliding with sphere 1
		if (dist1 < 0.0) {
			if (dot(delta1, delta1) < radiusSquared) {
				o_position = a_p1 + a_radius * HairStrandsSkin::SafeNormalize(-delta1, float3(0, 0, 1));
				return true;
			}
			return false;
		}

		// colliding with the middle cylinder
		const float3 x = (dist0 * a_p1 + dist1 * a_p0) / (dist0 + dist1);
		const float3 delta = a_position - x;
		if (dot(delta, delta) < radiusSquared) {
			const float3 n = HairStrandsSkin::SafeNormalize(delta, float3(0, 0, 1));
			const float3 vec = a_position - a_oldPosition;
			const float3 segN = normalize(segment);
			const float3 vecTangent = dot(vec, segN) * segN;
			const float3 vecNormal = vec - vecTangent;
			o_position = a_oldPosition + CapsuleFriction * vecTangent + (vecNormal + a_radius * n - delta);
			return true;
		}
		return false;
	}

	float3 ClosestOnSegment(float3 a_p, float3 a_a, float3 a_b)
	{
		const float3 ab = a_b - a_a;
		const float lengthSquared = dot(ab, ab);
		const float t = lengthSquared > 1e-8 ? saturate(dot(a_p - a_a, ab) / lengthSquared) : 0.0;
		return a_a + ab * t;
	}

	// TressFX ResolveCapsuleCollisions over the colliders. Each collider shrinks, for this point,
	// to the depth its target already lies at (not below HairStrandsSkin::MinColliderDepth of its
	// radius), so a styled shape resting on or in a collider stays as it is.
	bool Collide(inout float3 io_position, float3 a_oldPosition, float3 a_target)
	{
		bool collided = false;
		[loop] for (uint c = 0; c < ColliderCount; ++c)
		{
			const float3 a = Colliders[c * 2].xyz;
			const float radius = Colliders[c * 2].w;
			const float3 b = Colliders[c * 2 + 1].xyz;
			const float targetDistance = length(a_target - ClosestOnSegment(a_target, a, b));
			const float allowed = max(min(radius, targetDistance), radius * HairStrandsSkin::MinColliderDepth);
			float3 pushed;
			if (CapsuleCollision(io_position, a_oldPosition, a, b, allowed, pushed)) {
				io_position = pushed;
				collided = true;
			}
		}
		return collided;
	}

	// The head bone's frame at a_f of the way from last frame's pose to this frame's, as the
	// targets are taken at each step: the head field rides it.
	HairStrandsSkin::HeadFrame StepHeadFrame(float a_f)
	{
		float3x4 previous = HairStrandsSkin::LoadBone(HeadBone, BoneCount * 3);
		previous[0].w += PreviousToCurrent.x;
		previous[1].w += PreviousToCurrent.y;
		previous[2].w += PreviousToCurrent.z;
		const float3x4 head = lerp(previous, HairStrandsSkin::LoadBone(HeadBone, 0), a_f);
		HairStrandsSkin::HeadFrame frame;
		frame.fromSkin = (float3x3)head;
		frame.toSkin = HairStrandsSkin::Inverse(frame.fromSkin);
		frame.origin = float3(head[0].w, head[1].w, head[2].w);
		return frame;
	}

	// TressFX mixes its four wind vectors per strand, so neighbouring strands blow apart.
	float3 GuideWind(uint a_guide)
	{
		const float a = (float)(a_guide % 20) / 20.0;
		return a * Wind[0].xyz + (1.0 - a) * Wind[1].xyz + a * Wind[2].xyz + (1.0 - a) * Wind[3].xyz;
	}
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const uint guide = dispatchID.x;
	if (guide >= GuideCount)
		return;
	const uint n = min(PointsPerStrand, MAX_POINTS);
	const uint base = guide * PointsPerStrand;

	float3 targetStart[MAX_POINTS];         // skinned rest positions last frame, relative to this frame's camera
	float3 targetEnd[MAX_POINTS];           // and this frame
	float3 position[MAX_POINTS];            // TressFX's g_HairVertexPositions
	float3 previous[MAX_POINTS];            // g_HairVertexPositionsPrev
	float3 stepOffset[MAX_POINTS];          // position - target, at the last step
	float3 previousStepOffset[MAX_POINTS];  // and at the step before
	float2 bodyLimits[MAX_POINTS];          // how far off the body each point is kept, and the deepest it is believed to be
	uint i, step, iteration;

	[loop] for (i = 0; i < n; ++i)
	{
		const HairStrands::RestPoint rest = RestPoints[base + i];
		float3x4 current, previousFrame, targetCurrent, targetPrevious;
		HairStrandsSkin::Skin(rest, current, previousFrame);
		HairStrandsSkin::TargetSkin(rest, current, previousFrame, targetCurrent, targetPrevious);
		const float4 restPosition = float4(rest.Position, 1.0);
		targetEnd[i] = mul(targetCurrent, restPosition);
		targetStart[i] = mul(targetPrevious, restPosition) + PreviousToCurrent;
	}

	// A root that jumped (teleport, load, animation snap) restarts the whole strand.
	const bool reset = (Flags & HAIR_STRANDS_FLAG_RESET) != 0 || !(distance(targetEnd[0], Guides[base].Position + EyeShift) <= TeleportDistance);

	float3 previousPrevious1 = targetEnd[1];  // g_HairVertexPositionsPrevPrev of the second point
	[loop] for (i = 0; i < n; ++i)
	{
		const HairStrands::GuidePoint old = Guides[base + i];
		position[i] = reset ? targetEnd[i] : old.Position + EyeShift;
		previous[i] = reset ? targetEnd[i] : old.PreviousPosition + EyeShift;
		stepOffset[i] = reset ? (float3)0 : old.StepOffset;
		previousStepOffset[i] = reset ? (float3)0 : old.PreviousStepOffset;
		if (i == 1 && !reset)
			previousPrevious1 = old.PreviousPreviousPosition + EyeShift;
	}

	const uint steps = reset ? 0 : Steps;
	const float h = StepTime;
	const float decay = exp(-Damping * h * 60.0);
	// Stronger gravity restores long locks promptly after a stop. Scalp strands keep the
	// lighter load their shape constraints supported; ramp to the full load over 4-20 units.
	const float gravityScale = lerp(0.25, 1.0, saturate((Strands[guide].Length - 4.0) / (HairStrandsSim::FullGravityLength - 4.0)));
	const float3 gravity = float3(0.0, 0.0, -Gravity) * (gravityScale * h * h);
	// TressFX: 1.0 for stiffness makes things unstable sometimes.
	const float localStiffness = 0.5 * min(LocalStiffness, 0.95);
	const float lengthScale = saturate(Strands[guide].Length / HairStrandsSim::ShortStrandLength);
	const float globalCount = GlobalRange * (float)n / max(lengthScale, 1e-4);
	const float3 wind = HairStrandsSim::GuideWind(guide);
	const bool haveWind = any(wind != 0.0);
	const bool collide = (Flags & HAIR_STRANDS_FLAG_COLLIDE) != 0 && ColliderCount > 0;
	const bool collideHead = (Flags & HAIR_STRANDS_FLAG_HEAD_FIELD) != 0;
	const bool collideBody = (Flags & HAIR_STRANDS_FLAG_BODY_FIELD) != 0;
	const bool collideCards = (Flags & HAIR_STRANDS_FLAG_CARD_FIELD) != 0;
	[loop] for (i = 0; i < n; ++i)
	{
		bodyLimits[i] = 0;
		if (collideBody)
			bodyLimits[i] = HairStrandsSkin::BodyLimits(targetEnd[i]);
	}

	[loop] for (step = 0; step < steps; ++step)
	{
		const float f = saturate(FirstStep + (float)step * StepFraction);

		// IntegrationAndGlobalShapeConstraints.
		[loop] for (i = 0; i < n; ++i)
		{
			const float3 target = lerp(targetStart[i], targetEnd[i], f);
			float3 next = target;
			if (i >= 2) {
				next = position[i] + decay * (position[i] - previous[i]) + gravity;
				if (GlobalStiffness > 0.0 && (float)i < globalCount)
					next += GlobalStiffness * (target - next);
			}
			if (i == 1)
				previousPrevious1 = previous[1];
			previous[i] = position[i];
			position[i] = next;
		}

		// CalculateStrandLevelData, then VelocityShockPropagation. The root's motion over the step
		// (rotation, translation) and the share of it given to the strand stay for the body contact.
		float4 rotation;
		float3 translation;
		float vsp;
		{
			const float3 u = HairStrandsSkin::SafeNormalize(previous[1] - previous[0], float3(0, 0, -1));
			const float3 v = HairStrandsSkin::SafeNormalize(position[1] - position[0], u);
			rotation = HairStrandsSim::QuatFromTwoUnitVectors(u, v);
			translation = position[0] - HairStrandsSim::MultQuaternionAndVector(rotation, previous[0]);
			const float accel = length(position[1] - 2.0 * previous[1] + previousPrevious1);
			vsp = accel > VspAccelThreshold ? 1.0 : VspCoeff * lengthScale;
			[loop] for (i = 2; i < n; ++i)
			{
				position[i] = lerp(position[i], HairStrandsSim::MultQuaternionAndVector(rotation, position[i]) + translation, vsp);
				previous[i] = lerp(previous[i], HairStrandsSim::MultQuaternionAndVector(rotation, previous[i]) + translation, vsp);
			}
		}

		// LocalShapeConstraints.
		[loop] for (iteration = 0; iteration < LocalIterations; ++iteration)
		{
			[loop] for (i = 1; i + 1 < n; ++i)
			{
				const float3 bindPos = lerp(targetStart[i], targetEnd[i], f);
				const float3 bindPosMinusOne = lerp(targetStart[i - 1], targetEnd[i - 1], f);
				const float3 bindPosPlusOne = lerp(targetStart[i + 1], targetEnd[i + 1], f);
				const float3 lastVecBindPose = HairStrandsSkin::SafeNormalize(bindPos - bindPosMinusOne, float3(0, 0, -1));
				const float3 lastVec = HairStrandsSkin::SafeNormalize(position[i] - position[i - 1], lastVecBindPose);
				const float4 rotGlobal = HairStrandsSim::QuatFromTwoUnitVectors(lastVecBindPose, lastVec);
				const float3 orgPos = HairStrandsSim::MultQuaternionAndVector(rotGlobal, bindPosPlusOne - bindPos) + position[i];
				const float3 del = localStiffness * (orgPos - position[i + 1]);
				if (i >= 2)
					position[i] -= del;
				position[i + 1] += del;
			}
		}

		// LengthConstriantsWindAndCollision: wind across each segment, from the third point to
		// the one before the tip. Each point's segment reaches the next point, not yet moved.
		// TressFX's force grows with the segment's length squared; at its rest length here, so a
		// stretched strand does not catch more wind and stretch further.
		if (haveWind) {
			[loop] for (i = 2; i + 1 < n; ++i)
			{
				const float restLength = distance(lerp(targetStart[i], targetEnd[i], f), lerp(targetStart[i + 1], targetEnd[i + 1], f));
				const float3 segment = HairStrandsSkin::SafeNormalize(position[i] - position[i + 1], 0) * restLength;
				position[i] += -cross(cross(segment, wind), segment) * (h * h);
			}
		}

		// Length constraints: even pairs, then odd pairs, as TressFX's threads do them.
		[loop] for (iteration = 0; iteration < LengthIterations; ++iteration)
		{
			[loop] for (i = 0; i + 1 < n; i += 2)
			{
				const float restLength = distance(lerp(targetStart[i], targetEnd[i], f), lerp(targetStart[i + 1], targetEnd[i + 1], f));
				HairStrandsSim::ApplyDistanceConstraint(position[i], position[i + 1], i, restLength);
			}
			[loop] for (i = 1; i + 1 < n; i += 2)
			{
				const float restLength = distance(lerp(targetStart[i], targetEnd[i], f), lerp(targetStart[i + 1], targetEnd[i + 1], f));
				HairStrandsSim::ApplyDistanceConstraint(position[i], position[i + 1], i, restLength);
			}
		}

		// Not TressFX: a few Jacobi passes cannot stop a long strand that is still moving when
		// the head stops (a landing, the end of a sprint), and it stretched to twice its length.
		// No segment is left longer than MaxStretch of its rest length, measured from the root.
		[loop] for (i = 2; i < n; ++i)
		{
			const float maxLength = HairStrandsSim::MaxStretch * distance(lerp(targetStart[i - 1], targetEnd[i - 1], f), lerp(targetStart[i], targetEnd[i], f));
			const float3 segment = position[i] - position[i - 1];
			const float segmentLength = length(segment);
			if (segmentLength > maxLength)
				position[i] = position[i - 1] + segment * (maxLength / segmentLength);
		}

		// Collision, then TressFX's clamp of the move since the last step (its formula, and only on
		// the movable points: rewriting the pinned points' history would skew the next step's VSP).
		[loop] for (i = 2; i < n; ++i)
		{
			const float3 target = lerp(targetStart[i], targetEnd[i], f);
			bool collided = false;
			if (collide)
				collided = HairStrandsSim::Collide(position[i], previous[i], target);
			bool contact = false;
			float3 contactNormal = 0;
			float3 surfaceMove = 0;
			float3 shift = 0;  // VSP's move of the point (and its previous position) with the root this step
			if (collideBody || collideCards)
				shift = (HairStrandsSim::MultQuaternionAndVector(rotation, position[i]) + translation - position[i]) * vsp;
			if (collideBody) {
				// The field is the body at the frame's end: the point goes ahead with the surface to
				// then, and the surface moved over the step as over the frame.
				const float3 ahead = HairStrandsSkin::BodyAhead(position[i], f);
				float surfaceDistance;
				float3 frameMove;
				if (HairStrandsSkin::CollideBody(position[i], ahead, bodyLimits[i], surfaceDistance, contactNormal, frameMove)) {
					surfaceMove = frameMove * StepFraction;
					contact = true;
					// Into a thin part from the other side in one step (inside it, the field is the far
					// side's): back where it began on the surface, on the side it came from.
					if (surfaceDistance < 0.0) {
						const float3 start = previous[i] - shift + surfaceMove;
						float startDistance;
						float3 startNormal, startMove;
						const bool started = HairStrandsSkin::SampleBody(start + ahead, startDistance, startNormal, startMove);
						if (started && dot(startNormal, contactNormal) < 0.0) {
							position[i] = start + startNormal * max(bodyLimits[i].x - startDistance, 0.0);
							contactNormal = startNormal;
						}
					}
				}
			}
			if (collideCards) {
				// The field is the cards at the frame's end, as the body's: the point goes ahead with
				// them to then. Its clearance is read only near them (the target is read again).
				const float3 ahead = HairStrandsCards::CardsAhead(position[i], f);
				float cardDistance;
				float3 cardNormal, cardMove;
				if (HairStrandsCards::SampleCards(position[i] + ahead, cardDistance, cardNormal, cardMove)) {
					const float clearance = HairStrandsCards::CardClearance(targetEnd[i]);
					const float3 start = previous[i] - shift + cardMove * StepFraction;
					float startDistance;
					float3 startNormal, startMove;
					const bool started = HairStrandsCards::SampleCards(start + ahead, startDistance, startNormal, startMove);
					if (started && HairStrandsCards::CrossedCard(start + ahead, startDistance, startNormal, position[i] + ahead, cardNormal)) {
						// Through a card in one step: back where it began, on the side it came from.
						position[i] = start + startNormal * max(clearance - startDistance, 0.0);
						cardNormal = startNormal;
						cardDistance = 0.0;
					} else if (cardDistance < clearance) {
						position[i] += cardNormal * (clearance - cardDistance);
					}
					if (cardDistance < clearance) {
						contact = true;
						contactNormal = cardNormal;
						surfaceMove = cardMove * StepFraction;
					}
				}
			}
			float3 positionDelta = position[i] - previous[i];
			const float speedSquared = dot(positionDelta, positionDelta);
			if (speedSquared > ClampPositionDelta * ClampPositionDelta) {
				positionDelta *= ClampPositionDelta * ClampPositionDelta / speedSquared;
				previous[i] = position[i] - positionDelta;
			}
			if (collided) {
				previous[i] = position[i];
			} else if (contact) {
				// On the body (or a card): the point moves on with the surface, keeping BodySlide of its
				// slide along it. VSP will move it with the root again next step: its velocity is the
				// surface's move less that.
				float3 slide = position[i] - previous[i] + shift - surfaceMove;
				slide -= contactNormal * dot(slide, contactNormal);
				previous[i] = position[i] - (surfaceMove - shift) - HairStrandsSim::BodySlide * slide;
			}
		}

		// The head field, as TressFX's signed distance field collision.
		if (collideHead) {
			const HairStrandsSkin::HeadFrame head = HairStrandsSim::StepHeadFrame(f);
			[loop] for (i = 2; i < n; ++i)
			{
				const float3 pushed = HairStrandsSkin::CollideHead(position[i], HairStrandsSkin::HeadDepth(lerp(targetStart[i], targetEnd[i], f), head), head);
				if (any(pushed != position[i])) {
					position[i] = pushed;
					previous[i] = pushed;
				}
			}
		}

		[loop] for (i = 0; i < n; ++i)
		{
			previousStepOffset[i] = stepOffset[i];
			stepOffset[i] = position[i] - lerp(targetStart[i], targetEnd[i], f);
		}
	}

	[loop] for (i = 0; i < n; ++i)
	{
		if (any(!isfinite(position[i])) || any(!isfinite(previous[i])) || any(!isfinite(stepOffset[i])) || any(!isfinite(previousStepOffset[i]))) {
			position[i] = targetEnd[i];
			previous[i] = targetEnd[i];
			stepOffset[i] = 0;
			previousStepOffset[i] = 0;
		}
	}
	if (any(!isfinite(previousPrevious1)))
		previousPrevious1 = targetEnd[1];

	// Drawn: the offsets interpolated from the step before the last to the last, at the time this
	// frame is past the last step, and added to this frame's targets.
	[loop] for (i = 0; i < n; ++i)
	{
		const uint a = i > 0 ? i - 1 : 0;
		const uint b = min(i + 1, n - 1);
		const float3 offsetA = lerp(previousStepOffset[a], stepOffset[a], DisplayAlpha);
		const float3 offsetB = lerp(previousStepOffset[b], stepOffset[b], DisplayAlpha);
		const float3 restTangent = HairStrandsSkin::SafeNormalize(targetEnd[b] - targetEnd[a], float3(0, 0, -1));
		const float3 tangent = HairStrandsSkin::SafeNormalize(targetEnd[b] + offsetB - targetEnd[a] - offsetA, restTangent);

		// One whole-element write per point: fxc cannot map partial UAV writes under a branch (X4532).
		HairStrands::GuidePoint result;
		result.Rotation = HairStrandsSkin::ShortestArc(restTangent, tangent);
		result.Position = position[i];
		result.PreviousPosition = previous[i];
		result.PreviousPreviousPosition = i == 1 ? previousPrevious1 : previous[i];
		result.StepOffset = stepOffset[i];
		result.PreviousStepOffset = previousStepOffset[i];
		result.Offset = lerp(previousStepOffset[i], stepOffset[i], DisplayAlpha);
		result.PreviousOffset = reset ? result.Offset : Guides[base + i].Offset;
		result.Pad0 = 0;
		result.Pad1 = 0;
		result.Pad2 = 0;
		result.Pad3 = 0;
		result.Pad4 = 0;
		result.Pad5 = 0;
		result.Pad6 = 0;
		Guides[base + i] = result;
	}
}
