// Hair Strands: the Lighting shader for the parts of a hair kept as cards (braids, twists, ties,
// buns and the hair gathered into them), drawn in place of the game's card draw, which the strands
// hide whole.
//
// Compiled with the defines of the hair's own Lighting permutation, as StrandLighting.hlsl is, so
// its pixel shader is Lighting.hlsl's and shades the cards as the game does: the card texture, its
// alpha test, the normal map. The vertex shader skins the cards with the strands' bone palette,
// whose last bones are the joints of the chains that hanging braids swing on.

#if defined(VSHADER)
#	undef VSHADER
#	define HAIR_CARDS_VSHADER
#endif

#include "Lighting.hlsl"

#if defined(HAIR_CARDS_VSHADER)

#	include "HairStrands/Common.hlsli"

// The game's Lighting VS constants, still bound for the hair's pass.
cbuffer CardPerTechnique : register(b0)
{
	float4 HighDetailRange : packoffset(c0);
	float4 FogParam : packoffset(c1);
	float4 FogNearColor : packoffset(c2);
	float4 FogFarColor : packoffset(c3);
};

cbuffer CardPerFrame : register(b12)
{
	row_major float3x3 ScreenProj : packoffset(c0);
	row_major float4x4 Proj : packoffset(c4);
	row_major float4x4 ViewProj : packoffset(c8);
};

// Mirrors HairStrands::CardDrawCB (StrandRenderer.h).
cbuffer CardDraw : register(b7)
{
	float3 EyeDelta;  // skinning camera to this pass's camera
	uint BoneCount;   // palette bones (skin instance and chain joints)
	float3 PreviousEyeDelta;
	float CardPad;
	// The material's UV offset (xy) and scale (zw): the depth prepass's Utility shader binds
	// another b1, without the Lighting shader's TexcoordOffset.
	float4 CardTexcoordOffset;
};

StructuredBuffer<HairStrands::CardVertex> CardVertices : register(t0);
// BoneCount 3x4 skin-to-world rows relative to the skinning camera, then BoneCount for the
// previous frame (HairStrands/Skinning.hlsli's palette).
StructuredBuffer<float4> CardPalette : register(t1);

namespace HairCardsVS
{
	float3x4 LoadBone(uint a_bone, uint a_base)
	{
		const uint row = a_base + a_bone * 3;
		return float3x4(CardPalette[row], CardPalette[row + 1], CardPalette[row + 2]);
	}

	float3 SafeNormalize(float3 a_v, float3 a_fallback)
	{
		const float lengthSquared = dot(a_v, a_v);
		return lengthSquared > 1e-12 ? a_v * rsqrt(lengthSquared) : a_fallback;
	}
}

VS_OUTPUT main(uint vertexID : SV_VertexID)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;
	const HairStrands::CardVertex vertex = CardVertices[vertexID];

	uint4 bones = uint4(vertex.Bones01 & 0xFFFF, vertex.Bones01 >> 16, vertex.Bones23 & 0xFFFF, vertex.Bones23 >> 16);
	bones = min(bones, (BoneCount - 1).xxxx);
	const float4 weights = float4(vertex.Weights & 0xFF, (vertex.Weights >> 8) & 0xFF, (vertex.Weights >> 16) & 0xFF, vertex.Weights >> 24) / 255.0;
	float3x4 current = 0;
	float3x4 previous = 0;
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		current += HairCardsVS::LoadBone(bones[i], 0) * weights[i];
		previous += HairCardsVS::LoadBone(bones[i], BoneCount * 3) * weights[i];
	}

	const float4 restPosition = float4(vertex.Position, 1.0);
	const float3 position = mul(current, restPosition) + EyeDelta;
	const float3 previousPosition = mul(previous, restPosition) + PreviousEyeDelta;
	const float4 worldPosition = float4(position, 1.0);
	const float4 viewPos = mul(ViewProj, worldPosition);
	vsout.Position = viewPos;
	vsout.TexCoord0.xy = float2(vertex.U, vertex.V) * CardTexcoordOffset.zw + CardTexcoordOffset.xy;

#	if defined(SKINNED) || !defined(MODELSPACENORMALS)
	// As Lighting.hlsl's skinned vertex shader: the TBN rows turned by the bones, as columns.
	const float3x3 turn = (float3x3)current;
	const float3 normal = HairCardsVS::SafeNormalize(mul(turn, vertex.Normal), float3(0, 0, 1));
	const float3 tangent = HairCardsVS::SafeNormalize(mul(turn, vertex.Tangent), float3(1, 0, 0));
	const float3 bitangent = HairCardsVS::SafeNormalize(mul(turn, vertex.Bitangent), float3(0, 1, 0));
	vsout.TBN0.xyz = float3(tangent.x, bitangent.x, normal.x);
	vsout.TBN1.xyz = float3(tangent.y, bitangent.y, normal.y);
	vsout.TBN2.xyz = float3(tangent.z, bitangent.z, normal.z);
#	endif

	vsout.WorldPosition = worldPosition;
	vsout.PreviousWorldPosition = float4(previousPosition, 1.0);
	vsout.Color = 1.0.xxxx;

	const bool reverseProjection = FrameBuffer::IsReverseProjection(Proj);
	float fogColorParam = min(FogParam.w,
		exp2(FogParam.z * log2(saturate(length(FrameBuffer::ToStandardClip(viewPos, reverseProjection)) * FogParam.y - FogParam.x))));
	vsout.FogParam.xyz = lerp(FogNearColor.xyz, FogFarColor.xyz, fogColorParam);
	vsout.FogParam.w = fogColorParam;

	vsout.ModelPosition = vertex.Position;

	return vsout;
}

#endif  // HAIR_CARDS_VSHADER
