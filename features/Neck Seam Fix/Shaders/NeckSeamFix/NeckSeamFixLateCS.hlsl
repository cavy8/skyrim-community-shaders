// =============================================================================
// NeckSeamFixLateCS.hlsl
//
// Post-composite seam color projection.
//
// Runs after DeferredCompositeCS. This pass performs a one-way transfer from
// the head mesh onto nearby body skin in screen space. It finds the nearest
// head-labeled pixel around a body-side pixel, mirrors that vector into the
// body mesh for a baseline sample, and transfers a clamped color offset that
// fades out with distance from the seam.
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

bool IsHeadLabel(float4 labels)
{
	return labels.w > kLabelThreshold;
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

float SquaredLength(int2 v)
{
	return (float)(v.x * v.x + v.y * v.y);
}

bool IsSameObject(float a, float b)
{
	return abs(a - b) < 0.5f;
}

RayHit FindNearestHeadPixel(int2 pixCoord, int radius, uint2 bufDim, float excludedObjectId, out int2 hitOffset)
{
	RayHit hit = EmptyHit();
	float bestDistanceSq = 0.0f;
	hitOffset = int2(0, 0);

	for (int y = -radius; y <= radius; ++y)
	{
		for (int x = -radius; x <= radius; ++x)
		{
			int2 offset = int2(x, y);
			if (all(offset == int2(0, 0)))
				continue;

			float distanceSq = SquaredLength(offset);
			if (distanceSq > (float)(radius * radius))
				continue;

			int2 sampleCoord = pixCoord + offset;
			if (any(sampleCoord < int2(0, 0)) || any(sampleCoord >= int2(bufDim)))
				continue;

			float rawDepth = DepthTexture.Load(int3(sampleCoord, 0)).x;
			if (!IsValidSceneDepth(rawDepth))
				continue;

			float4 labels = LabelTexture.Load(int3(sampleCoord, 0));
			float objectId = DecodeObjectId(labels);
			if (!IsTaggedSkin(labels) || !IsHeadLabel(labels) || objectId <= 0.0f)
				continue;
			if (excludedObjectId > 0.0f && IsSameObject(objectId, excludedObjectId))
				continue;

			if (!hit.hit || distanceSq < bestDistanceSq) {
				hit.hit = true;
				hit.objectId = objectId;
				hit.distance = sqrt(distanceSq);
				hit.color = MainTexture.Load(int3(sampleCoord, 0)).rgb;
				bestDistanceSq = distanceSq;
				hitOffset = offset;
			}
		}
	}

	return hit;
}

RayHit FindNearestNonHeadSkinPixel(int2 pixCoord, int radius, uint2 bufDim, float excludedObjectId)
{
	RayHit hit = EmptyHit();
	float bestDistanceSq = 0.0f;

	for (int y = -radius; y <= radius; ++y)
	{
		for (int x = -radius; x <= radius; ++x)
		{
			int2 offset = int2(x, y);
			if (all(offset == int2(0, 0)))
				continue;

			float distanceSq = SquaredLength(offset);
			if (distanceSq > (float)(radius * radius))
				continue;

			int2 sampleCoord = pixCoord + offset;
			if (any(sampleCoord < int2(0, 0)) || any(sampleCoord >= int2(bufDim)))
				continue;

			float rawDepth = DepthTexture.Load(int3(sampleCoord, 0)).x;
			if (!IsValidSceneDepth(rawDepth))
				continue;

			float4 labels = LabelTexture.Load(int3(sampleCoord, 0));
			float objectId = DecodeObjectId(labels);
			if (!IsTaggedSkin(labels) || IsHeadLabel(labels) || objectId <= 0.0f)
				continue;
			if (excludedObjectId > 0.0f && IsSameObject(objectId, excludedObjectId))
				continue;

			if (!hit.hit || distanceSq < bestDistanceSq) {
				hit.hit = true;
				hit.objectId = objectId;
				hit.distance = sqrt(distanceSq);
				hit.color = MainTexture.Load(int3(sampleCoord, 0)).rgb;
				bestDistanceSq = distanceSq;
			}
		}
	}

	return hit;
}

RayHit SampleSameObjectNearCoord(int2 sampleCoord, uint2 bufDim, float centerObjectId)
{
	RayHit hit = EmptyHit();
	float bestDistanceSq = 0.0f;

	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			int2 candidateCoord = sampleCoord + int2(x, y);
			if (any(candidateCoord < int2(0, 0)) || any(candidateCoord >= int2(bufDim)))
				continue;

			float rawDepth = DepthTexture.Load(int3(candidateCoord, 0)).x;
			if (!IsValidSceneDepth(rawDepth))
				continue;

			float4 labels = LabelTexture.Load(int3(candidateCoord, 0));
			float objectId = DecodeObjectId(labels);
			if (!IsTaggedSkin(labels) || !IsSameObject(objectId, centerObjectId))
				continue;

			float distanceSq = SquaredLength(int2(x, y));
			if (!hit.hit || distanceSq < bestDistanceSq) {
				hit.hit = true;
				hit.objectId = objectId;
				hit.distance = sqrt(distanceSq);
				hit.color = MainTexture.Load(int3(candidateCoord, 0)).rgb;
				bestDistanceSq = distanceSq;
			}
		}
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

ProjectionCandidate BuildHeadToBodyCandidate(
	int2 pixCoord,
	int radius,
	uint2 bufDim,
	float centerObjectId,
	float3 sourceColor)
{
	ProjectionCandidate candidate = EmptyCandidate();
	int2 headOffset = int2(0, 0);
	RayHit head = FindNearestHeadPixel(pixCoord, radius, bufDim, centerObjectId, headOffset);
	if (!head.hit)
		return candidate;

	int2 mirroredCoord = pixCoord - headOffset;
	RayHit sameMirror = SampleSameObjectNearCoord(mirroredCoord, bufDim, centerObjectId);
	if (!sameMirror.hit)
		return candidate;

	float3 sameBaseline = sameMirror.hit ? sameMirror.color : sourceColor;
	float seamFalloff = SeamDistanceFalloff(head.distance, (float)radius);
	float3 offset = ClampColorOffset(head.color - sameBaseline);

	candidate.valid = true;
	candidate.score = seamFalloff / max(head.distance, 1.0f);
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
	bool centerIsHead = IsHeadLabel(centerLabels);

	float3 corrected = sourceMain.rgb;
	bool shouldCorrect = false;
	float bestScore = 0.0f;

	if (centerIsSkin && !centerIsHead) {
		ProjectionCandidate candidate = BuildHeadToBodyCandidate(pixCoord, radius, bufDim, centerObjectId, sourceMain.rgb);
		if (candidate.valid) {
			corrected = candidate.color;
			shouldCorrect = true;
			bestScore = candidate.score;
		}
	} else if (!centerIsHead) {
		int2 headOffset = int2(0, 0);
		RayHit head = FindNearestHeadPixel(pixCoord, radius, bufDim, 0.0f, headOffset);
		RayHit body = FindNearestNonHeadSkinPixel(pixCoord, radius, bufDim, head.objectId);
		if (head.hit && body.hit && !IsSameObject(head.objectId, body.objectId)) {
			float seamFalloff = SeamDistanceFalloff(head.distance, (float)radius);
			float score = seamFalloff / max(head.distance + body.distance, 1.0f);
			float3 offset = ClampColorOffset(head.color - body.color);
			corrected = sourceMain.rgb + offset * seamFalloff;
			shouldCorrect = true;
			bestScore = score;
		}
	}

	float3 finalColor = shouldCorrect ? lerp(sourceMain.rgb, corrected, lateBlendStrength) : sourceMain.rgb;
	MainOut[pixCoord] = float4(max(float3(0.0f, 0.0f, 0.0f), finalColor), sourceMain.a);
}
