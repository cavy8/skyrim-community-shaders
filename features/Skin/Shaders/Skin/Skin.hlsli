#ifndef __SKIN_HLSLI__
#define __SKIN_HLSLI__

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/LightDisk.hlsli"
#include "Common/LightingCommon.hlsli"
#include "Common/Math.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"

namespace Skin
{
	float CalculateCurvature(float3 N)
	{
		const float3 dNdx = ddx(N);
		const float3 dNdy = ddy(N);
		return length(float2(dot(dNdx, dNdx), dot(dNdy, dNdy)));
	}

#if defined(PSHADER)
	cbuffer SkinPerGeometry : register(b7)
	{
		float4 skinPerGeometry;
		float4 skinMaterialFlags;
		SharedData::SkinData skinPerGeometryProfile;
	};

	SharedData::SkinData GetSkinData()
	{
		return skinPerGeometryProfile;
	}
#else
	SharedData::SkinData GetSkinData()
	{
		return SharedData::skinData;
	}
#endif
#if defined(CS_SKIN_SHADING)
	Texture2D<float4> TexSkinDetailNormal : register(t72);
	SamplerState DetailSampler : register(s15);

	float3x3 SurfaceBasis(float3 position, float3 normal, float2 uv)
	{
		float3 dx = ddx(position), dy = ddy(position);
		float2 du = ddx(uv), dv = ddy(uv);
		float determinant = du.x * dv.y - du.y * dv.x;
		float signUV = determinant < 0 ? -1.0f : 1.0f;
		float3 tangent = (dx * dv.y - dy * du.y) * signUV;
		tangent -= normal * dot(normal, tangent);
		float3 fallbackAxis = abs(normal.z) < 0.999f ? float3(0, 0, 1) : float3(0, 1, 0);
		if (abs(determinant) < 1e-12f || dot(tangent, tangent) < 1e-12f)
			tangent = cross(fallbackAxis, normal);
		tangent = normalize(tangent);
		float3 bitangent = (dy * du.x - dx * dv.x) * signUV;
		float handedness = dot(cross(normal, tangent), bitangent) < 0 ? -1.0f : 1.0f;
		return float3x3(tangent, cross(normal, tangent) * handedness, normal);
	}

	// [Jorge Jimenez, Diego Gutierrez 2015, "Separable Subsurface Scattering"]
	// https://www.iryoku.com/separable-sss/
	float3 SSSSTransmittance(float translucency, float sssWidth, float NdotL, float d, float2 disk)
	{
		/**
		* Calculate the scale of the effect.
		*/
		float scale = 8.25 * (1.0 - translucency) / sssWidth;

		/**
		* First we shrink the position inwards the surface to avoid artifacts:
		* (Note that this can be done once for all the lights)
		*/
		// float4 shrinkedPos = float4(worldPosition - 0.005 * worldNormal, 1.0);

		/**
		* Now we calculate the thickness from the light point of view:
		*/
		// float4 shadowPosition = mul(shrinkedPos, lightViewProjection);
		// float d1 = SSSSSampleShadowmap(shadowPosition.xy / shadowPosition.w).r; // 'd1' has a range of 0..1
		// float d2 = shadowPosition.z; // 'd2' has a range of 0..'lightFarPlane'
		// d1 *= lightFarPlane; // So we scale 'd1' accordingly:
		// float d = scale * abs(d1 - d2);
		d = scale * abs(d);  // Use the passed 'd' value instead of calculating it here.

		/**
		* Armed with the thickness, we can now calculate the color by means of the
		* precalculated transmittance profile.
		* (It can be precomputed into a texture, for maximum performance):
		*/
		float dd = -d * d;
		float3 profile = float3(0.233, 0.455, 0.649) * exp(dd / 0.0064) +
		                 float3(0.1, 0.336, 0.344) * exp(dd / 0.0484) +
		                 float3(0.118, 0.198, 0.0) * exp(dd / 0.187) +
		                 float3(0.113, 0.007, 0.007) * exp(dd / 0.567) +
		                 float3(0.358, 0.004, 0.0) * exp(dd / 1.99) +
		                 float3(0.078, 0.0, 0.0) * exp(dd / 7.41);

		/**
		* Using the profile, we finally approximate the transmitted lighting from
		* the back of the object:
		*/
		return profile * LightDisk::WrappedCosine(-NdotL, 0.3, 1.0, disk);
	}

	float3 DualSpecularGGX(float AverageRoughness, float Lobe0Roughness, float Lobe1Roughness, float LobeMix, float3 SpecularColor, float NdotL, float NdotV, float NdotH, float VdotH, float2 disk, out float3 F)
	{
		const float lobe0 = BRDF::D_GGX(max(Lobe0Roughness, 0.04), NdotH) * LightDisk::GGXNormalization(max(Lobe0Roughness, 0.04), disk);
		const float lobe1 = BRDF::D_GGX(max(Lobe1Roughness, 0.04), NdotH) * LightDisk::GGXNormalization(max(Lobe1Roughness, 0.04), disk);
		float D = lerp(lobe0, lobe1, LobeMix);
		float G = BRDF::Vis_SmithJointApprox(AverageRoughness, NdotV, NdotL);
		F = BRDF::F_Schlick(SpecularColor, VdotH);

		return D * G * F;
	}

	float3 CharlieSpecular(float3 N, float3 V, float3 L, float NdotV, MaterialProperties material)
	{
		const float3 H = normalize(V + L);
		const float NdotL = clamp(dot(N, L), 1e-5, 1.0);
		return BRDF::D_Charlie(material.FuzzRoughness, saturate(dot(N, H))) * BRDF::Vis_Neubelt(NdotV, NdotL) *
		       BRDF::F_Schlick(material.FuzzColor, saturate(dot(V, H))) * NdotL;
	}

	float FilterRoughness(float roughness, float3 normalDx, float3 normalDy)
	{
		const float variance = 0.25 * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
		const float alpha = roughness * roughness;
		return sqrt(sqrt(saturate(alpha * alpha + min(2.0 * variance, 0.18))));
	}

	float NormalMapShadow(Texture2D<float4> normalTexture, SamplerState normalSampler, float2 uv, float3x3 decode, float3x3 frame, float3 lightDir, float worldPerUV, float footprint, float4 shadowParams)
	{
		const float maxSlope = 2.7;
		const float3 localLight = mul(frame, lightDir);
		const float lengthXY = length(localLight.xy);
		if (lengthXY < 1e-3)
			return 1.0;
		const float tanElevation = max(localLight.z, 0.0) / lengthXY;
		if (tanElevation >= maxSlope)
			return 1.0;

		const float2 dir = localLight.xy / lengthXY;
		const float uvPerWorld = rcp(max(worldPerUV, 1e-4));
		const float traceLength = shadowParams.y;
		const float hardness = shadowParams.z;
		float2 dimensions;
		normalTexture.GetDimensions(dimensions.x, dimensions.y);
		const float texDim = max(dimensions.x, dimensions.y);

		const uint stepCount = 8;
		float height = 0.0;
		float occlusion = 0.0;
		float previousEnd = 0.0;
		[loop] for (uint i = 1; i <= stepCount; i++)
		{
			const float t = (float)i / (float)stepCount;
			const float end = traceLength * t * t;
			const float stepLength = end - previousEnd;
			const float2 sampleUV = uv + dir * ((previousEnd + stepLength * 0.5) * uvPerWorld);
			const float mipLevel = log2(max(max(footprint, stepLength * uvPerWorld) * texDim, 1.0));
			const float3 encoded = normalTexture.SampleLevel(normalSampler, sampleUV, mipLevel).xyz;
#	if defined(MODELSPACENORMALS)
			const float3 n = mul(frame, normalize(mul(decode, encoded.xzy * 2.0 - 1.0)));
#	else
			const float3 n = mul(frame, normalize(mul(decode, encoded * 2.0 - 1.0)));
#	endif
			const float slope = -dot(n.xy, dir) / max(n.z, 0.35);
			height += stepLength * (slope - tanElevation);
			occlusion = max(occlusion, height / end * (1.0 - t * t));
			if (occlusion * hardness >= 1.0 || height + (maxSlope - tanElevation) * (traceLength - end) <= 0.0)
				break;
			previousEnd = end;
		}
		return 1.0 - saturate(occlusion * hardness) * shadowParams.x;
	}

	float3 PenumbraScattering(float shadow)
	{
		const float3 bleed = float3(0.6, 0.15, 0.1);
		const float s = saturate(shadow);
		return s + bleed * s * (1.0 - s);
	}

	void SkinDirectLightInput(
		out DirectLightingOutput lightingOutput,
		DirectContext context,
		MaterialProperties material)
	{
		lightingOutput = (DirectLightingOutput)0;
		context.lightColor *= Color::PBRLightingCompensation * PenumbraScattering(context.detailedShadow);

		const float3 N = context.worldNormal;
		const float3 V = context.viewDir;
		const float3 L = context.lightDir;
		const float3 specularLight = LightDisk::SpecularDirection(N, V, L, context.lightDisk);
		const float3 H = context.lightDisk.x > 0.0 ? normalize(V + specularLight) : context.halfVector;

		const float oNdotL = dot(N, L);
		const float diffuseCosine = max(LightDisk::DiffuseCosine(oNdotL, context.lightDisk), 1e-5);
		const float NdotL = clamp(dot(N, specularLight), 1e-5, 1.0);
		float NdotV = saturate(abs(dot(N, V)) + EPSILON_DOT_CLAMP);
		float NdotH = saturate(dot(N, H));
		float VdotH = saturate(dot(V, H));

		float averageRoughness = lerp(material.Roughness, material.RoughnessSecondary, material.SecondarySpecIntensity);
		SharedData::SkinData skinData = GetSkinData();

#	if defined(DEFERRED)
		const float3 scatteredCosine = diffuseCosine;
#	else
		const float3 wrap = float3(0.4, 0.15, 0.1) * saturate(skinData.sssParams.z);
		const float3 scatteredCosine = max(float3(
											   LightDisk::WrappedCosine(oNdotL, wrap.x, rcp(1.0 + wrap.x), context.lightDisk),
											   LightDisk::WrappedCosine(oNdotL, wrap.y, rcp(1.0 + wrap.y), context.lightDisk),
											   LightDisk::WrappedCosine(oNdotL, wrap.z, rcp(1.0 + wrap.z), context.lightDisk)),
			1e-5);
#	endif

		lightingOutput.diffuse += context.lightColor * scatteredCosine * BRDF::Diffuse_Burley(averageRoughness, NdotV, diffuseCosine, saturate(dot(V, context.halfVector)));
		float3 F;
		float3 F0 = material.F0;

		lightingOutput.specular += DualSpecularGGX(averageRoughness, material.Roughness, material.RoughnessSecondary, material.SecondarySpecIntensity, F0, NdotL, NdotV, NdotH, VdotH, context.lightDisk, F) * context.lightColor * NdotL;

		lightingOutput.specular *= material.DirectSpecularEnergyCompensation;
		lightingOutput.diffuse *= 1 - F;

		if (material.FuzzWeight > 0.0) {
			float3 fuzzF = BRDF::F_Schlick(material.FuzzColor, saturate(dot(V, context.halfVector)));
			float3 fuzzSpecular = CharlieSpecular(N, V, L, NdotV, material);
			if (context.lightDisk.x > 0.0) {
				float3 center, x, y;
				LightDisk::QuadratureAxes(L, context.lightDisk, center, x, y);
				fuzzSpecular = 0.25 * (CharlieSpecular(N, V, center + x, NdotV, material) + CharlieSpecular(N, V, center - x, NdotV, material) +
										  CharlieSpecular(N, V, center + y, NdotV, material) + CharlieSpecular(N, V, center - y, NdotV, material));
			}
			fuzzSpecular *= context.lightColor;
			float2 fuzzSpecularBRDF = BRDF::EnvBRDFApproxLazarov(material.FuzzRoughness, NdotV);
			fuzzSpecular *= 1 + material.FuzzColor * (1 / (fuzzSpecularBRDF.x + fuzzSpecularBRDF.y) - 1);

			float3 retainedEnergy = 1.0f - saturate(fuzzF * material.FuzzWeight);
			lightingOutput.diffuse *= retainedEnergy;
			lightingOutput.specular *= retainedEnergy;
			lightingOutput.specular += fuzzSpecular * material.FuzzWeight;
		}

		float3 sssTransmittance = SSSSTransmittance(
									  0.0f,
									  max(skinData.sssParams.y, 1e-4f),
									  oNdotL,
									  material.Thickness,
									  context.lightDisk) *
		                          skinData.sssParams.x * skinData.sssParams.w;
		lightingOutput.transmission = min(sssTransmittance * context.lightColor * context.softShadow * material.BaseColor, context.lightColor);
	}

	void SkinIndirectLobeWeights(
		out IndirectLobeWeights lobeWeights,
		MaterialProperties material,
		IndirectContext context)
	{
		lobeWeights = (IndirectLobeWeights)0;

		const float3 N = context.worldNormal;
		const float3 V = context.viewDir;
		const float3 VN = context.vertexNormal;

		float NdotV = saturate(dot(N, V));

		float averageRoughness = lerp(material.Roughness, material.RoughnessSecondary, material.SecondarySpecIntensity);

		float2 specularBRDF = BRDF::EnvBRDF(averageRoughness, NdotV);

		lobeWeights.specular = material.F0 * specularBRDF.x + specularBRDF.y;

		lobeWeights.diffuse = material.BaseColor * (1.0 - saturate(lobeWeights.specular));
		lobeWeights.specular *= 1 + material.F0 * (1 / (specularBRDF.x + specularBRDF.y) - 1);

		float3 R = reflect(-V, N);
		float horizon = min(1.0 + dot(R, VN), 1.0);
		horizon *= horizon;
		lobeWeights.specular *= horizon;

		float3 diffuseAO = MultiBounceAO(material.BaseColor, material.AO);
		float3 specularAO = MultiBounceAO(material.F0, SpecularAOLagarde(NdotV, material.AO, averageRoughness));

		lobeWeights.diffuse *= diffuseAO;
		lobeWeights.specular *= specularAO;
	}

	float FBM(float2 uv, float base_scale, int octaves, float lacunarity, float persistence, float z_offset_multiplier)
	{
		float total = 0.0;
		float frequency = base_scale;
		float amplitude = 1.0;
		float max_amplitude = 0.0;
		for (int i = 0; i < octaves; i++) {
			total += amplitude * (Random::perlinNoise(float3(uv * frequency, (float)i * z_offset_multiplier)) + 1.0) * 0.5;

			max_amplitude += amplitude;
			amplitude *= persistence;
			frequency *= lacunarity;
		}
		if (max_amplitude > 0.0) {
			return total / max_amplitude;
		}
		return 0.0;
	}

	float PerlinNoise(float2 uv, float scale, float lacunarity, float persistence, float strength)
	{
		if (strength <= 0.001f) {
			return 0.0f;
		}
		if (strength >= 0.999f) {
			return 1.0f;
		}
		int octaves = 5;
		float z_offset_multiplier = 7.375f;

		float noise_value = FBM(uv, scale, octaves, lacunarity, persistence, z_offset_multiplier);

		float dynamic_threshold = 1.0f - strength;

		float sweat_intensity = saturate((noise_value - dynamic_threshold) / strength);

		sweat_intensity = pow(sweat_intensity, 1.5f);

		if (strength > 0.8f) {
			sweat_intensity = sweat_intensity * saturate(0.99f - (strength - 0.8f) * 5.0f) + (strength - 0.8f) * 5.0f;
		}
		return pow(sweat_intensity, 0.1f);
	}
#endif

	float2 GetWetness(float z, float3 modelNormal)
	{
		if (!any(skinPerGeometry.xyw))
			return 0.f;

		float waterWet = 0.0f;
		float waterLevel = skinPerGeometry.z;

		waterWet = skinPerGeometry.y * (1 - smoothstep(waterLevel - 2.5f, waterLevel + 2.5f, z));
		waterWet = max(waterWet, skinPerGeometry.w);

		float sweatWet = skinPerGeometry.x;
#if !defined(SKIN)
		sweatWet *= 1.0f - saturate(dot(modelNormal, float3(0, 0, 1)));
#endif
		return float2(sweatWet, waterWet);
	}
}

#endif  // __SKIN_HLSLI__
