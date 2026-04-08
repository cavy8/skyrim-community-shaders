// =============================================================================
// NeckSeamFixLateCS.hlsl
//
// Post-composite seam color projection.
//
// Runs after DeferredCompositeCS. Instead of averaging a 2D neighborhood, this
// pass traces tiny screen-space cardinal rays across the detected seam and
// transfers a clamped low-frequency color offset from the opposing skin mesh.
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
static const float kMaxColorOffset = 0.15f;

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

struct RayHit
{
	bool hit;
	float objectId;
	float distance;
	float3 color;
};

RayHit EmptyHit()
{
	RayHit hit;
	hit.hit = false;
	hit.objectId = 0.0f;
	hit.distance = 0.0f;
	hit.color = 0.0f;
	return hit;
}

RayHit TraceSkinRay(int2 pixCoord, int2 direction, int radius, uint2 bufDim)
{
	RayHit hit = EmptyHit();

	for (int step = 1; step <= radius; ++step)
	{
		int2 sampleCoord = pixCoord + direction * step;
		if (any(sampleCoord < int2(0, 0)) || any(sampleCoord >= int2(bufDim)))
			break;

		float rawDepth = DepthTexture.Load(int3(sampleCoord, 0)).x;
		if (!IsValidSceneDepth(rawDepth))
			continue;

		float4 labels = LabelTexture.Load(int3(sampleCoord, 0));
		float objectId = DecodeObjectId(labels);
		if (!IsTaggedSkin(labels) || objectId <= 0.0f)
			continue;

		hit.hit = true;
		hit.objectId = objectId;
		hit.distance = (float)step;
		hit.color = MainTexture.Load(int3(sampleCoord, 0)).rgb;
		break;
	}

	return hit;
}

bool IsSameObject(float a, float b)
{
	return abs(a - b) < 0.5f;
}

float3 ClampColorOffset(float3 offset)
{
	return clamp(offset, float3(-kMaxColorOffset, -kMaxColorOffset, -kMaxColorOffset), float3(kMaxColorOffset, kMaxColorOffset, kMaxColorOffset));
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
	float4 centerLabels = LabelTexture.Load(int3(pixCoord, 0));
	float centerObjectId = DecodeObjectId(centerLabels);
	bool centerIsSkin = IsTaggedSkin(centerLabels) && centerObjectId > 0.0f;

	RayHit left = TraceSkinRay(pixCoord, int2(-1, 0), radius, bufDim);
	RayHit right = TraceSkinRay(pixCoord, int2(1, 0), radius, bufDim);
	RayHit up = TraceSkinRay(pixCoord, int2(0, -1), radius, bufDim);
	RayHit down = TraceSkinRay(pixCoord, int2(0, 1), radius, bufDim);

	float3 corrected = sourceMain.rgb;
	bool shouldCorrect = false;
	float bestScore = 0.0f;

	if (centerIsSkin) {
		#define TRY_SKIN_PROJECTION(SAME_HIT, OPPOSING_HIT) \
		{ \
			if ((SAME_HIT).hit && (OPPOSING_HIT).hit && IsSameObject((SAME_HIT).objectId, centerObjectId) && !IsSameObject((OPPOSING_HIT).objectId, centerObjectId)) { \
				float distanceRange = max(LateSearchRadius - 1.0f, 1.0f); \
				float normalizedDistance = saturate(((OPPOSING_HIT).distance - 1.0f) / distanceRange); \
				float seamFalloff = 1.0f - smoothstep(0.0f, 1.0f, normalizedDistance); \
				float score = seamFalloff / max((SAME_HIT).distance + (OPPOSING_HIT).distance, 1.0f); \
				if (!shouldCorrect || score > bestScore) { \
					float3 offset = ClampColorOffset((OPPOSING_HIT).color - (SAME_HIT).color); \
					corrected = sourceMain.rgb + offset * seamFalloff; \
					shouldCorrect = true; \
					bestScore = score; \
				} \
			} \
		}

		TRY_SKIN_PROJECTION(left, right);
		TRY_SKIN_PROJECTION(right, left);
		TRY_SKIN_PROJECTION(up, down);
		TRY_SKIN_PROJECTION(down, up);

		#undef TRY_SKIN_PROJECTION
	} else {
		#define TRY_GAP_PROJECTION(HIT_A, HIT_B) \
		{ \
			if ((HIT_A).hit && (HIT_B).hit && !IsSameObject((HIT_A).objectId, (HIT_B).objectId)) { \
				float score = 1.0f / max((HIT_A).distance + (HIT_B).distance, 1.0f); \
				if (!shouldCorrect || score > bestScore) { \
					corrected = 0.5f * ((HIT_A).color + (HIT_B).color); \
					shouldCorrect = true; \
					bestScore = score; \
				} \
			} \
		}

		TRY_GAP_PROJECTION(left, right);
		TRY_GAP_PROJECTION(up, down);

		#undef TRY_GAP_PROJECTION
	}

	float3 finalColor = shouldCorrect ? lerp(sourceMain.rgb, corrected, lateBlendStrength) : sourceMain.rgb;
	MainOut[pixCoord] = float4(max(float3(0.0f, 0.0f, 0.0f), finalColor), sourceMain.a);
}
