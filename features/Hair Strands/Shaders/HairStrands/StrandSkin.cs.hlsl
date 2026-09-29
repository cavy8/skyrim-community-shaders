// Hair Strands: skins strand control points with the hair's bone palette, and makes them
// follow their simulated guide strands. One thread per control point; only the strands the
// LOD keeps are dispatched.
//
// A strand point follows its guide at the same distance from the root: it moves off its own
// target by as much as the guide has moved off the guide's target there. Previous positions go
// the same way with the guide's previous state, so motion vectors carry the simulated motion.
// The guide's rotation turns only the normal. Turning the offset from the guide with it would
// make the offset a lever: a strand beside a short guide, or longer than it, would swing and
// stretch by the offset times every bend of the guide.
//
// Only guides collide in the simulation. A strand nearer the scalp than its guide can be
// carried into the head by the guide's displacement, so each strand point is also kept out of
// the head field (never deeper than its own target lies).

#include "HairStrands/Skinning.hlsli"

StructuredBuffer<HairStrands::GuidePoint> Guides : register(t3);

RWStructuredBuffer<HairStrands::SkinnedPoint> Skinned : register(u0);

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const uint id = dispatchID.x;
	if (id >= PointCount)
		return;

	const HairStrands::RestPoint rest = RestPoints[id];
	float3x4 current, previous;
	HairStrandsSkin::Skin(rest, current, previous);

	const float4 position = float4(rest.Position, 1.0);
	HairStrands::SkinnedPoint result;
	result.Position = mul(current, position);
	result.PreviousPosition = mul(previous, position);
	result.Normal = normalize(mul((float3x3)current, rest.Normal));
	result.Pad0 = 0;
	result.Pad1 = 0;
	result.Pad2 = 0;

	if ((Flags & HAIR_STRANDS_FLAG_FOLLOW) && SimWeight > 0.0 && PointsPerStrand > 1) {
		float3x4 targetCurrent, targetPrevious;
		HairStrandsSkin::TargetSkin(current, previous, targetCurrent, targetPrevious);
		const float3 target = mul(targetCurrent, position);
		const float3 previousTarget = mul(targetPrevious, position);
		const float3 normal = normalize(mul((float3x3)targetCurrent, rest.Normal));

		const HairStrands::StrandInfo info = Strands[id / PointsPerStrand];
		const uint guide = min(info.Guide, GuideCount - 1);
		const float guideLength = max(Strands[guide].Length, 1e-4);
		const float along = saturate(rest.T * info.Length / guideLength) * (PointsPerStrand - 1);
		const uint j = min((uint)along, PointsPerStrand - 2);
		const float w = along - j;
		const HairStrands::GuidePoint a = Guides[guide * PointsPerStrand + j];
		const HairStrands::GuidePoint b = Guides[guide * PointsPerStrand + j + 1];

		float3 followed = target + lerp(a.Position - a.Target, b.Position - b.Target, w);
		float3 followedPrevious = previousTarget + lerp(a.PreviousPosition - a.PreviousTarget, b.PreviousPosition - b.PreviousTarget, w);
		// The guide's displacement can carry a strand lying closer to the scalp into the head.
		if (Flags & HAIR_STRANDS_FLAG_HEAD_FIELD) {
			const HairStrandsSkin::HeadFrame head = HairStrandsSkin::LoadHeadFrame(0);
			const HairStrandsSkin::HeadFrame previousHead = HairStrandsSkin::LoadHeadFrame(BoneCount * 3);
			followed = HairStrandsSkin::CollideHead(followed, HairStrandsSkin::HeadDepth(target, head), head);
			followedPrevious = HairStrandsSkin::CollideHead(followedPrevious, HairStrandsSkin::HeadDepth(previousTarget, previousHead), previousHead);
		}
		const float4 rotation = normalize(lerp(a.Rotation, dot(a.Rotation, b.Rotation) < 0.0 ? -b.Rotation : b.Rotation, w));

		result.Position = lerp(result.Position, followed, SimWeight);
		result.PreviousPosition = lerp(result.PreviousPosition, followedPrevious, SimWeight);
		result.Normal = HairStrandsSkin::SafeNormalize(lerp(result.Normal, HairStrandsSkin::Rotate(rotation, normal), SimWeight), result.Normal);
	}
	Skinned[id] = result;
}
