// =============================================================================
// NeckSeamFixCS.hlsl
//
// Reconstructs the thin seam between Skyrim's head and body skin surfaces.
//
// The pass operates before screen-space lighting consumers like SSGI/SSS:
//   1. Gather nearby skin pixels around the current screen pixel.
//   2. Find opposing skin support across the seam (left/right or up/down).
//   3. If the current pixel is a hole or exposed interior, reconstruct a skin
//      surface for MAIN, ALBEDO, NORMALROUGHNESS, MASKS, and depth.
//   4. If the current pixel is already skin but sits on a seam edge, feather
//      it toward the opposing side so touching head/body edges blend together.
//
// Register layout (must match NeckSeamFix.cpp)
//   t0  = DepthTexture             (raw scene depth SRV)
//   t1  = MaskTexture              (MASKS render target; .x > 0 = skin)
//   t2  = MainTexture              (source direct lighting color)
//   t3  = AlbedoTexture            (source albedo)
//   t4  = NormalRoughnessTexture   (source encoded normal + gloss)
//   u0  = MainOut                  (seam-fixed direct lighting)
//   u1  = AlbedoOut                (seam-fixed albedo)
//   u2  = NormalRoughnessOut       (seam-fixed encoded normal + gloss)
//   u3  = MaskOut                  (seam-fixed mask data)
//   u4  = DepthOut                 (seam-fixed raw R32 depth)
//   u5  = DepthOut16               (seam-fixed raw R16 depth)
//   b1  = NeckSeamCB               (SearchRadius, DepthThreshold, BlendStrength)
// =============================================================================

#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> MaskTexture : register(t1);
Texture2D<float4> MainTexture : register(t2);
Texture2D<float4> AlbedoTexture : register(t3);
Texture2D<float4> NormalRoughnessTexture : register(t4);

RWTexture2D<float4> MainOut : register(u0);
RWTexture2D<unorm float4> AlbedoOut : register(u1);
RWTexture2D<unorm float4> NormalRoughnessOut : register(u2);
RWTexture2D<float4> MaskOut : register(u3);
RWTexture2D<float> DepthOut : register(u4);
RWTexture2D<unorm half> DepthOut16 : register(u5);

cbuffer NeckSeamCB : register(b1)
{
	float SearchRadius;
	float DepthThreshold;
	float BlendStrength;
	float pad;
};

static const float kSkinEpsilon = 1e-4f;
static const float kColorThreshold = 0.03f;
static const float kColorRange = 0.10f;
static const float kSkinNormalThreshold = 0.75f;
static const float kInteriorNormalThreshold = 0.35f;

bool IsValidSceneDepth(float rawDepth)
{
	return rawDepth < 0.9999f;
}

struct Accumulator
{
	float weight;
	float rawDepth;
	float linearDepth;
	float glossiness;
	float3 mainColor;
	float4 albedo;
	float4 mask;
	float3 normal;
};

void AddSample(
	inout Accumulator accum,
	float weight,
	float rawDepth,
	float linearDepth,
	float3 mainColor,
	float4 albedo,
	float4 normalRoughness,
	float4 mask)
{
	accum.weight += weight;
	accum.rawDepth += rawDepth * weight;
	accum.linearDepth += linearDepth * weight;
	accum.glossiness += normalRoughness.z * weight;
	accum.mainColor += mainColor * weight;
	accum.albedo += albedo * weight;
	accum.mask += mask * weight;
	accum.normal += GBuffer::DecodeNormal(normalRoughness.xy) * weight;
}

Accumulator CombineAccum(Accumulator a, Accumulator b)
{
	Accumulator combined = (Accumulator)0;
	combined.weight = a.weight + b.weight;
	combined.rawDepth = a.rawDepth + b.rawDepth;
	combined.linearDepth = a.linearDepth + b.linearDepth;
	combined.glossiness = a.glossiness + b.glossiness;
	combined.mainColor = a.mainColor + b.mainColor;
	combined.albedo = a.albedo + b.albedo;
	combined.mask = a.mask + b.mask;
	combined.normal = a.normal + b.normal;
	return combined;
}

float3 AverageNormal(Accumulator accum, float3 fallbackNormal)
{
	if (accum.weight <= 0.0f)
		return fallbackNormal;

	float3 avgNormal = accum.normal / accum.weight;
	float normalLength = length(avgNormal);
	if (normalLength <= 1e-5f)
		return fallbackNormal;

	return avgNormal / normalLength;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint2 bufDim = uint2(SharedData::BufferDim.xy);
	if (any(DTid.xy >= bufDim))
		return;

	int2 pixCoord = int2(DTid.xy);
	int radius = max(1, (int)round(SearchRadius));
	float blendStrength = saturate(BlendStrength);

	float rawCenterDepth = DepthTexture.Load(int3(pixCoord, 0)).x;
	bool centerHasGeometry = IsValidSceneDepth(rawCenterDepth);
	float linearCenterDepth = centerHasGeometry ? SharedData::GetScreenDepth(rawCenterDepth) : 0.0f;

	float4 sourceMain = MainTexture.Load(int3(pixCoord, 0));
	float4 sourceAlbedo = AlbedoTexture.Load(int3(pixCoord, 0));
	float4 sourceNormalRoughness = NormalRoughnessTexture.Load(int3(pixCoord, 0));
	float4 sourceMask = MaskTexture.Load(int3(pixCoord, 0));

	float centerSkinMask = sourceMask.x;
	bool centerIsSkin = centerSkinMask > kSkinEpsilon;
	float3 centerNormal = centerHasGeometry ? GBuffer::DecodeNormal(sourceNormalRoughness.xy) : float3(0.0f, 0.0f, 1.0f);

	Accumulator total = (Accumulator)0;
	Accumulator left = (Accumulator)0;
	Accumulator right = (Accumulator)0;
	Accumulator up = (Accumulator)0;
	Accumulator down = (Accumulator)0;

	bool hasNearbyGapNeighbour = false;

	float neighbourDepthThreshold = max(DepthThreshold * 2.0f, 0.01f);

	for (int dy = -radius; dy <= radius; ++dy)
	{
		for (int dx = -radius; dx <= radius; ++dx)
		{
			if (dx == 0 && dy == 0)
				continue;

			int2 sampleCoord = clamp(pixCoord + int2(dx, dy), int2(0, 0), int2(bufDim) - 1);

			float rawNeighbourDepth = DepthTexture.Load(int3(sampleCoord, 0)).x;
			bool neighbourHasGeometry = IsValidSceneDepth(rawNeighbourDepth);

			float4 neighbourMask = MaskTexture.Load(int3(sampleCoord, 0));
			bool neighbourIsSkin = neighbourMask.x > kSkinEpsilon;

			if (centerIsSkin && !neighbourIsSkin) {
				if (!neighbourHasGeometry) {
					hasNearbyGapNeighbour = true;
				} else if (centerHasGeometry) {
					float linearNeighbourDepth = SharedData::GetScreenDepth(rawNeighbourDepth);
					if (linearNeighbourDepth > linearCenterDepth + neighbourDepthThreshold)
						hasNearbyGapNeighbour = true;
				}
			}

			if (!neighbourIsSkin || !neighbourHasGeometry)
				continue;

			float linearNeighbourDepth = SharedData::GetScreenDepth(rawNeighbourDepth);
			float4 neighbourMain = MainTexture.Load(int3(sampleCoord, 0));
			float4 neighbourAlbedo = AlbedoTexture.Load(int3(sampleCoord, 0));
			float4 neighbourNormalRoughness = NormalRoughnessTexture.Load(int3(sampleCoord, 0));

			float dist = length(float2(dx, dy));
			float weight = 1.0f / max(dist, 0.001f);

			AddSample(total, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);

			if (abs(dx) >= abs(dy)) {
				if (dx < 0)
					AddSample(left, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
				else
					AddSample(right, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
			}

			if (abs(dy) >= abs(dx)) {
				if (dy < 0)
					AddSample(up, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
				else
					AddSample(down, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
			}
		}
	}

	float4 outMain = sourceMain;
	float4 outAlbedo = sourceAlbedo;
	float4 outMask = sourceMask;
	float4 outNormalRoughness = sourceNormalRoughness;
	float outRawDepth = rawCenterDepth;

	if (blendStrength > 0.0f && total.weight > 0.0f) {
		bool horizontalValid = left.weight > 0.0f && right.weight > 0.0f;
		bool verticalValid = up.weight > 0.0f && down.weight > 0.0f;

		float horizontalDepthDelta = 1e30f;
		if (horizontalValid) {
			float leftDepth = left.linearDepth / left.weight;
			float rightDepth = right.linearDepth / right.weight;
			horizontalDepthDelta = abs(leftDepth - rightDepth);
			horizontalValid = horizontalDepthDelta <= neighbourDepthThreshold;
		}

		float verticalDepthDelta = 1e30f;
		if (verticalValid) {
			float upDepth = up.linearDepth / up.weight;
			float downDepth = down.linearDepth / down.weight;
			verticalDepthDelta = abs(upDepth - downDepth);
			verticalValid = verticalDepthDelta <= neighbourDepthThreshold;
		}

		if (horizontalValid || verticalValid) {
			bool useHorizontal = horizontalValid && (!verticalValid || (left.weight + right.weight) >= (up.weight + down.weight));
			Accumulator axisAccum = useHorizontal ? CombineAccum(left, right) : CombineAccum(up, down);
			Accumulator sideA = useHorizontal ? left : up;
			Accumulator sideB = useHorizontal ? right : down;

			float axisWeight = max(axisAccum.weight, 1e-5f);
			float4 axisAlbedo = axisAccum.albedo / axisWeight;
			float4 axisMask = axisAccum.mask / axisWeight;
			float3 axisMain = axisAccum.mainColor / axisWeight;
			float axisRawDepth = axisAccum.rawDepth / axisWeight;
			float axisLinearDepth = axisAccum.linearDepth / axisWeight;
			float axisGlossiness = axisAccum.glossiness / axisWeight;
			float3 axisNormal = AverageNormal(axisAccum, centerNormal);

			float3 sideAAlbedo = sideA.albedo.xyz / max(sideA.weight, 1e-5f);
			float3 sideBAlbedo = sideB.albedo.xyz / max(sideB.weight, 1e-5f);
			float axisColorContrast = length(sideAAlbedo - sideBAlbedo);
			float centerAlbedoContrast = length(sourceAlbedo.xyz - axisAlbedo.xyz);
			float centerMainContrast = length(sourceMain.rgb - axisMain);
			float seamContrast = max(axisColorContrast, max(centerAlbedoContrast, centerMainContrast));
			float seamFactor = saturate((seamContrast - kColorThreshold) / kColorRange);

			float centerNormalDotAxis = centerHasGeometry ? saturate(dot(centerNormal, axisNormal)) : 1.0f;
			bool holeCandidate = !centerHasGeometry || linearCenterDepth > axisLinearDepth + DepthThreshold;
			bool interiorCandidate =
				!centerIsSkin &&
				centerHasGeometry &&
				!holeCandidate &&
				centerNormalDotAxis < kInteriorNormalThreshold &&
				(centerMainContrast > kColorThreshold || centerAlbedoContrast > kColorThreshold);

			float2 encodedAxisNormal = GBuffer::EncodeNormal(axisNormal);
			float4 targetNormalRoughness = float4(encodedAxisNormal, axisGlossiness, sourceNormalRoughness.w);

			if (holeCandidate || interiorCandidate) {
				outMain = float4(lerp(sourceMain.rgb, axisMain, blendStrength), sourceMain.a);
				outAlbedo = float4(lerp(sourceAlbedo.rgb, axisAlbedo.rgb, blendStrength), sourceAlbedo.a);
				outMask = float4(lerp(sourceMask.rgb, axisMask.rgb, blendStrength), sourceMask.a);
				outNormalRoughness = float4(lerp(sourceNormalRoughness.xyz, targetNormalRoughness.xyz, blendStrength), sourceNormalRoughness.w);
				outRawDepth = axisRawDepth;
			} else {
				bool skinBlendCandidate =
					centerIsSkin &&
					centerHasGeometry &&
					abs(linearCenterDepth - axisLinearDepth) <= neighbourDepthThreshold &&
					centerNormalDotAxis >= kSkinNormalThreshold &&
					(hasNearbyGapNeighbour || seamFactor > 0.0f);

				if (skinBlendCandidate) {
					float alignmentFactor = saturate((centerNormalDotAxis - kSkinNormalThreshold) / (1.0f - kSkinNormalThreshold));
					float baseBlend = hasNearbyGapNeighbour ? max(0.5f, seamFactor) : seamFactor;
					float edgeBlend = saturate(blendStrength * baseBlend * alignmentFactor);

					outMain = float4(lerp(sourceMain.rgb, axisMain, edgeBlend), sourceMain.a);
					outAlbedo = float4(lerp(sourceAlbedo.rgb, axisAlbedo.rgb, edgeBlend), sourceAlbedo.a);
					outMask = float4(lerp(sourceMask.rgb, axisMask.rgb, edgeBlend), sourceMask.a);
					outNormalRoughness = float4(lerp(sourceNormalRoughness.xyz, targetNormalRoughness.xyz, edgeBlend), sourceNormalRoughness.w);
				}
			}
		}
	}

	MainOut[pixCoord] = outMain;
	AlbedoOut[pixCoord] = outAlbedo;
	NormalRoughnessOut[pixCoord] = outNormalRoughness;
	MaskOut[pixCoord] = outMask;
	DepthOut[pixCoord] = outRawDepth;
	DepthOut16[pixCoord] = outRawDepth;
}
