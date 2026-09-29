// Hair Strands: skins strand control points with the hair's bone palette, and makes them
// follow their simulated guide strands. One thread per control point; only the strands the
// LOD keeps are dispatched.
//
// A strand point follows its guide at the same distance from the root: its offset from the
// guide's target is turned by the guide's rotation there and added to the guide's simulated
// position. Previous positions go the same way with the guide's previous state, so motion
// vectors carry the simulated motion.

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

		const float3 followed = lerp(a.Position + HairStrandsSkin::Rotate(a.Rotation, target - a.Target),
			b.Position + HairStrandsSkin::Rotate(b.Rotation, target - b.Target), w);
		const float3 followedPrevious = lerp(a.PreviousPosition + HairStrandsSkin::Rotate(a.PreviousRotation, previousTarget - a.PreviousTarget),
			b.PreviousPosition + HairStrandsSkin::Rotate(b.PreviousRotation, previousTarget - b.PreviousTarget), w);
		const float4 rotation = normalize(lerp(a.Rotation, dot(a.Rotation, b.Rotation) < 0.0 ? -b.Rotation : b.Rotation, w));

		result.Position = lerp(result.Position, followed, SimWeight);
		result.PreviousPosition = lerp(result.PreviousPosition, followedPrevious, SimWeight);
		result.Normal = HairStrandsSkin::SafeNormalize(lerp(result.Normal, HairStrandsSkin::Rotate(rotation, normal), SimWeight), result.Normal);
	}
	Skinned[id] = result;
}
