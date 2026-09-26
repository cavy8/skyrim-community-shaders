#ifndef EFFECTS11_SKY_SCATTERING_HLSLI
#define EFFECTS11_SKY_SCATTERING_HLSLI

#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#if defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

namespace SkyScattering
{
	static const float CloudLayerHeight = 2e3 / GAME_UNIT_TO_M;
	static const float PlanetRadius = 6371e3 / GAME_UNIT_TO_M;
	static const float CloudSelfShadowArc = 0.2;
	static const float MaxCloudScattering = 32.0;

	struct Light
	{
		float3 direction;
		float3 color;
		float weight;
	};

	float3 SafeNormalize(float3 v)
	{
		return v * rsqrt(max(dot(v, v), 1e-8));
	}

	float HorizonFade(float z)
	{
		return smoothstep(-0.1, 0.02, z);
	}

	float3 GetChroma(float3 color)
	{
		return max(color, 0.0) / max(max(color.r, max(color.g, color.b)), 1e-4);
	}

	float PhaseHG(float cosTheta, float g)
	{
		float g2 = g * g;
		float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
		return (1.0 - g2) / (denom * sqrt(denom));
	}

	float PhaseHGPeak(float cosTheta, float g)
	{
		float x = saturate((1.0 - g) * (1.0 - g) / max(1.0 + g * g - 2.0 * g * cosTheta, 1e-6));
		return x * sqrt(x);
	}

	float GetSunWeight()
	{
		return saturate(SharedData::SunColor.w) * HorizonFade(SharedData::SunDirection.z);
	}

	Light GetLight()
	{
		Light light;
		if (SharedData::SunDirection.z > -0.1) {
			light.direction = SafeNormalize(SharedData::SunDirection.xyz);
			light.color = lerp(1.0.xxx, GetChroma(SharedData::SunColor.xyz), SharedData::enbSettings.SkyScatteringColorFromSun);
			light.weight = GetSunWeight();
		} else {
			float masser = dot(max(SharedData::MasserColor.xyz, 0.0), 1.0 / 3.0) * HorizonFade(SharedData::MasserDirection.z);
			float secunda = dot(max(SharedData::SecundaColor.xyz, 0.0), 1.0 / 3.0) * HorizonFade(SharedData::SecundaDirection.z);
			bool useMasser = masser >= secunda;
			float3 moonDirection = useMasser ? SharedData::MasserDirection.xyz : SharedData::SecundaDirection.xyz;
			float3 moonColor = max(useMasser ? SharedData::MasserColor.xyz : SharedData::SecundaColor.xyz, 0.0);
			light.direction = SafeNormalize(moonDirection);
			light.color = lerp(dot(moonColor, 1.0 / 3.0).xxx, moonColor, SharedData::enbSettings.SkyScatteringColorFromSun);
			light.weight = (1.0 - smoothstep(-0.2, -0.1, SharedData::SunDirection.z)) * HorizonFade(light.direction.z) * SharedData::enbSettings.SkyScatteringMoonGlowAmount;
		}
		light.color *= SharedData::enbSettings.SkyScatteringColor * SharedData::enbSettings.SkyScatteringIntensity;
		return light;
	}

	float GetCloudLayerDistance(float3 viewDirection)
	{
		float b = PlanetRadius * viewDirection.z;
		float c = CloudLayerHeight * (2.0 * PlanetRadius + CloudLayerHeight);
		float root = sqrt(b * b + c);
		return b >= 0.0 ? c / (b + root) : root - b;
	}

	float GetRayLength(float3 viewDirection, float depth, float3 positionMS)
	{
		float cloudDistance = GetCloudLayerDistance(viewDirection);
		return depth < 1.0 ? min(length(positionMS), cloudDistance) : cloudDistance;
	}

	float GetOpticalDepth(float distance, float viewZ)
	{
		float extinction = SharedData::enbSettings.SkyScatteringExtinction;
		float k = max(viewZ, 0.0) / SharedData::enbSettings.SkyScatteringScaleHeight;
		float x = k * distance;
		return x < 1e-3 ? extinction * distance * (1.0 - 0.5 * x) : extinction * (1.0 - exp(-x)) / k;
	}

	float GetDistanceAtOpticalDepth(float opticalDepth, float viewZ)
	{
		float extinction = max(SharedData::enbSettings.SkyScatteringExtinction, 1e-20);
		float k = max(viewZ, 0.0) / SharedData::enbSettings.SkyScatteringScaleHeight;
		float y = opticalDepth * k / extinction;
		return y < 1e-3 ? opticalDepth / extinction * (1.0 + 0.5 * y) : -log(max(1.0 - y, 1e-6)) / k;
	}

	float GetInscatterAmount(float rayLength, float viewZ)
	{
		return 1.0 - exp(-GetOpticalDepth(rayLength, viewZ));
	}

	float GetOpticalDepthFromAlpha(float alpha)
	{
		return -log(1.0 - clamp(alpha, 0.0, 0.98));
	}

#if defined(CLOUD_SHADOWS)
	float GetCloudTransmittance(float3 samplePosition, float3 lightDirection, SamplerState textureSampler)
	{
		float3 cloudDirection = CloudShadows::GetCloudShadowSampleDir(samplePosition, lightDirection);
		float occlusion = CloudShadows::CloudShadowsTexture.SampleLevel(textureSampler, cloudDirection, 0);
		return 1.0 - sqrt(saturate(occlusion));
	}

	float GetCloudOpticalDepthToLight(float3 viewDirection, float3 lightDirection, float noise, SamplerState textureSampler, out float selfOpticalDepth)
	{
		static const uint sampleCount = 6;
		float opticalDepth = 0.0;
		selfOpticalDepth = 0.0;
		[unroll] for (uint i = 0; i < sampleCount; i++)
		{
			float t = (float(i) + noise) / float(sampleCount);
			float3 sampleDirection = SafeNormalize(lerp(viewDirection, lightDirection, t * t * CloudSelfShadowArc));
			float occlusion = CloudShadows::CloudShadowsTexture.SampleLevel(textureSampler, sampleDirection, 0);
			float sampleOpticalDepth = GetOpticalDepthFromAlpha(sqrt(saturate(occlusion)));
			if (i == 0)
				selfOpticalDepth = sampleOpticalDepth;
			opticalDepth += sampleOpticalDepth;
		}
		return opticalDepth / float(sampleCount);
	}
#endif

	float GetMultipleScatteringTransmittance(float opticalDepth)
	{
		return (exp(-opticalDepth) + 0.5 * exp(-0.5 * opticalDepth) + 0.25 * exp(-0.25 * opticalDepth)) / 1.75;
	}

	float GetCloudScattering(float cosTheta, float opticalDepthToLight, float opticalDepthView)
	{
		static const float ForwardG = 0.75;
		static const float BackwardG = -0.2;
		static const float ForwardWeight = 0.7;

		float scattering = 0.0;
		float octaveWeight = 1.0;
		float octaveScale = 1.0;

		[unroll] for (uint octave = 0; octave < 3; octave++)
		{
			float lightTransmittance = exp(-opticalDepthToLight * octaveScale);
			float viewDepth = opticalDepthView * octaveScale;
			float viewTransmittance = exp(-viewDepth);
			float forward = viewDepth > 1e-3 ? viewDepth * viewTransmittance / (1.0 - viewTransmittance) : 1.0;
			float backward = 0.5 * (1.0 + viewTransmittance);
			float phase = ForwardWeight * PhaseHG(cosTheta, ForwardG * octaveScale) * forward +
			              (1.0 - ForwardWeight) * PhaseHG(cosTheta, BackwardG * octaveScale) * backward;

			scattering += octaveWeight * lightTransmittance * phase;

			octaveWeight *= 0.5;
			octaveScale *= 0.5;
		}

		return scattering;
	}

	float3 GetCelestialCloudLighting(float3 viewDirection, float3 lightDirection, float3 lightColor, float opticalDepthView, float noise, SamplerState textureSampler, out float lightTransmittance, out float shadowTransmittance)
	{
		float opticalDepthToLight = 0.0;
		float selfOpticalDepth = 0.0;
#if defined(CLOUD_SHADOWS)
		opticalDepthToLight = GetCloudOpticalDepthToLight(viewDirection, lightDirection, noise, textureSampler, selfOpticalDepth);
#endif
		float density = SharedData::enbSettings.CloudsLightingDensity;
		lightTransmittance = exp(-opticalDepthToLight * density);
		shadowTransmittance = GetMultipleScatteringTransmittance(max(opticalDepthToLight - selfOpticalDepth, 0.0) * density);
		return lightColor * GetCloudScattering(dot(viewDirection, lightDirection), opticalDepthToLight * density, opticalDepthView);
	}

	float3 RelightCloud(float3 cloudColor, float cloudLuminance, float alpha, float3 viewDirection, float2 screenPosition, SamplerState textureSampler, out float3 edgeTransmittance)
	{
		edgeTransmittance = 0.0;
		if (alpha < 1e-3)
			return cloudColor;

		float opticalDepthView = GetOpticalDepthFromAlpha(alpha) * SharedData::enbSettings.CloudsLightingDensity;
		float noise = Random::InterleavedGradientNoise(screenPosition);
		float3 lighting = 0.0;
		float shade = 1.0;

		float sunWeight = GetSunWeight();
		[branch] if (sunWeight > 0.0)
		{
			float3 sunColor = GetChroma(SharedData::SunColor.xyz) * (sunWeight * SharedData::enbSettings.CloudsLightingSunMultiplier);
			float shadowTransmittance;
			lighting += GetCelestialCloudLighting(viewDirection, SafeNormalize(SharedData::SunDirection.xyz), sunColor, opticalDepthView, noise, textureSampler, edgeTransmittance.x, shadowTransmittance);
			shade = lerp(1.0, lerp(SharedData::enbSettings.CloudsLightingSunMinIntensity, 1.0, shadowTransmittance), sunWeight);
		}

		[branch] if (SharedData::enbSettings.EnableCloudsLightingFromMoon && SharedData::enbSettings.CloudsLightingMoonIntensity > 0.0)
		{
			float unused;
			float masserWeight = HorizonFade(SharedData::MasserDirection.z) * (1.0 - sunWeight);
			[branch] if (masserWeight > 0.0 && any(SharedData::MasserColor.xyz > 0.0))
			{
				float3 masserColor = max(SharedData::MasserColor.xyz, 0.0) * (masserWeight * SharedData::enbSettings.CloudsLightingMoonIntensity);
				lighting += GetCelestialCloudLighting(viewDirection, SafeNormalize(SharedData::MasserDirection.xyz), masserColor, opticalDepthView, noise, textureSampler, edgeTransmittance.y, unused);
			}

			float secundaWeight = HorizonFade(SharedData::SecundaDirection.z) * (1.0 - sunWeight);
			[branch] if (secundaWeight > 0.0 && any(SharedData::SecundaColor.xyz > 0.0))
			{
				float3 secundaColor = max(SharedData::SecundaColor.xyz, 0.0) * (secundaWeight * SharedData::enbSettings.CloudsLightingMoonIntensity);
				lighting += GetCelestialCloudLighting(viewDirection, SafeNormalize(SharedData::SecundaDirection.xyz), secundaColor, opticalDepthView, noise, textureSampler, edgeTransmittance.z, unused);
			}
		}

		return cloudColor * shade + cloudLuminance * min(lighting, MaxCloudScattering);
	}
}

#endif
