// =============================================================================
// NeckSeamFixLateCS.hlsl
//
// Post-composite seam color correction.
//
// Runs after DeferredCompositeCS. It keeps the current pixel's detail and applies
// only a gamma-space low-frequency color offset toward the seam consensus color.
// =============================================================================

#include "Common/SharedData.hlsli"

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> LabelTexture : register(t1);
Texture2D<float4> MainTexture : register(t2);

RWTexture2D<float4> MainOut : register(u0);

cbuffer NeckSeamCB : register(b1)
{
	float SearchRadius;
	float DepthThreshold;
	float BlendStrength;
	float LateSearchRadius;
	float LateBlendStrength;
	float3 pad;
};

static const float kLabelThreshold = 0.5f;

bool IsValidSceneDepth(float rawDepth)
{
	return rawDepth < 0.9999f;
}

bool IsTaggedSkin(float4 labels)
{
	return labels.x > kLabelThreshold;
}

float DecodeObjectId(float4 labels)
{
	return floor(labels.y * 255.0f + 0.5f) + floor(labels.z * 255.0f + 0.5f) * 256.0f;
}

struct SideAccumulator
{
	float weight;
	float objectId;
	float3 color;
};

void AddSample(inout SideAccumulator accum, float weight, float objectId, float3 color)
{
	accum.weight += weight;
	accum.objectId += objectId * weight;
	accum.color += color * weight;
}

float AverageObjectId(SideAccumulator accum)
{
	return floor(accum.objectId / max(accum.weight, 1e-5f) + 0.5f);
}

float3 AverageColor(SideAccumulator accum)
{
	return accum.color / max(accum.weight, 1e-5f);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint2 bufDim = uint2(SharedData::BufferDim.xy);
	if (any(DTid.xy >= bufDim))
		return;

	int2 pixCoord = int2(DTid.xy);
	float lateBlendStrength = saturate(LateBlendStrength);
	float4 sourceMain = MainTexture.Load(int3(pixCoord, 0));

	if (lateBlendStrength <= 0.0f) {
		MainOut[pixCoord] = sourceMain;
		return;
	}

	int radius = max(1, (int)round(LateSearchRadius));
	float rawCenterDepth = DepthTexture.Load(int3(pixCoord, 0)).x;
	float4 centerLabels = LabelTexture.Load(int3(pixCoord, 0));
	float centerObjectId = DecodeObjectId(centerLabels);
	bool centerIsSkin = IsTaggedSkin(centerLabels) && centerObjectId > 0.0f;

	SideAccumulator left = (SideAccumulator)0;
	SideAccumulator right = (SideAccumulator)0;
	SideAccumulator up = (SideAccumulator)0;
	SideAccumulator down = (SideAccumulator)0;

	for (int dy = -radius; dy <= radius; ++dy)
	{
		for (int dx = -radius; dx <= radius; ++dx)
		{
			if (dx == 0 && dy == 0)
				continue;

			int2 sampleCoord = clamp(pixCoord + int2(dx, dy), int2(0, 0), int2(bufDim) - 1);
			float rawNeighbourDepth = DepthTexture.Load(int3(sampleCoord, 0)).x;
			if (!IsValidSceneDepth(rawNeighbourDepth))
				continue;
			if (abs(rawNeighbourDepth - rawCenterDepth) > DepthThreshold)
				continue;

			float4 neighbourLabels = LabelTexture.Load(int3(sampleCoord, 0));
			float neighbourObjectId = DecodeObjectId(neighbourLabels);
			bool neighbourIsSkin = IsTaggedSkin(neighbourLabels) && neighbourObjectId > 0.0f;
			if (!neighbourIsSkin)
				continue;

			float dist = length(float2(dx, dy));
			float weight = 1.0f / max(dist, 0.001f);
			float3 neighbourColor = MainTexture.Load(int3(sampleCoord, 0)).rgb;

			if (abs(dx) >= abs(dy)) {
				if (dx < 0)
					AddSample(left, weight, neighbourObjectId, neighbourColor);
				else
					AddSample(right, weight, neighbourObjectId, neighbourColor);
			}

			if (abs(dy) >= abs(dx)) {
				if (dy < 0)
					AddSample(up, weight, neighbourObjectId, neighbourColor);
				else
					AddSample(down, weight, neighbourObjectId, neighbourColor);
			}
		}
	}

	SideAccumulator sideA = (SideAccumulator)0;
	SideAccumulator sideB = (SideAccumulator)0;
	bool seamAxisFound = false;
	float bestAxisWeight = 0.0f;

	#define TRY_AXIS(ACCUM_A, ACCUM_B) \
	{ \
		if ((ACCUM_A).weight > 0.0f && (ACCUM_B).weight > 0.0f) { \
			float objectIdA = AverageObjectId(ACCUM_A); \
			float objectIdB = AverageObjectId(ACCUM_B); \
			bool axisHasDifferentObjects = objectIdA > 0.0f && objectIdB > 0.0f && objectIdA != objectIdB; \
			bool centerMatchesAxis = !centerIsSkin || abs(centerObjectId - objectIdA) < 0.5f || abs(centerObjectId - objectIdB) < 0.5f; \
			if (axisHasDifferentObjects && centerMatchesAxis) { \
				float axisWeight = (ACCUM_A).weight + (ACCUM_B).weight; \
				if (!seamAxisFound || axisWeight > bestAxisWeight) { \
					seamAxisFound = true; \
					bestAxisWeight = axisWeight; \
					sideA = (ACCUM_A); \
					sideB = (ACCUM_B); \
				} \
			} \
		} \
	}

	TRY_AXIS(left, right);
	TRY_AXIS(up, down);

	#undef TRY_AXIS

	if (!seamAxisFound) {
		MainOut[pixCoord] = sourceMain;
		return;
	}

	float sideAObjectId = AverageObjectId(sideA);
	float sideBObjectId = AverageObjectId(sideB);
	float3 meanA = AverageColor(sideA);
	float3 meanB = AverageColor(sideB);
	float3 midpoint = 0.5f * (meanA + meanB);

	float sameSideWeight = 0.0f;
	float opposingSideWeight = 0.0f;
	float3 mySideMean = midpoint;

	if (centerIsSkin && abs(centerObjectId - sideAObjectId) < 0.5f) {
		sameSideWeight = sideA.weight;
		opposingSideWeight = sideB.weight;
		mySideMean = meanA;
	} else if (centerIsSkin && abs(centerObjectId - sideBObjectId) < 0.5f) {
		sameSideWeight = sideB.weight;
		opposingSideWeight = sideA.weight;
		mySideMean = meanB;
	} else {
		// Gap pixel — blend fully toward midpoint
		opposingSideWeight = 1.0f;
	}

	// seamProximity: 0 deep inside one mesh, ~0.5 at the seam boundary.
	// Remap so boundary (0.5) → 1.0 for full gradient effect there.
	float seamProximity = opposingSideWeight / max(sameSideWeight + opposingSideWeight, 1e-5f);
	float t = saturate(seamProximity * 2.0f);

	// Decompose this pixel's color into base tone + high-frequency detail:
	//   detail = pores, SSS variation, specular highlights, shadow gradients
	//   mySideMean = average skin tone for this mesh
	float3 detail = sourceMain.rgb - mySideMean;

	// Smooth gradient from this side's mean toward the midpoint of both sides.
	// At t=0 (deep inside): gradient = mySideMean → corrected = original
	// At t=1 (at boundary): gradient = midpoint   → corrected = midpoint + detail
	float3 gradient = lerp(mySideMean, midpoint, t);
	float3 corrected = gradient + detail;

	float3 finalColor = lerp(sourceMain.rgb, corrected, lateBlendStrength);
	MainOut[pixCoord] = float4(max(0.0f, finalColor), sourceMain.a);
}
