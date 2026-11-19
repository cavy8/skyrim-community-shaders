#ifndef __EXPONENTIAL_HEIGHT_FOG_HLSLI__
#define __EXPONENTIAL_HEIGHT_FOG_HLSLI__

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "ExponentialHeightFog/VolumetricFogCommon.hlsli"

#if defined(DYNAMIC_CUBEMAPS)
#	include "DynamicCubemaps/DynamicCubemaps.hlsli"
#endif

#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

#if defined(PHYSICAL_SKY)
#	include "PhysicalSky/Common.hlsli"
#endif

Texture3D<float4> ExponentialHeightFogIntegratedLightScattering : register(t19);
Texture3D<float4> ExponentialHeightFogIntegratedLightScatteringFar : register(t22);
Texture2D<float4> ExponentialHeightFogIBLFogColor : register(t21);

namespace ExponentialHeightFog
{
	static const float MinDistanceHazeFadeDistance = 1.0f;

	float4 ApplyDistanceHaze(float4 heightFog, float3 hazeColor, float3 viewToPos)
	{
		[branch] if (SharedData::exponentialHeightFogSettings.distanceHazeMaxOpacity > 0.0f)
		{
			float horizontalDistance = length(viewToPos.xy);
			float hazeDistance = max(horizontalDistance - SharedData::exponentialHeightFogSettings.distanceHazeStartDistance, 0.0f);
			float fadeDistance = max(SharedData::exponentialHeightFogSettings.distanceHazeFadeDistance, MinDistanceHazeFadeDistance);
			float hazeOpacity = saturate(SharedData::exponentialHeightFogSettings.distanceHazeMaxOpacity) * smoothstep(0.0f, fadeDistance, hazeDistance);
			float visibleHazeOpacity = hazeOpacity * (1.0f - heightFog.a);
			float combinedOpacity = heightFog.a + visibleHazeOpacity;
			if (visibleHazeOpacity > 0.0f)
				heightFog = float4((heightFog.rgb * heightFog.a + hazeColor * visibleHazeOpacity) / combinedOpacity, combinedOpacity);
		}
		return heightFog;
	}

	float GetVanillaFogFade(float vanillaFogFade)
	{
		return SharedData::exponentialHeightFogSettings.respectVanillaFogFade != 0 ? vanillaFogFade : 1.0f;
	}

	bool ShouldDisableVanillaFog()
	{
		return SharedData::exponentialHeightFogSettings.enabled && SharedData::exponentialHeightFogSettings.disableVanillaFog != 0;
	}

	bool ShouldApplyVolumetricFog(out uint3 volumeSize)
	{
		volumeSize = 0u.xxx;
		bool applyVolumetricFog = false;
		[branch] if (SharedData::exponentialHeightFogSettings.enabled != 0 &&
					 SharedData::exponentialHeightFogSettings.volumetricFogEnabled != 0 &&
					 SharedData::viewAtmosphereSettings.localRange > GetVolumetricStartDistance() + 1.0f)
		{
			ExponentialHeightFogIntegratedLightScattering.GetDimensions(volumeSize.x, volumeSize.y, volumeSize.z);
			applyVolumetricFog = all(volumeSize > 0u);
		}
		return applyVolumetricFog;
	}

	float GetSceneDepthForFog(float3 positionWS, out float2 volumeUV)
	{
		volumeUV = 0.0f.xx;
		float sceneDepth = 0.0f;
		float4 clipPosition = mul(FrameBuffer::CameraViewProj, float4(positionWS, 1.0f));
		[branch] if (clipPosition.w > 0.0f)
		{
			sceneDepth = max(clipPosition.w, SharedData::CameraData.y);
			volumeUV = saturate(clipPosition.xy / clipPosition.w * float2(0.5f, -0.5f) + 0.5f);
		}
		return sceneDepth;
	}

	float4 SampleFogGrid(Texture3D<float4> volume, uint3 volumeSize, float2 volumeUV, float sceneDepth, float3 gridZ, float startDistance)
	{
		float slice = log2(max(sceneDepth * gridZ.x + gridZ.y, 1e-6f)) * gridZ.z / float(volumeSize.z);
		float3 texelCenter = 0.5f / float3(volumeSize);
		float2 uvMin = texelCenter.xy;
		float2 uvMax = 1.0f.xx - texelCenter.xy;
		float z = clamp(slice - texelCenter.z, texelCenter.z, 1.0f - texelCenter.z);
		float4 fog = volume.SampleLevel(SampColorSampler, float3(clamp(volumeUV, uvMin, uvMax), z), 0);
		float firstBackDepth = (exp2(1.0f / gridZ.z) - gridZ.y) / max(gridZ.x, 1e-20f);
		float nearWeight = saturate((sceneDepth - startDistance) / max(firstBackDepth - startDistance, EPSILON_DIVISION));
		return lerp(float4(0.0f.xxx, 1.0f), fog, nearWeight);
	}

	float4 SampleVolumetricFogPair(uint3 volumeSize, float2 volumeUV, float sceneDepth)
	{
		float4 fog = SampleFogGrid(ExponentialHeightFogIntegratedLightScattering, volumeSize, volumeUV, sceneDepth,
			GetVolumetricGridZParams(float(volumeSize.z)), GetVolumetricStartDistance());

		float nearGridEndDistance = GetVolumetricNearGridEndDistance();
		[branch] if (nearGridEndDistance + 1.0f < GetVolumetricTotalFarPlane() && sceneDepth > nearGridEndDistance)
		{
			uint3 farVolumeSize;
			ExponentialHeightFogIntegratedLightScatteringFar.GetDimensions(farVolumeSize.x, farVolumeSize.y, farVolumeSize.z);
			[branch] if (all(farVolumeSize > 0u))
			{
				float4 farFog = SampleFogGrid(ExponentialHeightFogIntegratedLightScatteringFar, farVolumeSize, volumeUV, sceneDepth,
					GetVolumetricFarGridZParams(float(farVolumeSize.z)), nearGridEndDistance);
				fog = float4(fog.rgb + fog.a * farFog.rgb, fog.a * farFog.a);
			}
		}
		return fog;
	}

	float4 CompositeFogScattering(float4 analyticalFog, float4 volume)
	{
		float opacity = saturate(1.0f - volume.a * (1.0f - analyticalFog.a));
		float3 premultiplied = volume.rgb + volume.a * analyticalFog.rgb;
		return float4(opacity > EPSILON_DIVISION ? premultiplied / opacity : 0.0f.xxx, opacity);
	}

	float4 CombineVolumetricFog(float4 analyticalFog, float3 positionWS, uint3 volumeSize)
	{
		float2 volumeUV;
		float sceneDepth = GetSceneDepthForFog(positionWS, volumeUV);
		float4 volume = sceneDepth > 0.0f ? SampleVolumetricFogPair(volumeSize, volumeUV, sceneDepth) : float4(0.0f.xxx, 1.0f);
		return CompositeFogScattering(analyticalFog, volume);
	}

	float4 CombineVolumetricFog(float4 analyticalFog, float4 screenPosition, uint3 volumeSize)
	{
		// Callers provide full-resolution pixel coordinates by undoing dynamic-resolution
		// scaling. Normalize by the full buffer so this matches the UV space used to build
		// the froxel volume and its conservative depth.
		float2 volumeUV = screenPosition.xy * SharedData::BufferDim.zw;
		float sceneDepth = SharedData::GetScreenDepth(screenPosition.z);
		[branch] if (SharedData::viewAtmosphereSettings.lookupJitter > 0.0f)
		{
			float2 noise = float2(
				Random::InterleavedGradientNoise(screenPosition.xy, SharedData::FrameCount),
				Random::InterleavedGradientNoise(screenPosition.yx + 19.19f, SharedData::FrameCount));
			volumeUV += (noise * 2.0f - 1.0f) * SharedData::viewAtmosphereSettings.lookupJitter /
			            max(float2(volumeSize.xy), 1.0f.xx);
		}
		float4 volume = SampleVolumetricFogPair(volumeSize, volumeUV, sceneDepth);
		return CompositeFogScattering(analyticalFog, volume);
	}

	float4 GetExponentialHeightFogInternal(float3 positionWS, float3 cameraWS, float3 fogColor, bool useScreenPosition, float4 screenPosition, bool applyVolumetricFog)
	{
#if defined(PHYSICAL_SKY)
		// Reflection AP already includes this fog, including its extinction.
		if (ViewAtmosphere::ReflectionEnabled())
			return 0.0;
#endif
		float fogHeightFalloff = SharedData::exponentialHeightFogSettings.fogHeightFalloff * 0.001f;
		float fogDensity = GetHeightFogDensity();
		float fogHeightFalloff2 = GetHeightFogFalloff2();
		float fogDensity2 = GetHeightFogDensity2();
		if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0) {
			fogDensity *= SharedData::exponentialHeightFogSettings.volumetricFogExtinctionScale;
			fogDensity2 *= SharedData::exponentialHeightFogSettings.volumetricFogExtinctionScale;
		}
		bool hasHeightFog = fogDensity > 0.0f || fogDensity2 > 0.0f;
		if (!hasHeightFog && SharedData::exponentialHeightFogSettings.distanceHazeMaxOpacity <= 0.0f)
			return 0.0f.xxxx;
		float3 viewToPos = positionWS;
		uint3 volumeSize = 0u.xxx;
		applyVolumetricFog = applyVolumetricFog && hasHeightFog;
		if (applyVolumetricFog)
			applyVolumetricFog = ShouldApplyVolumetricFog(volumeSize);

		float viewToPosLength = length(viewToPos);
		float viewToPosLengthInv = rcp(max(viewToPosLength, 1e-4f));

		float rayStartHeight = cameraWS.z;
		float rayLength = viewToPosLength;

		float excludeDistance = SharedData::exponentialHeightFogSettings.startDistance;
		if (applyVolumetricFog) {
			float2 volumeUV;
			float sceneDepth = GetSceneDepthForFog(positionWS, volumeUV);
			float cosAngle = sceneDepth * viewToPosLengthInv;
			float invCosAngle = cosAngle > 0.001f ? rcp(cosAngle) : 0.0f;
			excludeDistance = max(excludeDistance, GetVolumetricEndDistance() * invCosAngle);
		}

		if (excludeDistance > 0) {
			excludeDistance = min(excludeDistance, viewToPosLength);
			float excludeIntersectionTime = excludeDistance * viewToPosLengthInv;
			float cameraToExclusionIntersectionZ = excludeIntersectionTime * viewToPos.z;
			float exclusionIntersectionZ = cameraWS.z + cameraToExclusionIntersectionZ;
			rayLength = (1.0f - excludeIntersectionTime) * viewToPosLength;
			rayStartHeight = exclusionIntersectionZ;
		}

		const float rayEndHeight = cameraWS.z + viewToPos.z;
		const float2 fogHeights = float2(SharedData::exponentialHeightFogSettings.fogHeight, SharedData::exponentialHeightFogSettings.fogHeight2);
		const float2 layerIntegrals = HeightDensityIntegral(rayStartHeight - fogHeights, rayEndHeight - fogHeights, float2(fogHeightFalloff, fogHeightFalloff2));
		float exponentialHeightLineIntegralCalc = fogDensity * layerIntegrals.x + fogDensity2 * layerIntegrals.y;
		float exponentialHeightLineIntegral = exponentialHeightLineIntegralCalc * rayLength;

		float expFogFactor = hasHeightFog ? saturate(exp2(-exponentialHeightLineIntegral)) : 1.0f;

		float3 fogInscatteringColor = 0.0f.xxx;
		if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0) {
			fogInscatteringColor = GetFogAmbientColor(viewToPosLength);
		} else if (SharedData::exponentialHeightFogSettings.originalFogColorAmount > 0.0f) {
			if (!SharedData::InInterior) {
				fogColor = ColorManagement::SRGBToWorking(SharedData::exponentialHeightFogSettings.vanillaFogNearColor.rgb);
#if defined(IBL)
				if (SharedData::iblSettings.EnableIBL) {
					const float4 iblFogColor = ExponentialHeightFogIBLFogColor.Load(int3(0, 0, 0));
					if (iblFogColor.w > 0.0f)
						fogColor = iblFogColor.rgb;
				}
#endif
			}
			float3 originalFogColor = lerp(fogColor, SharedData::exponentialHeightFogSettings.originalFogColorOverride.rgb, SharedData::exponentialHeightFogSettings.originalFogColorOverride.a);
			fogInscatteringColor = originalFogColor * SharedData::exponentialHeightFogSettings.originalFogColorAmount;
		}
		fogInscatteringColor += SharedData::exponentialHeightFogSettings.fogInscatteringColor.rgb * SharedData::exponentialHeightFogSettings.fogInscatteringColor.a;

#if defined(IBL) || defined(DYNAMIC_CUBEMAPS)
		[branch] if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings == 0)
		{
			float cubemapMipLevel = SharedData::exponentialHeightFogSettings.cubemapMipLevel;
			float3 cubemapDirection = normalize(lerp(positionWS, float3(0, 0, 1), saturate((cubemapMipLevel + 1) / 9)));
			float3 inscattering = 0.0;
			[branch] if (SharedData::InInterior)
			{
#	if defined(DYNAMIC_CUBEMAPS)
				if (SharedData::exponentialHeightFogSettings.useDynamicCubemaps > 0)
					inscattering = DynamicCubemaps::EnvTexture.SampleLevel(SampColorSampler, cubemapDirection, cubemapMipLevel).xyz;
#	endif
			}
			else
			{
#	if defined(IBL)
				if (SharedData::exponentialHeightFogSettings.useSkyIBL > 0)
					inscattering = ImageBasedLighting::GetSkyIBLColor(cubemapDirection);
#	endif
			}
			fogInscatteringColor += inscattering * SharedData::exponentialHeightFogSettings.inscatteringTint.rgb * SharedData::exponentialHeightFogSettings.inscatteringTint.a;
		}
#endif

		fogColor = fogInscatteringColor;

		float3 directionalInscattering = 0;

		float3 viewDirection = viewToPos * viewToPosLengthInv;

		// Calculate directional light inscattering using Henyey-Greenstein phase function
		if (SharedData::exponentialHeightFogSettings.directionalInscatteringMultiplier > 0) {
			float3 dirLightColor = Color::DirectionalLight(SharedData::DirLightColor.xyz);
#if defined(PHYSICAL_SKY) && defined(COMMON_HLSLI)
			if (SharedData::physSkyData.enabled) {
				float3 physSkyTransmittance = PhysSky::SampleDirLightTr(SampColorSampler);
				dirLightColor *= saturate(physSkyTransmittance);
			}
#endif
			float3 lightDirection = normalize(SharedData::DirLightDirection.xyz);
			float cosTheta = dot(lightDirection, viewDirection);
			float phase = HenyeyGreenstein(cosTheta, SharedData::exponentialHeightFogSettings.directionalInscatteringAnisotropy);
			float3 directionalLightInscattering = dirLightColor * phase;
			directionalInscattering = directionalLightInscattering * SharedData::exponentialHeightFogSettings.directionalInscatteringMultiplier;
			if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0)
				directionalInscattering *= SharedData::exponentialHeightFogSettings.fogLightingInfluence;
		}

		fogColor += directionalInscattering;
		float4 analyticalFog = float4(fogColor * (1.0f - expFogFactor), 1.0f - expFogFactor);
		float4 combinedFog;
		if (!applyVolumetricFog) {
			combinedFog = float4(analyticalFog.a > EPSILON_DIVISION ? analyticalFog.rgb / analyticalFog.a : 0.0f.xxx, analyticalFog.a);
		} else {
			combinedFog = useScreenPosition ? CombineVolumetricFog(analyticalFog, screenPosition, volumeSize) : CombineVolumetricFog(analyticalFog, positionWS, volumeSize);
		}
		return ApplyDistanceHaze(combinedFog, fogColor, viewToPos);
	}

	float4 GetExponentialHeightFog(float3 positionWS, float3 cameraWS, float3 fogColor)
	{
		return GetExponentialHeightFogInternal(positionWS, cameraWS, fogColor, false, 0.0f.xxxx, true);
	}

	float4 GetExponentialHeightFog(float3 positionWS, float3 cameraWS, float3 fogColor, float4 screenPosition)
	{
		return GetExponentialHeightFogInternal(positionWS, cameraWS, fogColor, true, screenPosition, true);
	}

	float4 GetExponentialHeightFogNoVolumetric(float3 positionWS, float3 cameraWS, float3 fogColor)
	{
		return GetExponentialHeightFogInternal(positionWS, cameraWS, fogColor, false, 0.0f.xxxx, false);
	}

	float4 GetExponentialHeightFogNoVolumetric(float3 positionWS, float3 cameraWS, float3 fogColor, float4 screenPosition)
	{
		return GetExponentialHeightFogInternal(positionWS, cameraWS, fogColor, true, screenPosition, false);
	}

	float GetSunlightFogAttenuation(float3 positionWS, float3 cameraWS)
	{
		float fogHeightFalloff = SharedData::exponentialHeightFogSettings.fogHeightFalloff * 0.001f;
		float fogDensity = GetHeightFogDensity();
		float fogHeightFalloff2 = GetHeightFogFalloff2();
		float fogDensity2 = GetHeightFogDensity2();
		if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0) {
			fogDensity *= SharedData::exponentialHeightFogSettings.volumetricFogExtinctionScale;
			fogDensity2 *= SharedData::exponentialHeightFogSettings.volumetricFogExtinctionScale;
		}
		if (fogDensity <= 0.0f && fogDensity2 <= 0.0f) {
			return 1.0f;
		}

		float exponent = fogHeightFalloff * max(positionWS.z + cameraWS.z - SharedData::exponentialHeightFogSettings.fogHeight, 0.0f);
		float localDensity = fogDensity * exp2(-exponent);
		float exponent2 = fogHeightFalloff2 * max(positionWS.z + cameraWS.z - SharedData::exponentialHeightFogSettings.fogHeight2, 0.0f);
		float localDensity2 = fogDensity2 * exp2(-exponent2);

		float3 lightDir = SharedData::DirLightDirection.xyz;
		float lightDirZ = lightDir.z;

		float sunlightFogAttenuation = 0.0f;

		// Integral = Density * (1 - exp2(-slope * inf)) / slope
		if (lightDirZ > 0.001f) {
			float slope = max(fogHeightFalloff * lightDirZ, 1e-8f);
			float slope2 = max(fogHeightFalloff2 * lightDirZ, 1e-8f);
			float exponentialHeightLineIntegral = localDensity / slope + localDensity2 / slope2;
			sunlightFogAttenuation = saturate(exp2(-exponentialHeightLineIntegral));
		}

		float attenuationAmount = SharedData::exponentialHeightFogSettings.sunlightAttenuationAmount;
		if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0)
			attenuationAmount *= SharedData::exponentialHeightFogSettings.fogLightingInfluence;
		return lerp(1.0f, sunlightFogAttenuation, attenuationAmount);
	}

	float GetSunFogAttenuation(float3 cameraWS)
	{
		float fogHeightFalloff = SharedData::exponentialHeightFogSettings.fogHeightFalloff * 0.001f;
		float fogDensity = GetHeightFogDensity();
		float fogHeightFalloff2 = GetHeightFogFalloff2();
		float fogDensity2 = GetHeightFogDensity2();
		if (SharedData::exponentialHeightFogSettings.useVanillaFogSettings != 0) {
			fogDensity *= SharedData::exponentialHeightFogSettings.volumetricFogExtinctionScale;
			fogDensity2 *= SharedData::exponentialHeightFogSettings.volumetricFogExtinctionScale;
		}
		if (fogDensity <= 0.0f && fogDensity2 <= 0.0f) {
			return 1.0f;
		}

		float exponent = fogHeightFalloff * max(cameraWS.z - SharedData::exponentialHeightFogSettings.fogHeight, 0.0f);
		float localDensity = fogDensity * exp2(-exponent);
		float exponent2 = fogHeightFalloff2 * max(cameraWS.z - SharedData::exponentialHeightFogSettings.fogHeight2, 0.0f);
		float localDensity2 = fogDensity2 * exp2(-exponent2);

		float3 lightDir = SharedData::DirLightDirection.xyz;
		float lightDirZ = lightDir.z;

		float sunFogAttenuation = 0.0f;

		// Integral = Density * (1 - exp2(-slope * inf)) / slope
		if (lightDirZ > 0.001f) {
			float slope = max(fogHeightFalloff * lightDirZ, 1e-8f);
			float slope2 = max(fogHeightFalloff2 * lightDirZ, 1e-8f);
			float exponentialHeightLineIntegral = localDensity / slope + localDensity2 / slope2;
			sunFogAttenuation = saturate(exp2(-exponentialHeightLineIntegral));
		}
		return sunFogAttenuation;
	}
}
#endif
