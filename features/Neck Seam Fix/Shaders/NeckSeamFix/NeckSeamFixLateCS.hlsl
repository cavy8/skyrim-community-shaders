// =============================================================================
// NeckSeamFixLateCS.hlsl
//
// Post-composite seam color projection.
//
// Runs after DeferredCompositeCS. This pass reflects color response across the
// detected seam: each skin pixel looks through nearby same-object/gap pixels for
// the opposing skin object, samples an equal-distance point on its own side as a
// baseline, and transfers a clamped color offset that fades with seam distance.
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

RayHit TraceAnySkinRay(int2 pixCoord, int2 direction, int radius, uint2 bufDim)
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

RayHit TraceOpposingSkinRay(int2 pixCoord, int2 direction, int radius, uint2 bufDim, float centerObjectId)
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

		if (IsSameObject(objectId, centerObjectId))
			continue;

		hit.hit = true;
		hit.objectId = objectId;
		hit.distance = (float)step;
		hit.color = MainTexture.Load(int3(sampleCoord, 0)).rgb;
		break;
	}

	return hit;
}

RayHit SampleSameObjectAtDistance(int2 pixCoord, int2 direction, int distance, uint2 bufDim, float centerObjectId)
{
	RayHit hit = EmptyHit();

	// Prefer the exact reflected point, but tolerate one pixel of label jitter.
	for (int offset = 0; offset <= 1; ++offset)
	{
		int step = distance - offset;
		if (step < 1)
			continue;

		int2 sampleCoord = pixCoord + direction * step;
		if (any(sampleCoord < int2(0, 0)) || any(sampleCoord >= int2(bufDim)))
			continue;

		float rawDepth = DepthTexture.Load(int3(sampleCoord, 0)).x;
		if (!IsValidSceneDepth(rawDepth))
			continue;

		float4 labels = LabelTexture.Load(int3(sampleCoord, 0));
		float objectId = DecodeObjectId(labels);
		if (!IsTaggedSkin(labels) || !IsSameObject(objectId, centerObjectId))
			continue;

		hit.hit = true;
		hit.objectId = objectId;
		hit.distance = (float)step;
		hit.color = MainTexture.Load(int3(sampleCoord, 0)).rgb;
		break;
	}

	return hit;
}

float3 ClampColorOffset(float3 offset)
{
	return clamp(offset, float3(-kMaxColorOffset, -kMaxColorOffset, -kMaxColorOffset), float3(kMaxColorOffset, kMaxColorOffset, kMaxColorOffset));
}

float SeamDistanceFalloff(float distance, float radius)
{
	float distanceRange = max(radius - 1.0f, 1.0f);
	float normalizedDistance = saturate((distance - 1.0f) / distanceRange);
	return 1.0f - smoothstep(0.0f, 1.0f, normalizedDistance);
}

struct ProjectionCandidate
{
	bool valid;
	float score;
	float3 color;
};

ProjectionCandidate EmptyCandidate()
{
	ProjectionCandidate candidate;
	candidate.valid = false;
	candidate.score = 0.0f;
	candidate.color = 0.0f;
	return candidate;
}

ProjectionCandidate BuildReflectionCandidate(
	int2 pixCoord,
	int2 opposingDirection,
	int radius,
	uint2 bufDim,
	float centerObjectId,
	float3 sourceColor)
{
	ProjectionCandidate candidate = EmptyCandidate();
	RayHit opposing = TraceOpposingSkinRay(pixCoord, opposingDirection, radius, bufDim, centerObjectId);
	if (!opposing.hit)
		return candidate;

	RayHit sameMirror = SampleSameObjectAtDistance(pixCoord, -opposingDirection, (int)round(opposing.distance), bufDim, centerObjectId);
	float3 sameBaseline = sameMirror.hit ? sameMirror.color : sourceColor;
	float seamFalloff = SeamDistanceFalloff(opposing.distance, (float)radius);
	float3 offset = ClampColorOffset(opposing.color - sameBaseline);

	candidate.valid = true;
	candidate.score = seamFalloff / max(opposing.distance, 1.0f);
	candidate.color = sourceColor + offset * seamFalloff;
	return candidate;
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

	float3 corrected = sourceMain.rgb;
	bool shouldCorrect = false;
	float bestScore = 0.0f;

	if (centerIsSkin) {
		#define TRY_REFLECTION(DIRECTION) \
		{ \
			ProjectionCandidate candidate = BuildReflectionCandidate(pixCoord, DIRECTION, radius, bufDim, centerObjectId, sourceMain.rgb); \
			if (candidate.valid && (!shouldCorrect || candidate.score > bestScore)) { \
				corrected = candidate.color; \
				shouldCorrect = true; \
				bestScore = candidate.score; \
			} \
		}

		TRY_REFLECTION(int2(-1, 0));
		TRY_REFLECTION(int2(1, 0));
		TRY_REFLECTION(int2(0, -1));
		TRY_REFLECTION(int2(0, 1));

		#undef TRY_REFLECTION
	} else {
		RayHit left = TraceAnySkinRay(pixCoord, int2(-1, 0), radius, bufDim);
		RayHit right = TraceAnySkinRay(pixCoord, int2(1, 0), radius, bufDim);
		RayHit up = TraceAnySkinRay(pixCoord, int2(0, -1), radius, bufDim);
		RayHit down = TraceAnySkinRay(pixCoord, int2(0, 1), radius, bufDim);

		#define TRY_GAP_PROJECTION(HIT_A, HIT_B) \
		{ \
			if ((HIT_A).hit && (HIT_B).hit && !IsSameObject((HIT_A).objectId, (HIT_B).objectId)) { \
				float score = 1.0f / max((HIT_A).distance + (HIT_B).distance, 1.0f); \
				if (!shouldCorrect || score > bestScore) { \
					float sumDistance = max((HIT_A).distance + (HIT_B).distance, 1.0f); \
					corrected = ((HIT_A).color * (HIT_B).distance + (HIT_B).color * (HIT_A).distance) / sumDistance; \
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
