#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "FootstepParticles/Common.hlsli"

SamplerState LinearSampler : register(s0);

#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepth : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepth : register(t0);
#endif
Texture2D<float4> ShadowMaskTexture : register(t1);

#define SampColorSampler LinearSampler

#if defined(SKYLIGHTING)
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

#if defined(EXP_HEIGHT_FOG)
#	include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#endif

#include "Common/ShadowSampling.hlsli"

#if defined(LIGHT_LIMIT_FIX)
#	include "LightLimitFix/LightLimitFix.hlsli"
#endif

#if defined(ISL) && defined(LIGHT_LIMIT_FIX)
#	include "InverseSquareLighting/InverseSquareLighting.hlsli"
#endif

struct PS_INPUT
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	nointerpolation float4 Color : COLOR0;
	float3 WorldPosition : POSITION1;
	float4 FogParam : COLOR1;
	nointerpolation uint Type : TEXCOORD1;
	nointerpolation float Seed : TEXCOORD2;
	nointerpolation float Softness : TEXCOORD3;
	float ViewDepth : TEXCOORD4;
};

float3 GetPointLighting(float3 positionWS)
{
	float3 lighting = 0.0;
#if defined(LIGHT_LIMIT_FIX)
	float3 viewPosition = FrameBuffer::WorldToView(positionWS);
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	uint clusterIndex = 0;
	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		uint lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
		[loop] for (uint i = 0; i < lightCount; i++)
		{
			LightLimitFix::Light light = LightLimitFix::lights[LightLimitFix::lightList[lightOffset + i]];
			if (light.lightFlags & LightLimitFix::LightFlags::ShadowCaster)
				continue;
			float lightDistance = length(light.positionWS.xyz - positionWS);
#	if defined(ISL)
			float attenuation = InverseSquareLighting::GetAttenuation(lightDistance, light);
#	else
			float intensityFactor = saturate(lightDistance / light.radius);
			float attenuation = 1.0 - intensityFactor * intensityFactor;
#	endif
			const bool isLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
			lighting += Color::PointLight(light.color.xyz, isLinear) * attenuation * 0.5 * light.fade * Color::EffectLightingMult();
		}
	}
#endif
	return lighting;
}

float4 main(PS_INPUT input) : SV_Target
{
	uint type = input.Type & 0xFFu;
	bool snowy = (input.Type & 0x100u) != 0;
	float2 uv = input.TexCoord;
	float radiusSquared = dot(uv, uv);
	if (radiusSquared >= 1.0)
		discard;

	float shape;
	if (type == FootstepParticles::TypeClod) {
		float edge = 0.82 + 0.18 * sin(atan2(uv.y, uv.x) * 5.0 + input.Seed * Math::TAU);
		shape = saturate((edge - sqrt(radiusSquared)) * 6.0);
	} else if (type == FootstepParticles::TypeDroplet) {
		shape = saturate((1.0 - radiusSquared) * 2.5);
	} else {
		float noise = Random::perlinNoise(float3(uv * 1.7, input.Seed * 17.0)) * 0.5 + 0.5;
		float falloff = 1.0 - radiusSquared;
		shape = falloff * falloff * lerp(0.55, 1.25, noise);
	}

	float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	float softness = saturate((sceneDepth - input.ViewDepth) / max(input.Softness, 0.5));
	float coverage = saturate(shape * softness);
	float alpha = saturate(coverage * input.Color.a);
	if (alpha <= 0.002)
		discard;

	float3 positionWS = input.WorldPosition;
	float3 viewDirection = normalize(positionWS);
	float3 lightDirection = SharedData::DirLightDirection.xyz;

	float3 ambient = ShadowSampling::GetAmbientLighting();
	float3 directional = ShadowSampling::GetDirectionalLighting();

#if defined(EFFECTS11)
	if (SharedData::enbSettings.Enable) {
		directional *= SharedData::enbSettings.ParticleLightingInfluence;
		ambient *= SharedData::enbSettings.ParticleAmbientInfluence;
	}
#endif

#if defined(SKYLIGHTING)
	if (!SharedData::InInterior) {
		sh2 skylightingSH = Skylighting::SampleNoBias(positionWS);
		float skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, float3(0.0, 0.0, 1.0), Skylighting::GetFadeOutFactor(positionWS));
#	if defined(IBL)
		if (!SharedData::iblSettings.EnableIBL)
#	endif
		{
			ambient = Color::IrradianceToGamma(Color::IrradianceToLinear(ambient) * skylightingDiffuse);
		}
	}
#endif

	float2 screenUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy * SharedData::BufferDim.zw);
	float shadow = ShadowMaskTexture.SampleLevel(LinearSampler, screenUV, 0).x;
	shadow *= ShadowSampling::GetWorldShadow(positionWS, FrameBuffer::CameraPosAdjust.xyz);

#if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled)
		directional *= ExponentialHeightFog::GetSunlightFogAttenuation(positionWS, FrameBuffer::CameraPosAdjust.xyz);
#endif

	float3 right = FrameBuffer::ViewToWorld(float3(1.0, 0.0, 0.0), false);
	float3 up = FrameBuffer::ViewToWorld(float3(0.0, 1.0, 0.0), false);
	float3 sphereNormal = normalize(right * uv.x + up * uv.y - viewDirection * sqrt(saturate(1.0 - radiusSquared)));

	float3 pointLighting = GetPointLighting(positionWS);
	float3 color;
	float3 highlight = 0.0;

	if (type == FootstepParticles::TypeClod || type == FootstepParticles::TypeDroplet) {
		float wrap = saturate(dot(sphereNormal, lightDirection) * 0.6 + 0.4);
		color = input.Color.rgb * (ambient + directional * shadow * wrap + pointLighting);
		if (type == FootstepParticles::TypeDroplet || snowy) {
			float3 reflected = reflect(viewDirection, sphereNormal);
			float specularPower = type == FootstepParticles::TypeDroplet ? 48.0 : 96.0;
			float specular = pow(saturate(dot(reflected, lightDirection)), specularPower);
			float fresnel = pow(1.0 - saturate(dot(-viewDirection, sphereNormal)), 3.0);
			highlight = directional * shadow * specular * (type == FootstepParticles::TypeDroplet ? 1.5 : 0.6) + ambient * fresnel * (type == FootstepParticles::TypeDroplet ? 0.35 : 0.1);
		}
	} else {
		const float g = 0.3;
		float cosTheta = dot(viewDirection, lightDirection);
		float denominator = 1.0 + g * g - 2.0 * g * cosTheta;
		float phase = lerp(1.0, (1.0 - g * g) / (denominator * sqrt(denominator)), 0.5);
		color = input.Color.rgb * (ambient + directional * shadow * phase + pointLighting);
		if (snowy) {
			float2 cell = floor(uv * 6.0 + input.Seed * 37.0);
			float sparkle = step(0.93, FootstepParticles::Hash(asuint(cell.x) * 73856093u ^ asuint(cell.y) * 19349663u, asuint(input.Seed)));
			highlight = directional * shadow * sparkle * phase * 0.5;
		}
	}

	float fogFactor = 0.0;
	float3 fogColor = 0.0;
	if (FootstepParticles::FrameFlags & FootstepParticles::FrameFlagFog) {
		fogFactor = Color::FogAlpha(input.FogParam.w);
		fogColor = Color::Fog(input.FogParam.xyz);
#if defined(IBL)
		if (SharedData::iblSettings.EnableIBL)
			fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
#endif
	}

	float heightFogFactor = 0.0;
	float3 heightFogColor = 0.0;
#if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 heightFog = ExponentialHeightFog::GetExponentialHeightFog(positionWS, FrameBuffer::CameraPosAdjust.xyz, fogColor, float4(input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy, input.Position.z, 1.0));
		heightFogColor = heightFog.xyz;
		heightFogFactor = heightFog.w;
		if (ExponentialHeightFog::ShouldDisableVanillaFog())
			fogFactor = 0.0;
	}
#endif

	color = lerp(color, fogColor, fogFactor);
	color = lerp(color, heightFogColor, heightFogFactor);
	highlight *= (1.0 - fogFactor) * (1.0 - heightFogFactor);
	alpha *= 1.0 - heightFogFactor;
	coverage *= 1.0 - heightFogFactor;

	return float4(max(color * alpha + highlight * coverage * saturate(input.Color.a * 2.0), 0.0), alpha);
}
