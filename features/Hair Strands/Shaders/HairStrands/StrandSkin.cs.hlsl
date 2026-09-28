// Hair Strands: skins strand control points with the hair's bone palette.
// One thread per control point; only the strands the LOD keeps are dispatched.

#include "HairStrands/Common.hlsli"

cbuffer SkinCB : register(b0)
{
	uint PointCount;
	uint BoneCount;
	uint2 Pad;
};

StructuredBuffer<HairStrands::RestPoint> RestPoints : register(t0);
// BoneCount 3x4 skin-to-world rows (translation relative to this frame's camera),
// then BoneCount rows for the previous frame (relative to the previous frame's camera).
StructuredBuffer<float4> Palette : register(t1);

RWStructuredBuffer<HairStrands::SkinnedPoint> Skinned : register(u0);

float3x4 LoadBone(uint a_bone, uint a_base)
{
	uint row = a_base + a_bone * 3;
	return float3x4(Palette[row], Palette[row + 1], Palette[row + 2]);
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const uint id = dispatchID.x;
	if (id >= PointCount)
		return;

	HairStrands::RestPoint rest = RestPoints[id];
	uint4 bones = uint4(rest.Bones01 & 0xFFFF, rest.Bones01 >> 16, rest.Bones23 & 0xFFFF, rest.Bones23 >> 16);
	bones = min(bones, (BoneCount - 1).xxxx);
	float4 weights = float4(rest.Weights & 0xFF, (rest.Weights >> 8) & 0xFF, (rest.Weights >> 16) & 0xFF, rest.Weights >> 24) / 255.0;

	float3x4 current = 0;
	float3x4 previous = 0;
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		current += LoadBone(bones[i], 0) * weights[i];
		previous += LoadBone(bones[i], BoneCount * 3) * weights[i];
	}

	const float4 position = float4(rest.Position, 1.0);
	HairStrands::SkinnedPoint result;
	result.Position = mul(current, position);
	result.PreviousPosition = mul(previous, position);
	result.Normal = normalize(mul((float3x3)current, rest.Normal));
	result.Pad0 = 0;
	result.Pad1 = 0;
	result.Pad2 = 0;
	Skinned[id] = result;
}
