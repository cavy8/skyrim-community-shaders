#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/SharedData.hlsli"

#if defined(PROCEDURAL_SUN)
#	include "ProceduralSun/ProceduralSun.hlsli"

bool IsProceduralSunActive()
{
	bool effects11OwnsSun = false;
#	if defined(EFFECTS11)
	effects11OwnsSun = SharedData::enbSettings.EnableProceduralSun != 0;
#	endif
	return SharedData::proceduralSunSettings.enabled && !effects11OwnsSun &&
	       (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun) &&
	       (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld) &&
	       !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection);
}
#endif

struct VS_INPUT
{
	float4 Position: POSITION0;

#if defined(TEX) || defined(HORIZFADE)
	float2 TexCoord: TEXCOORD0;
#endif

	float4 Color: COLOR0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;

#if defined(DITHER) && defined(TEX)
	float4 TexCoord0: TEXCOORD0;
#elif defined(DITHER)
	float2 TexCoord0: TEXCOORD3;
#elif defined(TEX) || defined(HORIZFADE)
	float2 TexCoord0: TEXCOORD0;
#endif

#if defined(TEXLERP)
	float2 TexCoord1: TEXCOORD1;
#endif

#if defined(HORIZFADE)
	float TexCoord2: TEXCOORD2;
#endif

#if defined(TEX) || defined(DITHER) || defined(HORIZFADE)
	float4 Color: COLOR0;
#endif

#if !defined(OCCLUSION) && !defined(MOONMASK) && !defined(HORIZFADE)
	float4 SkyBlendColor0: TEXCOORD5;
	float4 SkyBlendColor2: TEXCOORD6;
#endif

	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
	float3 FogPosition: TEXCOORD4;
};

#ifdef VSHADER
cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj : packoffset(c0);
	row_major float4x4 World : packoffset(c4);
	row_major float4x4 PreviousWorld : packoffset(c8);
	float3 EyePosition : packoffset(c12);
	float VParams : packoffset(c12.w);
	float4 BlendColor[3] : packoffset(c13);
	float2 TexCoordOff : packoffset(c16);
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float4 inputPosition = float4(input.Position.xyz, 1.0);
	float4 previousInputPosition = inputPosition;

#	if defined(PROCEDURAL_SUN) && defined(TEX) && !defined(DITHER)
	if (IsProceduralSunActive()) {
		float outerCos = SharedData::proceduralSunSettings.haloEnabled && SharedData::proceduralSunSettings.haloIntensity > 0.0f ?
		                     SharedData::proceduralSunSettings.sunHaloCos :
		                     SharedData::proceduralSunSettings.sunDiskCos;
		inputPosition.xyz = ProceduralSun::ResizeBillboardVertex(
			input.Position.xyz, World, SharedData::proceduralSunSettings.sunQuadModelRadius, outerCos);
		previousInputPosition.xyz = ProceduralSun::ResizeBillboardVertex(
			input.Position.xyz, PreviousWorld, SharedData::proceduralSunSettings.sunQuadModelRadius, outerCos);
	}
#	endif

#	if defined(OCCLUSION)

#		if defined(PROCEDURAL_SUN)
	if (IsProceduralSunActive()) {
		inputPosition.xyz *= ProceduralSun::GetOcclusionBillboardScale(SharedData::proceduralSunSettings.sunQuadModelRadius);
		previousInputPosition = inputPosition;
	}
#		endif

#	elif defined(MOONMASK)

	vsout.TexCoord0 = input.TexCoord;
	vsout.Color = float4(VParams.xxx, 1.0);

#	elif defined(HORIZFADE)

	float worldHeight = mul(World, inputPosition).z;
	float eyeHeightDelta = -EyePosition.z + worldHeight;

	vsout.TexCoord0.xy = input.TexCoord;
	vsout.TexCoord2.x = saturate((1.0 / 17.0) * eyeHeightDelta);
	vsout.Color.xyz = BlendColor[0].xyz * VParams;
	vsout.Color.w = BlendColor[0].w;

#	else  // MOONMASK HORIZFADE

#		if defined(DITHER)

#			if defined(TEX)
	vsout.TexCoord0.xyzw = input.TexCoord.xyxy * float4(1.0, 1.0, 501.0, 501.0);
#			else
	float3 inputDirection = normalize(input.Position.xyz);
	inputDirection.y += inputDirection.z;

	vsout.TexCoord0.x = 501 * acos(inputDirection.x);
	vsout.TexCoord0.y = 501 * asin(inputDirection.y);
#			endif  // TEX

#		elif defined(CLOUDS)
	vsout.TexCoord0.xy = TexCoordOff + input.TexCoord;
#		else
	vsout.TexCoord0.xy = input.TexCoord;
#		endif  // DITHER CLOUDS

#		ifdef TEXLERP
	vsout.TexCoord1.xy = TexCoordOff + input.TexCoord;
#		endif  // TEXLERP

	float3 skyColor = BlendColor[0].xyz * input.Color.xxx + BlendColor[1].xyz * input.Color.yyy +
	                  BlendColor[2].xyz * input.Color.zzz;

	vsout.Color.xyz = VParams * skyColor;
	vsout.Color.w = BlendColor[0].w * input.Color.w;
	vsout.SkyBlendColor0 = float4(BlendColor[0].xyz * VParams, 0);
	vsout.SkyBlendColor2 = float4(BlendColor[2].xyz * VParams, 0);
#	endif      // OCCLUSION MOONMASK HORIZFADE

#	ifdef REVERSE_Z
	float4 skyPosition = mul(WorldViewProj, inputPosition);
	vsout.Position = float4(skyPosition.xy, FrameBuffer::FarPlaneClipZ(skyPosition.w), skyPosition.w);
#	else
	vsout.Position = mul(WorldViewProj, inputPosition).xyww;
#	endif
	vsout.WorldPosition = mul(World, inputPosition);
	vsout.FogPosition = vsout.WorldPosition.xyz - EyePosition.xyz;
	vsout.PreviousWorldPosition = mul(PreviousWorld, previousInputPosition);

	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
	float4 MotionVectors: SV_Target1;
	float4 Normal: SV_Target2;
#if defined(CLOUD_SHADOWS) && defined(CLOUDS) && !defined(DEFERRED)
	float4 CloudShadows: SV_Target3;
#endif
};

#ifdef PSHADER
SamplerState SampBaseSampler : register(s0);
SamplerState SampBlendSampler : register(s1);
SamplerState SampNoiseGradSampler : register(s2);

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexBlendSampler : register(t1);
Texture2D<float4> TexNoiseGradSampler : register(t2);

cbuffer PerGeometry : register(b2)
{
	float2 PParams : packoffset(c0);
};

cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}

#	include "Common/MotionBlur.hlsli"
#	include "Common/SharedData.hlsli"

#	if defined(CLOUD_SHADOWS)
#		include "CloudShadows/CloudShadows.hlsli"
#	endif

#	if defined(EFFECTS11) && defined(CLOUDS)
#		include "Effects11/SkyScattering.hlsli"
#	endif

#	if defined(CLOUD_RELIGHT) && defined(CLOUD_SHADOWS) && defined(TEX) && defined(CLOUDS)
#		define CR_CLOUDS
#		include "CloudRelight/CloudRelight.hlsli"
#	endif

#	if defined(EXP_HEIGHT_FOG)
#		define SampColorSampler SampBaseSampler
#		include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#	endif

#	ifdef HDR_OUTPUT
#		include "HDRDisplay/HDRSun.hlsli"
#	endif

Texture2D<float> TexDepthSampler : register(t17);

#	if defined(EFFECTS11)
float ComputeProceduralSun(float2 uv)
{
	float2 p = uv * 2.0 - 1.0;
	float dist = dot(p, p) - SharedData::enbSettings.ProceduralSunDiskRadiusSq;

	float c = saturate(dist * SharedData::enbSettings.ProceduralSunCoronaScale);
	float corona = (1.0 - c) * rcp(SharedData::enbSettings.ProceduralSunCoronaFalloff * c + 1.0) * SharedData::enbSettings.ProceduralSunGlowIntensity;

	float disk = saturate(-dist * SharedData::enbSettings.ProceduralSunDiskEdgeScale);

	return corona + disk;
}
#	endif

float3 ComposeSkyColor(float3 skyColor, float3 textureColor, float3 skyOffset, bool composeAuthoredSky)
{
	if (composeAuthoredSky)
		return Color::Sky(skyColor * textureColor + skyOffset);
	return Color::Sky(skyColor) * textureColor + Color::Sky(skyOffset);
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	float3 skyScale = PParams.yyy;
	bool composeAuthoredSky = ENABLE_LL;

#	ifndef OCCLUSION
#		ifndef TEXLERP
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
	if (!composeAuthoredSky)
		baseColor.xyz = Color::Sky(baseColor.xyz);
#			ifdef TEXFADE
	baseColor.w *= PParams.x;
#			endif
#		else
	float4 blendColor = TexBlendSampler.Sample(SampBlendSampler, input.TexCoord1.xy);
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
	if (!composeAuthoredSky) {
		blendColor.xyz = Color::Sky(blendColor.xyz);
		baseColor.xyz = Color::Sky(baseColor.xyz);
	}
	baseColor = PParams.xxxx * (-baseColor + blendColor) + baseColor;
#		endif
#		if defined(CR_CLOUDS)
	if (SharedData::cloudRelightSettings.enabled) {
		float3 viewDir = normalize(input.WorldPosition.xyz);
		baseColor.rgb = CloudRelight::RelightCloud(baseColor, viewDir, SampBaseSampler);
	}
#		endif

#		if defined(HDR_OUTPUT)
	if (HDRSun::IsHdrSunActive()) {
		if (composeAuthoredSky)
			baseColor.xyz = Color::Sky(baseColor.xyz);
		composeAuthoredSky = false;
		float hdrSunGain = HDRSun::GetHdrSunGain(input.TexCoord0.xy, baseColor);
		baseColor.xyz *= hdrSunGain;
	}
#		endif

	// Standalone Procedural Sun and Effects11's own simpler procedural sun (ComputeProceduralSun,
	// below) both replace the vanilla sun texture; ownership must be deterministic when both are
	// enabled. Effects11's EnableProceduralSun setting always wins when set -- the standalone
	// block below is skipped via effects11OwnsSun, and Effects11's own block (unconditional on
	// its own setting) runs after and overwrites baseColor again. When Effects11's toggle is off,
	// the standalone feature's own "enabled" setting decides. Matches upstream's coexistence
	// design (alandtse/open-shaders), adapted to this file's structure rather than reimplemented.
#		if defined(PROCEDURAL_SUN) && defined(TEX)
	if (IsProceduralSunActive()) {
		float3 viewDirection = normalize(input.WorldPosition.xyz);
		float cosTheta = clamp(dot(viewDirection, SharedData::SunDirection.xyz), -1.0f, 1.0f);
		float3 limbDarkening;
		float discCoverage;
		ProceduralSun::EvaluateDisc(
			cosTheta,
			SharedData::proceduralSunSettings.sunDiskCos,
			SharedData::proceduralSunSettings.edgeSoftness,
			limbDarkening,
			discCoverage);

		float haloProfile = 0.0f;
		if (SharedData::proceduralSunSettings.haloEnabled) {
			haloProfile = ProceduralSun::EvaluateHalo(
				cosTheta,
				SharedData::proceduralSunSettings.sunDiskCos,
				SharedData::proceduralSunSettings.sunHaloCos,
				SharedData::proceduralSunSettings.haloFalloff);
		}

		float3 proceduralSunColor;
		float sunCoverage;
		ProceduralSun::ComposeDiscAndHalo(
			limbDarkening,
			discCoverage,
			SharedData::proceduralSunSettings.diskIntensity,
			haloProfile,
			SharedData::proceduralSunSettings.haloIntensity,
			proceduralSunColor,
			sunCoverage);

		baseColor.xyz = proceduralSunColor;
		composeAuthoredSky = false;
		baseColor.w = sunCoverage;
#			if defined(CLOUD_SHADOWS)
		if (sunCoverage > 0.0f && SharedData::proceduralSunSettings.cloudExtinction > 0.0f) {
			float capturedCloudOcclusion = CloudShadows::CloudShadowsTexture.SampleLevel(SampBaseSampler, viewDirection, 0).x;
			baseColor.w *= ProceduralSun::GetGlareCloudTransmission(capturedCloudOcclusion, SharedData::proceduralSunSettings.cloudExtinction);
		}
#			endif
		skyScale = 0.0;
	}
#		endif  // defined(PROCEDURAL_SUN) && defined(TEX)

#		if defined(TEX) && defined(EFFECTS11)
	if (SharedData::enbSettings.EnableProceduralSun && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun)) {
		baseColor.xyz = ComputeProceduralSun(input.TexCoord0.xy);
		composeAuthoredSky = false;
		baseColor.w = input.Color.w;
		skyScale = 0.0;
	}
#		endif

#		if defined(DITHER)
	float2 noiseGradUv = float2(0.125, 0.125) * input.Position.xy;
	float noiseGrad = TexNoiseGradSampler.Sample(SampNoiseGradSampler, noiseGradUv).x * 0.03125 - 0.0078125;
	noiseGrad *= 10.0;

#			ifdef TEX
	psout.Color.xyz = ComposeSkyColor(input.Color.xyz, baseColor.xyz, skyScale, composeAuthoredSky);
	psout.Color.xyz *= 1.0 + noiseGrad;
	psout.Color.w = baseColor.w * input.Color.w;
#			else
	float3 skyGradientColor = input.Color.xyz;

#if defined(EFFECTS11)
	float3 viewDirection = normalize(input.WorldPosition.xyz);
	if (SharedData::enbSettings.UseProceduralGradientWeights) {
		float gradientPosition = pow(1.0 - saturate(viewDirection.z), SharedData::enbSettings.ProceduralGradientWeightCurve);
		skyGradientColor = lerp(input.SkyBlendColor2.xyz, input.SkyBlendColor0.xyz, gradientPosition);
	}
#endif
	psout.Color.xyz = ComposeSkyColor(skyGradientColor, 1.0, skyScale, ENABLE_LL);

	psout.Color.xyz *= 1.0 + noiseGrad;
	psout.Color.w = input.Color.w;
#			endif  // TEX

#		elif defined(MOONMASK)
	psout.Color.xyzw = baseColor;
	if (composeAuthoredSky)
		psout.Color.xyz = Color::Sky(psout.Color.xyz);

	if (baseColor.w - AlphaTestRefRS.x < 0) {
		discard;
	}

#		elif defined(HORIZFADE)
	psout.Color.xyz = composeAuthoredSky ? Color::Sky(1.5 * (input.Color.xyz * baseColor.xyz + skyScale)) :
	                                       1.5 * ComposeSkyColor(input.Color.xyz, baseColor.xyz, skyScale, false);
	psout.Color.w = input.TexCoord2.x * (baseColor.w * input.Color.w);
#		else

#		if defined(CLOUDS) && defined(EFFECTS11)
	if (SharedData::enbSettings.Enable)
		baseColor.xyz = pow(abs(baseColor.xyz), SharedData::enbSettings.CloudsCurve);
#		endif

	psout.Color.w = input.Color.w * baseColor.w;
	psout.Color.xyz = ComposeSkyColor(input.Color.xyz, baseColor.xyz, skyScale, composeAuthoredSky);

#			if defined(PROCEDURAL_SUN) && defined(TEX) && defined(DEFERRED) && !defined(CLOUDS) && !defined(MOONMASK)
	if (IsProceduralSunActive())
		psout.Color = ProceduralSun::ToAdditiveBlend(psout.Color, SharedData::proceduralSunSettings.radianceLimit);
#			endif

#			if defined(CLOUDS) && defined(EFFECTS11)
	if (SharedData::enbSettings.Enable) {
		float3 cloudColor = psout.Color.xyz;
		float3 viewDirection = normalize(input.WorldPosition.xyz);

		cloudColor.xyz = lerp(abs(cloudColor.xyz), dot(cloudColor.xyz, 1.0 / 3.0), SharedData::enbSettings.CloudsDesaturation);

		float cloudLuminance = dot(cloudColor.xyz, 1.0 / 3.0);

		float sunLighting = saturate(dot(viewDirection, SharedData::SunDirection.xyz) * 0.5 + 0.5);
		float masserLighting = saturate(dot(viewDirection, SharedData::MasserDirection.xyz) * 0.5 + 0.5);
		float secundaLighting = saturate(dot(viewDirection, SharedData::SecundaDirection.xyz) * 0.5 + 0.5);

		float3 edgeTransmittance = 0.0;
		if (SharedData::enbSettings.EnableCloudsScattering) {
			float3 scatteringTransmittance;
			cloudColor = SkyScattering::RelightCloud(cloudColor, cloudLuminance, saturate(psout.Color.w), viewDirection, input.Position.xy, SampBaseSampler, scatteringTransmittance);
			if (SharedData::enbSettings.CalculateCloudsEdgeFromScattering)
				edgeTransmittance = scatteringTransmittance;
		}

		if (SharedData::enbSettings.CloudsEdgeIntensity > 0.0) {
			float cloudsEdgeAlpha = saturate(1.0 - baseColor.w);

			float3 sunPhase = pow(sunLighting, 32.0) * SharedData::SunColor.xyz * max(cloudsEdgeAlpha, edgeTransmittance.x);
			float3 masserPhase = pow(masserLighting, 32.0) * SharedData::MasserColor.xyz * SharedData::enbSettings.CloudsEdgeMoonMultiplier * max(cloudsEdgeAlpha, edgeTransmittance.y);
			float3 secundaPhase = pow(secundaLighting, 32.0) * SharedData::SecundaColor.xyz * SharedData::enbSettings.CloudsEdgeMoonMultiplier * max(cloudsEdgeAlpha, edgeTransmittance.z);

			float3 cloudsScatter = (sunPhase + masserPhase + secundaPhase) * SharedData::enbSettings.CloudsEdgeIntensity;

			cloudColor += cloudLuminance * cloudsScatter;
		}

		psout.Color.xyz = cloudColor;
		psout.Color.w = saturate(psout.Color.w);
	}
#			endif

#			if defined(CLOUDS) && defined(DEFERRED) && defined(PROCEDURAL_SUN)
	float cloudExtinction = SharedData::proceduralSunSettings.cloudExtinction * SharedData::proceduralSunSettings.sunVisibility;
	[branch] if (SharedData::proceduralSunSettings.enabled && cloudExtinction > 0.0 &&
		(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld)) {
		float3 cloudViewDirection = normalize(input.WorldPosition.xyz);
		float cloudSunCosTheta = dot(cloudViewDirection, SharedData::SunDirection.xyz);
		float influenceCos = ProceduralSun::GetInfluenceCos(
			SharedData::proceduralSunSettings.sunDiskCos,
			SharedData::proceduralSunSettings.haloEnabled,
			SharedData::proceduralSunSettings.sunHaloCos,
			SharedData::proceduralSunSettings.haloIntensity);

		[branch] if (cloudSunCosTheta > influenceCos) {
			float sunMask;
			float sunProfile;
			ProceduralSun::EvaluateCloudExtinction(
				cloudSunCosTheta,
				SharedData::proceduralSunSettings.sunDiskCos,
				SharedData::proceduralSunSettings.edgeSoftness,
				SharedData::proceduralSunSettings.diskIntensity,
				SharedData::proceduralSunSettings.haloEnabled,
				SharedData::proceduralSunSettings.sunHaloCos,
				SharedData::proceduralSunSettings.haloIntensity,
				SharedData::proceduralSunSettings.haloFalloff,
				sunMask,
				sunProfile);
			float sunLuminance = sunProfile * ProceduralSun::GetSunLuminance(SharedData::SunColor);
			float sunShare = sunLuminance / max(sunLuminance + Color::RGBToLuminance(max(psout.Color.xyz, 0.0)), 1e-5);
			psout.Color = ProceduralSun::ApplyCloudExtinction(psout.Color, 1.0 + cloudExtinction * sunMask, sunShare);
		}
	}
#			endif
#		endif

#	else
	psout.Color = float4(0, 0, 0, 1.0);
#	endif  // OCCLUSION

#	if defined(EXP_HEIGHT_FOG)
	const bool inReflection = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection) != 0;
	if (inReflection && SharedData::exponentialHeightFogSettings.enabled) {
		float3 skyFogPosition = normalize(input.FogPosition.xyz) * SharedData::CameraData.x;
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFogNoVolumetric(skyFogPosition, FrameBuffer::CameraPosAdjust.xyz, psout.Color.xyz, float4(input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy, input.Position.z, 1));
		psout.Color.xyz = lerp(psout.Color.xyz, exponentialHeightFog.xyz, exponentialHeightFog.w);
	}
#	endif

	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);

	psout.MotionVectors = float4(screenMotionVector, 0, psout.Color.w);
	psout.Normal = float4(0.5, 0.5, 0, psout.Color.w);

#	if defined(CLOUD_SHADOWS) && defined(CLOUDS) && !defined(DEFERRED)
	psout.CloudShadows = psout.Color.w;

	// Keep sun behind scene depth to prevent halo leaks through geometry.
	float depth = TexDepthSampler.Load(int3(input.Position.xy, 0));
#		ifdef REVERSE_Z
	if (depth > 0.0 && depth < 1.0 && SharedData::GetScreenDepth(depth) < SharedData::GetScreenDepth(input.Position.z) * 0.99)
#		else
	if (depth < input.Position.z)
#		endif
		psout.Color.w = 0;

#	else
	// Even without cloud shadows enabled, sun disc should be occluded by scene depth (clouds, terrain, etc.)
	[branch] if ((Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun) && psout.Color.w > 0.0) {
		float depth = TexDepthSampler.Load(int3(input.Position.xy, 0));
#		ifdef REVERSE_Z
		if (depth > 0.0 && depth < 1.0 && SharedData::GetScreenDepth(depth) < SharedData::GetScreenDepth(input.Position.z) * 0.99)
#		else
		if (depth < input.Position.z)
#		endif
			psout.Color.w = 0;
	}
#	endif

	return psout;
}
#endif
