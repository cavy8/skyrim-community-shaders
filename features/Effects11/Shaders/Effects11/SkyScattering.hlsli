#ifndef EFFECTS11_SKY_SCATTERING_HLSLI
#define EFFECTS11_SKY_SCATTERING_HLSLI

#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

#if defined(CLOUDS) && defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

namespace SkyScattering
{
	static const float PlanetRadius = 6371e3 / GAME_UNIT_TO_M;
	static const float RayleighScaleHeight = 8e3 / GAME_UNIT_TO_M;
	static const float AerosolScaleHeight = 1.2e3 / GAME_UNIT_TO_M;
	static const float3 RayleighExtinction = float3(6.6049e-6, 12.345e-6, 29.413e-6) * GAME_UNIT_TO_M;
	static const float AerosolExtinction = 4.44e-5 * GAME_UNIT_TO_M;

	float3 SafeNormalize(float3 v)
	{
		return v * rsqrt(max(dot(v, v), 1e-8));
	}

	float GetRelativeAirmass(float cosZenith, float scaleHeight)
	{
		float x = PlanetRadius / scaleHeight;
		float y = sqrt(0.5 * x) * max(cosZenith, 0.0);
		return sqrt(2.0 * x) / (y + sqrt(y * y + 4.0 / Math::PI));
	}

	float3 GetCelestialTransmittance(float3 viewDirection)
	{
		float rayleighExcess = GetRelativeAirmass(viewDirection.z, RayleighScaleHeight) - GetRelativeAirmass(1.0, RayleighScaleHeight);
		float aerosolExcess = GetRelativeAirmass(viewDirection.z, AerosolScaleHeight) - GetRelativeAirmass(1.0, AerosolScaleHeight);
		float3 opticalDepth = RayleighExtinction * (RayleighScaleHeight * rayleighExcess) + AerosolExtinction * AerosolScaleHeight * aerosolExcess;
		return exp(-max(opticalDepth, 0.0));
	}

	float Pow32(float x)
	{
		x *= x;
		x *= x;
		x *= x;
		x *= x;
		return x * x;
	}

	float3 Pow32(float3 x)
	{
		x *= x;
		x *= x;
		x *= x;
		x *= x;
		return x * x;
	}

	float3 GetSunDirection()
	{
		return SharedData::enbSettings.SkyScatteringSunDirection;
	}

	float GetFacing(float3 viewDirection, float3 lightDirection)
	{
		return saturate(dot(viewDirection, lightDirection) * 0.5 + 0.5);
	}

	float GetElevation(float3 viewDirection, float sunHeight)
	{
		return saturate(1.0 - saturate(1.0 - viewDirection.z) * saturate(1.0 + sunHeight));
	}

	float GetHorizonCrop(float viewHeight)
	{
		float below = saturate(1.0 - saturate(viewHeight) * 40.0);
		return saturate(1.0 - below * below);
	}

	float GetDustBand(float elevation)
	{
		return Pow32(saturate(1.0 - elevation * SharedData::enbSettings.SkyScatteringDustVolume));
	}

	float GetEarthShadow(float elevation, float facing, float sunHeight)
	{
		float lightHeight = -sunHeight;
		float threshold = lerp(0.8 + 0.3 * saturate((lightHeight + 0.1) * 5.0), 0.95 + saturate(lightHeight * 0.5), facing);
		float shadow = saturate(1.0 + elevation - threshold);
		float spread = saturate(1.0 - (lightHeight + 0.1) * 3.0);
		spread = 4.0 * (0.1 + spread * spread);
		shadow = saturate(shadow * spread * (1.0 + facing * 4.0));
		shadow = shadow * shadow * (3.0 - 2.0 * shadow);
		return shadow * shadow * (3.0 - 2.0 * shadow);
	}

	float3 GetScatteringColor(float dustBand, bool horizonTerms, float aboveHorizon)
	{
		float3 color = SharedData::enbSettings.SkyScatteringColor * Pow32(saturate(1.0 - dustBand * SharedData::enbSettings.SkyScatteringDustTint));
		float intensity = SharedData::enbSettings.SkyScatteringIntensity;
		if (horizonTerms) {
			intensity *= aboveHorizon;
			color *= 1.0 - dustBand * SharedData::enbSettings.SkyScatteringDustDarkening;
		}
		return color * intensity;
	}

	float GetScatteringFade(float elevation, float facing)
	{
		float away = 1.0 - facing;
		float spread = lerp(SharedData::enbSettings.SkyScatteringHorizonRange * away * away, SharedData::enbSettings.SkyScatteringAtmosphereThickness, elevation * elevation);
		return 1.0 / (1.0 + away * spread);
	}

	float3 GetScatteringColorAt(float3 direction)
	{
		float3 sunDirection = GetSunDirection();
		float dustBand = GetDustBand(GetElevation(direction, sunDirection.z));
		return GetScatteringColor(dustBand, false, 1.0) * saturate(SharedData::enbSettings.SkyScatteringAmount);
	}

	float3 ApplySkyScattering(float3 skyColor, float3 topColor, float3 viewDirection)
	{
		float3 sunDirection = GetSunDirection();
		float facing = GetFacing(viewDirection, sunDirection);
		float away = 1.0 - facing;
		float elevation = GetElevation(viewDirection, sunDirection.z);

		float below = saturate(-viewDirection.z);
		float aboveHorizon = saturate(1.0 - below * 10.0);
		aboveHorizon *= aboveHorizon;
		float sunAboveHorizon = saturate(1.0 - below * 40.0);
		sunAboveHorizon *= sunAboveHorizon;

		float shadow = lerp(1.0, GetEarthShadow(elevation, facing, sunDirection.z), SharedData::enbSettings.SkyScatteringShadowAmount);
		float3 scatteringColor = GetScatteringColor(GetDustBand(elevation), true, aboveHorizon);

		float3 result = skyColor;
		result += result * (SharedData::enbSettings.SkyScatteringAirGlowIntensity / (1.0 + away * SharedData::enbSettings.SkyScatteringAirGlowRange));
		result = lerp(result, scatteringColor, saturate(GetScatteringFade(elevation, facing) * SharedData::enbSettings.SkyScatteringAmount));
		result += result * (SharedData::enbSettings.SkyScatteringSunGlowIntensity * sunAboveHorizon / (1.0 + away * SharedData::enbSettings.SkyScatteringSunGlowRange));
		result = lerp(topColor * 0.5, result, shadow);
		return lerp(skyColor, result, aboveHorizon);
	}

	float GetBillboardRadius(float3 viewDirection, float3 centerDirection, float halfTan)
	{
		float cosAngle = dot(viewDirection, centerDirection);
		if (cosAngle <= 1e-3 || halfTan <= 0.0)
			return 2.0;
		return (1.0 - cosAngle * cosAngle) / (cosAngle * cosAngle * halfTan * halfTan);
	}

	float3 GetMoonGlow(float3 viewDirection, float3 moonDirection, float3 moonColor, float moonHalfTan)
	{
		float radius = GetBillboardRadius(viewDirection, SafeNormalize(moonDirection), 12.0 * moonHalfTan);
		if (radius >= 1.0)
			return 0.0;
		float glow = saturate(1.0 / (1.0 + radius * SharedData::enbSettings.SkyScatteringMoonGlowRange) - 0.005) * (1.0 - radius);
		return max(moonColor, 0.0) * (glow * SharedData::enbSettings.CloudsEdgeMoonMultiplier);
	}

	float3 GetMoonGlow(float3 viewDirection)
	{
		[branch] if (SharedData::enbSettings.SkyScatteringMoonGlowAmount <= 0.0) return 0.0;

		float3 glow = GetMoonGlow(viewDirection, SharedData::MasserDirection.xyz, SharedData::MasserColor.xyz, SharedData::enbSettings.MasserBillboardTan);
		glow += GetMoonGlow(viewDirection, SharedData::SecundaDirection.xyz, SharedData::SecundaColor.xyz, SharedData::enbSettings.SecundaBillboardTan);
		return glow * (SharedData::enbSettings.SkyScatteringMoonGlowAmount * GetHorizonCrop(viewDirection.z));
	}

#if defined(CLOUDS)
	static const float IsotropicPhase = 0.25 / Math::PI;

	float PhaseThomasSchander(float cosTheta)
	{
		float p1 = cosTheta + 8.194068e-01;
		float4 expValues = exp(float4(-6.5e+01 * cosTheta - 5.5e+01, -8.370334e+01 * p1 * p1, 7.810083e+00 * cosTheta, -4.552125e-12 * cosTheta));
		float4 expWeights = float4(9.805233e-06, 1.388198e-01, 2.054747e-03, 2.600563e-02);
		return dot(expValues, expWeights) * 0.25;
	}

	float GetCloudLightOcclusion(float3 viewDirection, float3 lightDirection, SamplerState textureSampler)
	{
		static const float3 PoissonDisc[4] = {
			float3(0.460921, 0.615192, 0.887539),
			float3(0.757347, 0.911008, 0.189581),
			float3(0.548753, 0.145482, 0.0548723),
			float3(0.90051, 0.157048, 0.623493)
		};

		float occlusion = 0.0;
		[unroll] for (uint i = 0; i < 4; i++)
		{
			float3 sampleDirection = SafeNormalize(lerp(viewDirection, lightDirection, (float(i) + 0.5) / 32.0)) + (PoissonDisc[i] * 2.0 - 1.0) * 0.01;
			if (sampleDirection.z < 0.0)
				occlusion += -sampleDirection.z;
#	if defined(CLOUD_SHADOWS)
			else
				occlusion += CloudShadows::CloudSelfShadowTexture.SampleLevel(textureSampler, sampleDirection, 0);
#	endif
		}
		return saturate(occlusion * 0.25);
	}

	float GetCloudLightVisibility(float3 viewDirection, float3 lightDirection, float occlusionScale, SamplerState textureSampler)
	{
		float visibility = saturate(1.0 - GetCloudLightOcclusion(viewDirection, lightDirection, textureSampler) * occlusionScale);
		return pow(max(visibility * visibility, 1e-6), SharedData::enbSettings.CloudsLightingDensity);
	}

	float3 DesaturateCloudLight(float3 color)
	{
		return max(lerp(color, dot(color, 1.0 / 3.0), SharedData::enbSettings.CloudsLightingDesaturation), 0.0);
	}

	float GetCloudPhase(float3 viewDirection, float3 lightDirection, float cloudAlpha)
	{
		float anisotropic = lerp(PhaseThomasSchander(dot(viewDirection, lightDirection)), IsotropicPhase, saturate(cloudAlpha)) / IsotropicPhase;
		return max(0.0, lerp(1.0, anisotropic, SharedData::enbSettings.CloudsLightingForwardScattering));
	}

	float3 GetCloudEdgeLight(float3 viewDirection, bool scattering, SamplerState textureSampler)
	{
		float3 edge = 0.0;
		float fadePower = SharedData::enbSettings.CloudsEdgeFadePower;

		float sunAlpha = saturate(SharedData::SunColor.w);
		[branch] if (sunAlpha > 0.0)
		{
			float3 sunDirection = SafeNormalize(SharedData::SunDirection.xyz);
			float radius = GetBillboardRadius(viewDirection, sunDirection, SharedData::enbSettings.SunBillboardTan);
			[branch] if (radius < 1.0)
			{
				float3 color = max(SharedData::SunColor.xyz, 0.0);
				if (scattering && SharedData::enbSettings.CalculateCloudsEdgeFromScattering)
					color = GetScatteringColorAt(viewDirection) * (SharedData::enbSettings.SkyScatteringSunIntensity * sunAlpha);
				if (scattering)
					color *= GetCloudLightVisibility(viewDirection, sunDirection, 1.1, textureSampler);
				edge += color * pow(saturate(1.0 - radius), fadePower);
			}
		}

		float moonScale = SharedData::enbSettings.CloudsEdgeMoonMultiplier;
		[branch] if (moonScale > 0.0)
		{
			float3 masserDirection = SafeNormalize(SharedData::MasserDirection.xyz);
			float masserRadius = GetBillboardRadius(viewDirection, masserDirection, 8.0 * SharedData::enbSettings.MasserBillboardTan);
			[branch] if (masserRadius < 1.0)
			{
				float3 color = max(SharedData::MasserColor.xyz, 0.0) * moonScale;
				if (scattering)
					color *= GetCloudLightVisibility(viewDirection, masserDirection, 1.0, textureSampler);
				edge += color * pow(saturate(1.0 - masserRadius), fadePower);
			}

			float3 secundaDirection = SafeNormalize(SharedData::SecundaDirection.xyz);
			float secundaRadius = GetBillboardRadius(viewDirection, secundaDirection, 8.0 * SharedData::enbSettings.SecundaBillboardTan);
			[branch] if (secundaRadius < 1.0)
			{
				float3 color = max(SharedData::SecundaColor.xyz, 0.0) * moonScale;
				if (scattering)
					color *= GetCloudLightVisibility(viewDirection, secundaDirection, 1.0, textureSampler);
				edge += color * pow(saturate(1.0 - secundaRadius), fadePower);
			}
		}

		return edge * (SharedData::enbSettings.CloudsEdgeIntensity * GetHorizonCrop(viewDirection.z));
	}

	float3 GetCloudMoonLight(float3 viewDirection, SamplerState textureSampler)
	{
		float3 light = 0.0;

		float3 masserColor = max(SharedData::MasserColor.xyz, 0.0);
		[branch] if (any(masserColor > 0.0))
			light += masserColor * GetCloudLightVisibility(viewDirection, SafeNormalize(SharedData::MasserDirection.xyz), 1.0, textureSampler);

		float3 secundaColor = max(SharedData::SecundaColor.xyz, 0.0);
		[branch] if (any(secundaColor > 0.0))
			light += secundaColor * GetCloudLightVisibility(viewDirection, SafeNormalize(SharedData::SecundaDirection.xyz), 1.0, textureSampler);

		return DesaturateCloudLight(light) * (SharedData::enbSettings.CloudsEdgeMoonMultiplier * SharedData::enbSettings.CloudsLightingMoonIntensity * GetHorizonCrop(viewDirection.z));
	}

	float3 ShadeCloud(float3 cloudColor, float textureAlpha, float textureGray, float3 viewDirection, SamplerState textureSampler)
	{
		float colorGray = dot(cloudColor, 1.0 / 3.0);
		float3 result = cloudColor;
		bool scattering = SharedData::enbSettings.EnableCloudsScattering;

		float sunWeight = saturate(SharedData::SunColor.w * 4.0);
		[branch] if (scattering && sunWeight > 0.0)
		{
			float3 sunDirection = GetSunDirection();
			float facing = GetFacing(viewDirection, sunDirection);
			float planetShadow = GetEarthShadow(GetElevation(viewDirection, sunDirection.z), facing, sunDirection.z);

			float sunVisibility = SharedData::enbSettings.SkyScatteringSunVisibility;
			if (sunVisibility < 1.0) {
				float3 flattened = SafeNormalize(float3(viewDirection.xy, viewDirection.z * 8.0));
				planetShadow *= saturate(dot(flattened.xy, sunDirection.xy) * 0.5 + 0.5 + sunVisibility * 2.0 - 1.0);
			}

			float3 sunLit = result * lerp(1.0 - 0.5 * SharedData::enbSettings.SkyScatteringShadowAmount, 1.0, planetShadow);

			[branch] if (SharedData::enbSettings.CloudsLightingSunIntensity > 0.0)
			{
				float3 sunLight = DesaturateCloudLight(GetScatteringColorAt(sunDirection));
				float visibility = GetCloudLightVisibility(viewDirection, sunDirection, 1.2, textureSampler);
				float phase = GetCloudPhase(viewDirection, sunDirection, textureAlpha);
				sunLit += sunLight * (visibility * phase * planetShadow * textureGray * SharedData::enbSettings.CloudsLightingSunIntensity);
			}

			result = lerp(result, sunLit, sunWeight);
		}

		[branch] if (scattering && SharedData::enbSettings.EnableCloudsLightingFromMoon && SharedData::enbSettings.CloudsLightingMoonIntensity != 0.0)
			result += GetCloudMoonLight(viewDirection, textureSampler) * textureGray;

		float edgeWeight = saturate(1.0 - textureAlpha - SharedData::enbSettings.CloudsEdgeClamp);
		[branch] if (edgeWeight > 0.0)
			result += GetCloudEdgeLight(viewDirection, scattering, textureSampler) * (colorGray * edgeWeight);

		return result;
	}
#endif
}

#endif
