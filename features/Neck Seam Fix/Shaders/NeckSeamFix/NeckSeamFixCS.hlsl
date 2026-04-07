// =============================================================================
// NeckSeamFixCS.hlsl
//
// Skin-to-skin seam reconstruction.
//
// The deferred lighting pass tags visible skin pixels in a dedicated label
// target. This pass then looks for opposing skin support around each pixel,
// fills narrow gaps between nearby skin surfaces, and feathers seam-edge
// pixels toward the reconstructed shared surface.
// =============================================================================

#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> MaskTexture : register(t1);
Texture2D<float4> LabelTexture : register(t2);
Texture2D<float4> MainTexture : register(t3);
Texture2D<float4> AlbedoTexture : register(t4);
Texture2D<float4> NormalRoughnessTexture : register(t5);
Texture2D<float4> SpecularTexture : register(t6);
Texture2D<float4> ReflectanceTexture : register(t7);

RWTexture2D<float4> MainOut : register(u0);
RWTexture2D<unorm float4> AlbedoOut : register(u1);
RWTexture2D<unorm float4> NormalRoughnessOut : register(u2);
RWTexture2D<float4> MaskOut : register(u3);
RWTexture2D<float> DepthOut : register(u4);
RWTexture2D<unorm half> DepthOut16 : register(u5);
RWTexture2D<float4> SpecularOut : register(u6);
RWTexture2D<unorm float4> ReflectanceOut : register(u7);

cbuffer NeckSeamCB : register(b1)
{
	float SearchRadius;
	float DepthThreshold;
	float BlendStrength;
	float pad;
};

static const float kLabelThreshold = 0.5f;
static const float kSeamSignalFloor = 0.15f;
static const float kNormalSignalScale = 0.5f;

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

struct Accumulator
{
	float weight;
	float rawDepth;
	float glossiness;
	float objectId;
	float3 mainColor;
	float4 albedo;
	float4 specular;
	float4 reflectance;
	float4 mask;
	float3 normal;
};

void AddSample(
	inout Accumulator accum,
	float weight,
	float rawDepth,
	float objectId,
	float3 mainColor,
	float4 albedo,
	float4 normalRoughness,
	float4 specular,
	float4 reflectance,
	float4 mask)
{
	accum.weight += weight;
	accum.rawDepth += rawDepth * weight;
	accum.glossiness += normalRoughness.z * weight;
	accum.objectId += objectId * weight;
	accum.mainColor += mainColor * weight;
	accum.albedo += albedo * weight;
	accum.specular += specular * weight;
	accum.reflectance += reflectance * weight;
	accum.mask += mask * weight;
	accum.normal += GBuffer::DecodeNormal(normalRoughness.xy) * weight;
}

Accumulator CombineAccum(Accumulator a, Accumulator b)
{
	Accumulator combined = (Accumulator)0;
	combined.weight = a.weight + b.weight;
	combined.rawDepth = a.rawDepth + b.rawDepth;
	combined.glossiness = a.glossiness + b.glossiness;
	combined.objectId = a.objectId + b.objectId;
	combined.mainColor = a.mainColor + b.mainColor;
	combined.albedo = a.albedo + b.albedo;
	combined.specular = a.specular + b.specular;
	combined.reflectance = a.reflectance + b.reflectance;
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

float4 AverageSpecular(Accumulator accum)
{
	return accum.specular / max(accum.weight, 1e-5f);
}

float4 AverageReflectance(Accumulator accum)
{
	return accum.reflectance / max(accum.weight, 1e-5f);
}

float4 AverageMask(Accumulator accum)
{
	return accum.mask / max(accum.weight, 1e-5f);
}

float AverageRawDepth(Accumulator accum)
{
	return accum.rawDepth / max(accum.weight, 1e-5f);
}

float AverageGlossiness(Accumulator accum)
{
	return accum.glossiness / max(accum.weight, 1e-5f);
}

float AverageObjectId(Accumulator accum)
{
	return floor(accum.objectId / max(accum.weight, 1e-5f) + 0.5f);
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

	float4 sourceMain = MainTexture.Load(int3(pixCoord, 0));
	float4 sourceAlbedo = AlbedoTexture.Load(int3(pixCoord, 0));
	float4 sourceNormalRoughness = NormalRoughnessTexture.Load(int3(pixCoord, 0));
	float4 sourceSpecular = SpecularTexture.Load(int3(pixCoord, 0));
	float4 sourceReflectance = ReflectanceTexture.Load(int3(pixCoord, 0));
	float4 sourceMask = MaskTexture.Load(int3(pixCoord, 0));
	float4 sourceLabels = LabelTexture.Load(int3(pixCoord, 0));

	float centerObjectId = DecodeObjectId(sourceLabels);
	bool centerIsTaggedSkin = IsTaggedSkin(sourceLabels) && centerObjectId > 0.0f;
	float3 centerNormal = centerHasGeometry ? GBuffer::DecodeNormal(sourceNormalRoughness.xy) : float3(0.0f, 0.0f, 1.0f);

	Accumulator left = (Accumulator)0;
	Accumulator right = (Accumulator)0;
	Accumulator up = (Accumulator)0;
	Accumulator down = (Accumulator)0;

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
			float4 neighbourLabels = LabelTexture.Load(int3(sampleCoord, 0));
			float neighbourObjectId = DecodeObjectId(neighbourLabels);
			bool neighbourIsTaggedSkin = IsTaggedSkin(neighbourLabels) && neighbourObjectId > 0.0f;

			if (!neighbourIsTaggedSkin || !neighbourHasGeometry)
				continue;

			float4 neighbourMain = MainTexture.Load(int3(sampleCoord, 0));
			float4 neighbourAlbedo = AlbedoTexture.Load(int3(sampleCoord, 0));
			float4 neighbourNormalRoughness = NormalRoughnessTexture.Load(int3(sampleCoord, 0));
			float4 neighbourSpecular = SpecularTexture.Load(int3(sampleCoord, 0));
			float4 neighbourReflectance = ReflectanceTexture.Load(int3(sampleCoord, 0));

			float dist = length(float2(dx, dy));
			float weight = 1.0f / max(dist, 0.001f);

			if (abs(dx) >= abs(dy)) {
				if (dx < 0)
					AddSample(left, weight, rawNeighbourDepth, neighbourObjectId, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourSpecular, neighbourReflectance, neighbourMask);
				else
					AddSample(right, weight, rawNeighbourDepth, neighbourObjectId, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourSpecular, neighbourReflectance, neighbourMask);
			}

			if (abs(dy) >= abs(dx)) {
				if (dy < 0)
					AddSample(up, weight, rawNeighbourDepth, neighbourObjectId, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourSpecular, neighbourReflectance, neighbourMask);
				else
					AddSample(down, weight, rawNeighbourDepth, neighbourObjectId, neighbourMain.rgb, neighbourAlbedo, neighbourNormalRoughness, neighbourSpecular, neighbourReflectance, neighbourMask);
			}
		}
	}

	Accumulator sideA = (Accumulator)0;
	Accumulator sideB = (Accumulator)0;
	bool seamAxisFound = false;
	float bestAxisWeight = 0.0f;

	#define TRY_AXIS(ACCUM_A, ACCUM_B) \
	{ \
		if ((ACCUM_A).weight > 0.0f && (ACCUM_B).weight > 0.0f) { \
			float objectIdA = AverageObjectId(ACCUM_A); \
			float objectIdB = AverageObjectId(ACCUM_B); \
			bool axisHasObjectOverlap = objectIdA > 0.0f && objectIdB > 0.0f && (centerIsTaggedSkin ? (objectIdA != centerObjectId || objectIdB != centerObjectId) : (objectIdA != objectIdB)); \
			if (axisHasObjectOverlap) { \
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

	float4 outMain = sourceMain;
	float4 outAlbedo = sourceAlbedo;
	float4 outSpecular = sourceSpecular;
	float4 outReflectance = sourceReflectance;
	float4 outMask = sourceMask;
	float4 outNormalRoughness = sourceNormalRoughness;
	float outRawDepth = rawCenterDepth;

	if (blendStrength > 0.0f && seamAxisFound) {
		Accumulator seamAccum = CombineAccum(sideA, sideB);
		float3 seamMain = AverageMain(seamAccum);
		float4 seamAlbedo = AverageAlbedo(seamAccum);
		float4 seamSpecular = AverageSpecular(seamAccum);
		float4 seamReflectance = AverageReflectance(seamAccum);
		float4 seamMask = AverageMask(seamAccum);
		float seamRawDepth = min(AverageRawDepth(sideA), AverageRawDepth(sideB));
		float seamGlossiness = 0.5f * (AverageGlossiness(sideA) + AverageGlossiness(sideB));

		float3 sideAMain = AverageMain(sideA);
		float3 sideBMain = AverageMain(sideB);
		float4 sideAAlbedo = AverageAlbedo(sideA);
		float4 sideBAlbedo = AverageAlbedo(sideB);
		float3 sideANormal = AverageNormal(sideA, centerNormal);
		float3 sideBNormal = AverageNormal(sideB, centerNormal);
		float3 seamNormal = normalize(sideANormal + sideBNormal);

		float colorSignal = max(length(sideAAlbedo.xyz - sideBAlbedo.xyz), length(sideAMain - sideBMain));
		float normalSignal = 1.0f - saturate(dot(sideANormal, sideBNormal));
		float seamSignal = max(colorSignal, normalSignal * kNormalSignalScale);
		float seamBlend = max(kSeamSignalFloor, seamSignal);

		float2 encodedSeamNormal = GBuffer::EncodeNormal(seamNormal);
		float4 seamNormalRoughness = float4(encodedSeamNormal, seamGlossiness, sourceNormalRoughness.w);

		if (!centerIsTaggedSkin) {
			float fillBlend = saturate(blendStrength * max(0.6f, seamBlend));
			outMain = float4(lerp(sourceMain.rgb, seamMain, fillBlend), sourceMain.a);
			outAlbedo = float4(lerp(sourceAlbedo.rgb, seamAlbedo.rgb, fillBlend), sourceAlbedo.a);
			outSpecular = float4(lerp(sourceSpecular.rgb, seamSpecular.rgb, fillBlend), sourceSpecular.a);
			outReflectance = lerp(sourceReflectance, seamReflectance, fillBlend);
			outMask = float4(lerp(sourceMask.rgb, seamMask.rgb, fillBlend), sourceMask.a);
			outNormalRoughness = float4(lerp(sourceNormalRoughness.xyz, seamNormalRoughness.xyz, fillBlend), sourceNormalRoughness.w);
			outRawDepth = seamRawDepth;
		} else if (centerIsTaggedSkin) {
			float edgeBlend = saturate(blendStrength * max(0.35f, seamBlend));

			outMain = float4(lerp(sourceMain.rgb, seamMain, edgeBlend), sourceMain.a);
			outAlbedo = float4(lerp(sourceAlbedo.rgb, seamAlbedo.rgb, edgeBlend), sourceAlbedo.a);
			outSpecular = float4(lerp(sourceSpecular.rgb, seamSpecular.rgb, edgeBlend), sourceSpecular.a);
			outReflectance = lerp(sourceReflectance, seamReflectance, edgeBlend);
			outMask = float4(lerp(sourceMask.rgb, seamMask.rgb, edgeBlend), sourceMask.a);
			outNormalRoughness = float4(lerp(sourceNormalRoughness.xyz, seamNormalRoughness.xyz, edgeBlend), sourceNormalRoughness.w);
			outRawDepth = lerp(rawCenterDepth, seamRawDepth, edgeBlend * 0.35f);
		}
	}

	MainOut[pixCoord] = outMain;
	AlbedoOut[pixCoord] = outAlbedo;
	NormalRoughnessOut[pixCoord] = outNormalRoughness;
	MaskOut[pixCoord] = outMask;
	DepthOut[pixCoord] = outRawDepth;
	DepthOut16[pixCoord] = outRawDepth;
	SpecularOut[pixCoord] = outSpecular;
	ReflectanceOut[pixCoord] = outReflectance;
}
