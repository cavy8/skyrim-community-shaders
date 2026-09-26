#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#define LinearSampler defaultSampler
SamplerState defaultSampler : register(s0);

#include "Common/ShadowSampling.hlsli"
#include "Effects11/SkyScattering.hlsli"

struct VS_OUTPUT_POST
{
	float4 pos : SV_POSITION;
	float2 txcoord0 : TEXCOORD0;
};

struct PS_OUTPUT
{
	float Scattering : SV_Target0;
	// Depth this texel raymarched with; the bilateral blur + upsample weight against it.
	float Depth : SV_Target1;
	float SkyLitFraction : SV_Target2;
};

float GetVolumetricRaysScattering(float3 positionMS, float noise, float3 cameraOffset)
{
	float extinction = SharedData::enbSettings.VolumetricRaysExtinction;
	float totalRayLength = length(positionMS);

	const uint sampleCount = 16;
	const float rcpSampleCount = 1.0 / float(sampleCount);
	float negExtTimesRayLen = -extinction * totalRayLength;

	float scattering = 0.0;
	float transmittance = 1.0;

	[unroll]
	for (uint i = 0; i < sampleCount; i++) {
		float t0 = float(i) * rcpSampleCount;
		float t1 = float(i + 1) * rcpSampleCount;

		t0 *= t0;
		t1 *= t1;

		float t = lerp(t0, t1, noise);
		float stepDelta = t1 - t0;

		float3 samplePos = positionMS * t;

		float shadow = 1.0;

#if defined(TERRAIN_SHADOWS)
		shadow = TerrainShadows::GetTerrainShadow(samplePos + cameraOffset, LinearSampler);
#endif

#if defined(CLOUD_SHADOWS)
		shadow *= CloudShadows::GetCloudShadowMult(samplePos, LinearSampler);
#endif
		shadow *= shadow;

		float stepTransmittance = exp(negExtTimesRayLen * stepDelta);
		scattering += shadow * (1.0 - stepTransmittance) * transmittance;
		transmittance *= stepTransmittance;
	}

	return scattering;
}

float GetSkyLitFraction(float3 positionMS, float depth, float noise, float3 cameraOffset)
{
	SkyScattering::Light light = SkyScattering::GetLight();
	if (light.weight <= 0.0)
		return 1.0;

	float3 viewDirection = SkyScattering::SafeNormalize(positionMS);
	float rayLength = SkyScattering::GetRayLength(viewDirection, depth, positionMS);
	float inscatterAmount = SkyScattering::GetInscatterAmount(rayLength, viewDirection.z);
	if (inscatterAmount < 1e-4)
		return 1.0;

	const uint sampleCount = 16;
	float lit = 0.0;

	[unroll]
	for (uint i = 0; i < sampleCount; i++) {
		float inscatterFraction = (float(i) + noise) / float(sampleCount) * inscatterAmount;
		float distance = SkyScattering::GetDistanceAtOpticalDepth(-log(1.0 - inscatterFraction), viewDirection.z);
		float3 samplePos = viewDirection * distance;

		float shadow = 1.0;
#if defined(TERRAIN_SHADOWS)
		shadow = TerrainShadows::GetTerrainShadow(samplePos + cameraOffset, LinearSampler);
#endif
#if defined(CLOUD_SHADOWS)
		shadow *= SkyScattering::GetCloudTransmittance(samplePos, light.direction, LinearSampler);
#endif
		lit += shadow;
	}

	return lit / float(sampleCount);
}

PS_OUTPUT main(VS_OUTPUT_POST input)
{
	float2 uv = input.txcoord0;

	float depth = SharedData::GetDepth(uv);
	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	float4 positionMS = mul(FrameBuffer::CameraViewProjInverse, positionCS);
	positionMS.xyz /= positionMS.w;

	float noise = Random::InterleavedGradientNoise(input.pos.xy, SharedData::FrameCount);
	float3 cameraOffset = FrameBuffer::CameraPosAdjust.xyz;

	PS_OUTPUT output;
	output.Scattering = 0.0;
	output.Depth = depth;
	output.SkyLitFraction = 1.0;

	[branch] if (SharedData::enbSettings.EnableVolumetricRays)
		output.Scattering = GetVolumetricRaysScattering(positionMS.xyz, noise, cameraOffset);

	[branch] if (SharedData::enbSettings.EnableCloudsScattering)
		output.SkyLitFraction = GetSkyLitFraction(positionMS.xyz, depth, noise, cameraOffset);

	return output;
}
