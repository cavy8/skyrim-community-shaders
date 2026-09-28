// Hair Strands: the Lighting shader for strand ribbons.
//
// Compiled with the defines of the hair's own Lighting permutation (HairStrands::StrandRenderer),
// so the pixel shader is Lighting.hlsl's, reading the textures, samplers and constants the game
// bound for the hair cards: every lighting feature (Hair Specular, Hair Backlighting, shadows,
// deferred output, motion vectors) applies to strands unchanged. Lighting.hlsl reads HAIR_STRANDS
// to take the strand colour from the card texture without its alpha and to ignore the card
// normal map. The vertex shader is this file's: it expands skinned strand control points into
// camera-facing ribbons and adds the procedural style (waves, curls, coils, frizz).

#define HAIR_STRANDS
// Strands sample the card colour slightly blurred so the gaps painted between card strands
// do not show as dark dashes along a strand.
#define HAIR_STRANDS_COLOR_MIP_BIAS 1.0

// Strands have no texture alpha to test: their coverage is their geometry.
#undef DO_ALPHA_TEST

#if defined(VSHADER)
#	undef VSHADER
#	define HAIR_STRANDS_VSHADER
#endif

#include "Lighting.hlsl"

#if defined(HAIR_STRANDS_VSHADER)

#	include "HairStrands/Common.hlsli"

// The game's Lighting VS constants, still bound for the hair's pass.
cbuffer StrandPerTechnique : register(b0)
{
	float4 HighDetailRange : packoffset(c0);
	float4 FogParam : packoffset(c1);
	float4 FogNearColor : packoffset(c2);
	float4 FogFarColor : packoffset(c3);
};

cbuffer StrandPerMaterial : register(b1)
{
	float4 LeftEyeCenter : packoffset(c0);
	float4 RightEyeCenter : packoffset(c1);
	float4 TexcoordOffset : packoffset(c2);
};

cbuffer StrandPerFrame : register(b12)
{
	row_major float3x3 ScreenProj : packoffset(c0);
	row_major float4x4 Proj : packoffset(c4);
	row_major float4x4 ViewProj : packoffset(c8);
};

// Mirrors HairStrands::StrandDrawCB (StrandRenderer.h).
cbuffer StrandDraw : register(b7)
{
	uint PointsPerStrand;
	uint Subdivisions;
	float WidthScale;
	float MinWidthPerDistance;  // minimum ribbon width per unit of distance (a pixel floor)

	float3 EyeDelta;  // skinning camera to this pass's camera
	float RootWidth;

	float3 PreviousEyeDelta;
	float TipWidth;

	float WaveAmplitude;
	float WaveLength;
	float CurlRadius;
	float CurlLength;

	float CurlStart;
	float Frizz;
	float Flyaways;
	float CurlCoherence;  // 0: every strand curls on its own phase, 1: whole clumps spiral together
};

StructuredBuffer<HairStrands::RestPoint> RestPoints : register(t0);
StructuredBuffer<HairStrands::StrandInfo> Strands : register(t1);
StructuredBuffer<HairStrands::SkinnedPoint> SkinnedPoints : register(t2);

namespace HairStrandsVS
{
	static const float TwoPi = 6.28318530718;

	float3 CatmullRom(float3 p0, float3 p1, float3 p2, float3 p3, float t)
	{
		float t2 = t * t;
		float t3 = t2 * t;
		return 0.5 * (2.0 * p1 + (p2 - p0) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (3.0 * p1 - p0 - 3.0 * p2 + p3) * t3);
	}

	float3 CatmullRomTangent(float3 p0, float3 p1, float3 p2, float3 p3, float t)
	{
		return 0.5 * ((p2 - p0) + 2.0 * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t + 3.0 * (3.0 * p1 - p0 - 3.0 * p2 + p3) * t * t);
	}

	float3 Hash3(float n)
	{
		return frac(sin(float3(n, n + 1.31, n + 2.77)) * 43758.5453);
	}

	// Smooth value noise along the strand, in [-1, 1].
	float3 Noise3(float x, float seed)
	{
		float i = floor(x);
		float f = frac(x);
		float u = f * f * (3.0 - 2.0 * f);
		return lerp(Hash3(i + seed), Hash3(i + 1.0 + seed), u) * 2.0 - 1.0;
	}

	// Control points past either end are extrapolated so the spline reaches the ends.
	void LoadSegment(uint base, uint seg, out HairStrands::SkinnedPoint s1, out HairStrands::SkinnedPoint s2, out float3 p0, out float3 p3, out float3 q0, out float3 q3)
	{
		s1 = SkinnedPoints[base + seg];
		s2 = SkinnedPoints[base + seg + 1];
		if (seg > 0) {
			HairStrands::SkinnedPoint s0 = SkinnedPoints[base + seg - 1];
			p0 = s0.Position;
			q0 = s0.PreviousPosition;
		} else {
			p0 = 2.0 * s1.Position - s2.Position;
			q0 = 2.0 * s1.PreviousPosition - s2.PreviousPosition;
		}
		if (seg + 2 < PointsPerStrand) {
			HairStrands::SkinnedPoint s3 = SkinnedPoints[base + seg + 2];
			p3 = s3.Position;
			q3 = s3.PreviousPosition;
		} else {
			p3 = 2.0 * s2.Position - s1.Position;
			q3 = 2.0 * s2.PreviousPosition - s1.PreviousPosition;
		}
	}
}

VS_OUTPUT main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;

	// Triangle strip per strand: two vertices (ribbon sides) per render point.
	const uint renderPoints = (PointsPerStrand - 1) * Subdivisions + 1;
	const uint j = min(vertexID >> 1, renderPoints - 1);
	const float side = (vertexID & 1) ? 1.0 : -1.0;
	const uint seg = min(j / Subdivisions, PointsPerStrand - 2);
	const float f = saturate((j - seg * Subdivisions) / (float)Subdivisions);

	const uint base = instanceID * PointsPerStrand;
	HairStrands::SkinnedPoint s1, s2;
	float3 p0, p3, q0, q3;
	HairStrandsVS::LoadSegment(base, seg, s1, s2, p0, p3, q0, q3);
	const HairStrands::RestPoint r1 = RestPoints[base + seg];
	const HairStrands::RestPoint r2 = RestPoints[base + seg + 1];
	const HairStrands::StrandInfo info = Strands[instanceID];

	float3 position = HairStrandsVS::CatmullRom(p0, s1.Position, s2.Position, p3, f) + EyeDelta;
	float3 previousPosition = HairStrandsVS::CatmullRom(q0, s1.PreviousPosition, s2.PreviousPosition, q3, f) + PreviousEyeDelta;
	float3 tangent = HairStrandsVS::CatmullRomTangent(p0, s1.Position, s2.Position, p3, f);
	tangent = dot(tangent, tangent) > 1e-10 ? normalize(tangent) : normalize(s2.Position - s1.Position + 1e-5);

	// Strand frame: the card's (shading) normal made perpendicular to the strand.
	float3 normal = lerp(s1.Normal, s2.Normal, f);
	normal -= tangent * dot(normal, tangent);
	normal = dot(normal, normal) > 1e-10 ? normalize(normal) : float3(0, 0, 1);
	const float3 binormal = cross(tangent, normal);

	const float t = lerp(r1.T, r2.T, f);
	const float s = t * info.Length;  // distance from the root

	// Procedural style, identical this frame and last so it adds no false motion.
	float3 offset = 0;
	{
		const float phase = HairStrandsVS::TwoPi * lerp(info.Random, info.ClumpRandom, CurlCoherence);
		const float curlRamp = smoothstep(0.0, max(CurlStart, 1e-3), t);
		const float curlAngle = HairStrandsVS::TwoPi * s / CurlLength + phase;
		offset += CurlRadius * curlRamp * (cos(curlAngle) * normal + sin(curlAngle) * binormal);

		const float wavePlane = info.ClumpRandom * 3.14159265;
		const float3 waveDir = cos(wavePlane) * binormal + sin(wavePlane) * normal;
		offset += WaveAmplitude * smoothstep(0.0, 0.2, t) * sin(HairStrandsVS::TwoPi * s / WaveLength + info.ClumpRandom * 6.0) * waveDir;

		const bool flyaway = frac(info.Random * 13.37) < Flyaways;
		const float frizz = Frizz * t * t + (flyaway ? 0.4 * t : 0.0);
		const float3 noise = HairStrandsVS::Noise3(s * 1.5, info.Random * 97.0);
		offset += frizz * (noise.x * normal + noise.y * binormal + 0.5 * noise.z * tangent);
		if (flyaway)
			offset += normal * (0.6 * t * t);
	}
	position += offset;
	previousPosition += offset;

	// Camera-facing ribbon (the camera is at the origin of camera-relative space).
	const float distance = length(position);
	const float3 viewDir = position / max(distance, 1e-4);
	float3 sideDir = cross(tangent, viewDir);
	sideDir = dot(sideDir, sideDir) > 1e-8 ? normalize(sideDir) : binormal;
	const float width = max(lerp(RootWidth, TipWidth, t) * WidthScale, MinWidthPerDistance * distance);
	position += sideDir * (0.5 * width * side);
	previousPosition += sideDir * (0.5 * width * side);

	const float4 worldPosition = float4(position, 1.0);
	const float4 viewPos = mul(ViewProj, worldPosition);
	vsout.Position = viewPos;

	const float2 uv = lerp(float2(r1.U, r1.V), float2(r2.U, r2.V), f);
	vsout.TexCoord0.xy = uv * TexcoordOffset.zw + TexcoordOffset.xy;

#	if defined(SKINNED) || !defined(MODELSPACENORMALS)
	// Columns: tangent (across the ribbon), bitangent (along the strand, Hair Specular's
	// hair direction) and normal.
	vsout.TBN0.xyz = float3(sideDir.x, tangent.x, normal.x);
	vsout.TBN1.xyz = float3(sideDir.y, tangent.y, normal.y);
	vsout.TBN2.xyz = float3(sideDir.z, tangent.z, normal.z);
#	endif

	vsout.WorldPosition = worldPosition;
	vsout.PreviousWorldPosition = float4(previousPosition, 1.0);
	vsout.Color = 1.0.xxxx;

	const bool reverseProjection = FrameBuffer::IsReverseProjection(Proj);
	float fogColorParam = min(FogParam.w,
		exp2(FogParam.z * log2(saturate(length(FrameBuffer::ToStandardClip(viewPos, reverseProjection)) * FogParam.y - FogParam.x))));
	vsout.FogParam.xyz = lerp(FogNearColor.xyz, FogFarColor.xyz, fogColorParam);
	vsout.FogParam.w = fogColorParam;

	vsout.ModelPosition = lerp(r1.Position, r2.Position, f);

	return vsout;
}

#endif  // HAIR_STRANDS_VSHADER
