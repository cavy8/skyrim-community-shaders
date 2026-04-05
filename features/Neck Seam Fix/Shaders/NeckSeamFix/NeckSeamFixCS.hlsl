// =============================================================================
// NeckSeamFixCS.hlsl
//
// Geometry-assisted body seam reconstruction.
//
// The body mesh is tagged during the deferred lighting draw. This pass then
// looks for a seam where that tagged body surface meets another skin surface
// (head, hands, feet, etc.), fills narrow gaps between them, and feathers the
// edge pixels toward the opposing surface.
// =============================================================================

#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> MaskTexture : register(t1);
Texture2D<float4> BodyLabelTexture : register(t2);
Texture2D<float4> MainTexture : register(t3);
Texture2D<float4> AlbedoTexture : register(t4);
Texture2D<float4> NormalRoughnessTexture : register(t5);

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
static const float kBodyLabelThreshold = 0.5f;
static const float kSeamSignalFloor = 0.15f;
static const float kNormalSignalScale = 0.5f;

bool IsValidSceneDepth(float rawDepth)
{
	return rawDepth < 0.9999f;
}

bool IsBodyLabel(float4 labels)
{
	return labels.z > kBodyLabelThreshold;
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

	float3 average = accum.normal / accum.weight;
	float averageLength = length(average);
	if (averageLength <= 1e-5f)
		return fallbackNormal;

	return average / averageLength;
}

float3 AverageMain(Accumulator accum)
{
	return accum.mainColor / max(accum.weight, 1e-5f);
}

float4 AverageAlbedo(Accumulator accum)
{
	return accum.albedo / max(accum.weight, 1e-5f);
}

float4 AverageMask(Accumulator accum)
{
	return accum.mask / max(accum.weight, 1e-5f);
}

float AverageRawDepth(Accumulator accum)
{
	return accum.rawDepth / max(accum.weight, 1e-5f);
}

float AverageLinearDepth(Accumulator accum)
{
	return accum.linearDepth / max(accum.weight, 1e-5f);
}

float AverageGlossiness(Accumulator accum)
{
	return accum.glossiness / max(accum.weight, 1e-5f);
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
	float4 sourceLabels = BodyLabelTexture.Load(int3(pixCoord, 0));

	bool centerIsSkin = sourceMask.x > kSkinEpsilon;
	bool centerIsBody = centerIsSkin && IsBodyLabel(sourceLabels);
	bool centerIsOtherSkin = centerIsSkin && !centerIsBody;
	float3 centerNormal = centerHasGeometry ? GBuffer::DecodeNormal(sourceNormalRoughness.xy) : float3(0.0f, 0.0f, 1.0f);

	Accumulator leftBody = (Accumulator)0;
	Accumulator rightBody = (Accumulator)0;
	Accumulator upBody = (Accumulator)0;
	Accumulator downBody = (Accumulator)0;
	Accumulator leftOther = (Accumulator)0;
	Accumulator rightOther = (Accumulator)0;
	Accumulator upOther = (Accumulator)0;
	Accumulator downOther = (Accumulator)0;

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
			float4 neighbourLabels = BodyLabelTexture.Load(int3(sampleCoord, 0));
			bool neighbourIsBody = IsBodyLabel(neighbourLabels);
			float4 neighbourMain = MainTexture.Load(int3(sampleCoord, 0));
			float4 neighbourAlbedo = AlbedoTexture.Load(int3(sampleCoord, 0));
			float4 neighbourNormalRoughness = NormalRoughnessTexture.Load(int3(sampleCoord, 0));

			float dist = length(float2(dx, dy));
			float weight = 1.0f / max(dist, 0.001f);

			if (abs(dx) >= abs(dy)) {
				if (dx < 0) {
					if (neighbourIsBody)
						AddSample(leftBody, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
					else
						AddSample(leftOther, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
				} else {
					if (neighbourIsBody)
						AddSample(rightBody, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
					else
						AddSample(rightOther, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
				}
			}

			if (abs(dy) >= abs(dx)) {
				if (dy < 0) {
					if (neighbourIsBody)
						AddSample(upBody, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
					else
						AddSample(upOther, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
				} else {
					if (neighbourIsBody)
						AddSample(downBody, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
					else
						AddSample(downOther, weight, rawNeighbourDepth, linearNeighbourDepth, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourMask);
				}
			}
		}
	}

	Accumulator bodyAccum = (Accumulator)0;
	Accumulator otherAccum = (Accumulator)0;
	bool seamAxisFound = false;
	float bestAxisWeight = 0.0f;

	#define TRY_AXIS(BODY_A, OTHER_B) \
	{ \
		if ((BODY_A).weight > 0.0f && (OTHER_B).weight > 0.0f) { \
			float axisDepthDelta = abs(AverageLinearDepth(BODY_A) - AverageLinearDepth(OTHER_B)); \
			if (axisDepthDelta <= neighbourDepthThreshold) { \
				float axisWeight = (BODY_A).weight + (OTHER_B).weight; \
				if (!seamAxisFound || axisWeight > bestAxisWeight) { \
					seamAxisFound = true; \
					bestAxisWeight = axisWeight; \
					bodyAccum = (BODY_A); \
					otherAccum = (OTHER_B); \
				} \
			} \
		} \
	}

	TRY_AXIS(leftBody, rightOther);
	TRY_AXIS(rightBody, leftOther);
	TRY_AXIS(upBody, downOther);
	TRY_AXIS(downBody, upOther);

	#undef TRY_AXIS

	float4 outMain = sourceMain;
	float4 outAlbedo = sourceAlbedo;
	float4 outMask = sourceMask;
	float4 outNormalRoughness = sourceNormalRoughness;
	float outRawDepth = rawCenterDepth;

	if (blendStrength > 0.0f && seamAxisFound) {
		Accumulator seamAccum = CombineAccum(bodyAccum, otherAccum);
		float3 seamMain = AverageMain(seamAccum);
		float4 seamAlbedo = AverageAlbedo(seamAccum);
		float4 seamMask = AverageMask(seamAccum);
		float seamRawDepth = min(AverageRawDepth(bodyAccum), AverageRawDepth(otherAccum));
		float seamLinearDepth = min(AverageLinearDepth(bodyAccum), AverageLinearDepth(otherAccum));
		float seamGlossiness = 0.5f * (AverageGlossiness(bodyAccum) + AverageGlossiness(otherAccum));

		float3 bodyMain = AverageMain(bodyAccum);
		float3 otherMain = AverageMain(otherAccum);
		float4 bodyAlbedo = AverageAlbedo(bodyAccum);
		float4 otherAlbedo = AverageAlbedo(otherAccum);
		float4 bodyMask = AverageMask(bodyAccum);
		float4 otherMask = AverageMask(otherAccum);
		float bodyRawDepth = AverageRawDepth(bodyAccum);
		float otherRawDepth = AverageRawDepth(otherAccum);
		float bodyLinearDepth = AverageLinearDepth(bodyAccum);
		float otherLinearDepth = AverageLinearDepth(otherAccum);
		float bodyGlossiness = AverageGlossiness(bodyAccum);
		float otherGlossiness = AverageGlossiness(otherAccum);
		float3 bodyNormal = AverageNormal(bodyAccum, centerNormal);
		float3 otherNormal = AverageNormal(otherAccum, centerNormal);
		float3 seamNormal = normalize(bodyNormal + otherNormal);

		float colorSignal = max(length(bodyAlbedo.xyz - otherAlbedo.xyz), length(bodyMain - otherMain));
		float normalSignal = 1.0f - saturate(dot(bodyNormal, otherNormal));
		float seamSignal = max(colorSignal, normalSignal * kNormalSignalScale);
		float seamBlend = max(kSeamSignalFloor, seamSignal);

		float centerNormalToSeam = centerHasGeometry ? saturate(dot(centerNormal, seamNormal)) : 1.0f;
		bool holeCandidate = !centerHasGeometry || linearCenterDepth > seamLinearDepth + DepthThreshold;
		bool interiorCandidate =
			!centerIsSkin &&
			centerHasGeometry &&
			linearCenterDepth <= max(bodyLinearDepth, otherLinearDepth) + neighbourDepthThreshold;

		float2 encodedSeamNormal = GBuffer::EncodeNormal(seamNormal);
		float4 seamNormalRoughness = float4(encodedSeamNormal, seamGlossiness, sourceNormalRoughness.w);

		if (holeCandidate || interiorCandidate) {
			float fillBlend = saturate(blendStrength * max(0.6f, seamBlend));
			outMain = float4(lerp(sourceMain.rgb, seamMain, fillBlend), sourceMain.a);
			outAlbedo = float4(lerp(sourceAlbedo.rgb, seamAlbedo.rgb, fillBlend), sourceAlbedo.a);
			outMask = float4(lerp(sourceMask.rgb, seamMask.rgb, fillBlend), sourceMask.a);
			outNormalRoughness = float4(lerp(sourceNormalRoughness.xyz, seamNormalRoughness.xyz, fillBlend), sourceNormalRoughness.w);
			outRawDepth = seamRawDepth;
		} else if (centerIsBody || centerIsOtherSkin) {
			float sourceSideLinearDepth = centerIsBody ? bodyLinearDepth : otherLinearDepth;
			float targetSideLinearDepth = centerIsBody ? otherLinearDepth : bodyLinearDepth;
			float sourceDepthDelta = abs(linearCenterDepth - sourceSideLinearDepth);
			bool seamEdgeCandidate =
				sourceDepthDelta <= neighbourDepthThreshold &&
				(hasNearbyGapNeighbour || seamSignal > 0.0f || centerNormalToSeam < 0.98f);

			if (seamEdgeCandidate) {
				float3 targetMain = centerIsBody ? otherMain : bodyMain;
				float4 targetAlbedo = centerIsBody ? otherAlbedo : bodyAlbedo;
				float4 targetMask = centerIsBody ? otherMask : bodyMask;
				float3 targetNormal = centerIsBody ? otherNormal : bodyNormal;
				float targetGlossiness = centerIsBody ? otherGlossiness : bodyGlossiness;
				float targetRawDepth = centerIsBody ? otherRawDepth : bodyRawDepth;

				float depthCloseness = 1.0f - saturate(sourceDepthDelta / max(neighbourDepthThreshold, 1e-5f));
				float edgeBlend = saturate(blendStrength * max(0.35f, seamBlend) * max(depthCloseness, 0.5f));

				float2 encodedTargetNormal = GBuffer::EncodeNormal(targetNormal);
				float4 targetNormalRoughness = float4(encodedTargetNormal, targetGlossiness, sourceNormalRoughness.w);

				outMain = float4(lerp(sourceMain.rgb, targetMain, edgeBlend), sourceMain.a);
				outAlbedo = float4(lerp(sourceAlbedo.rgb, targetAlbedo.rgb, edgeBlend), sourceAlbedo.a);
				outMask = float4(lerp(sourceMask.rgb, targetMask.rgb, edgeBlend), sourceMask.a);
				outNormalRoughness = float4(lerp(sourceNormalRoughness.xyz, targetNormalRoughness.xyz, edgeBlend), sourceNormalRoughness.w);
				outRawDepth = lerp(outRawDepth, targetRawDepth, edgeBlend * 0.35f);
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
