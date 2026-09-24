#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "FootstepParticles/Common.hlsli"

#if defined(SKYLIGHTING)
#	define SKYLIGHTING_PROBE_REGISTER t5
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepth : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepth : register(t0);
#endif
Texture2D<unorm float3> AlbedoTexture : register(t1);
Texture2D<unorm float3> NormalRoughnessTexture : register(t2);
Texture2D<float> SnowMapTexture : register(t3);
StructuredBuffer<FootstepParticles::FootstepEvent> Events : register(t4);

RWStructuredBuffer<FootstepParticles::Particle> Particles : register(u0);

SamplerState LinearSampler : register(s0);

groupshared uint gsCounts[FootstepParticles::TypeCount];
groupshared float3 gsGroundColor;
groupshared float3 gsSnowColor;
groupshared float3 gsWaterColor;
groupshared float gsSnowWeight;
groupshared float gsWetness;

static const uint kGroundSamples = 12;

bool ProjectToPixel(float3 positionWS, out int2 pixel, out float expectedDepth)
{
	pixel = 0;
	expectedDepth = 0.0;
	float4 clip = mul(FrameBuffer::CameraViewProj, float4(positionWS, 1.0));
	if (clip.w <= 1.0)
		return false;
	float3 ndc = clip.xyz / clip.w;
	float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
	if (any(uv <= 0.0) || any(uv >= 1.0))
		return false;
	pixel = int2(FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv) * SharedData::BufferDim.xy);
	expectedDepth = SharedData::GetScreenDepth(ndc.z);
	return true;
}

float SampleGround(float3 groundWS, float scale, uint seed, out float3 color, out float3 normal)
{
	float3 colorSum = 0.0;
	float3 normalSum = 0.0;
	float count = 0.0;
	float rotation = FootstepParticles::Hash(seed, 101) * Math::TAU;

	[loop] for (uint i = 0; i < kGroundSamples; i++)
	{
		bool innerRing = i < kGroundSamples / 2;
		float radius = (innerRing ? 16.0 : 30.0) * scale;
		float angle = rotation + (i % (kGroundSamples / 2)) * (Math::TAU / (kGroundSamples / 2)) + (innerRing ? 0.0 : Math::PI / 6.0);
		float3 samplePosition = groundWS + float3(cos(angle), sin(angle), 0.0) * radius;

		int2 pixel;
		float expectedDepth;
		if (!ProjectToPixel(samplePosition, pixel, expectedDepth))
			continue;

		float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(pixel, 0)));
		if (abs(sceneDepth - expectedDepth) > max(6.0, expectedDepth * 0.015))
			continue;

		colorSum += AlbedoTexture.Load(int3(pixel, 0));
		float3 normalVS = GBuffer::DecodeNormal(NormalRoughnessTexture.Load(int3(pixel, 0)).xy);
		normalSum += mul(FrameBuffer::CameraViewInverse, float4(normalVS, 0.0)).xyz;
		count += 1.0;
	}

	color = count > 0.0 ? colorSum / count : 0.0;
	normal = count > 0.0 && dot(normalSum, normalSum) > 1e-6 ? normalize(normalSum) : float3(0.0, 0.0, 1.0);
	if (normal.z < 0.1)
		normal = float3(0.0, 0.0, 1.0);
	return count / kGroundSamples;
}

float GetSkyOcclusion(float3 positionWS, float3 normal)
{
#if defined(SKYLIGHTING)
	sh2 skylightingSH = Skylighting::Sample(positionWS, normal);
	return saturate(SphericalHarmonics::Unproject(skylightingSH, float3(0.0, 0.0, 1.0)));
#else
	return 1.0;
#endif
}

void EvaluateWetness(float3 absolutePosition, float3 normal, float skyOcclusion, out float wetness, out float puddle)
{
	wetness = 0.0;
	puddle = 0.0;
	if (!(FootstepParticles::FrameFlags & FootstepParticles::FrameFlagWetness))
		return;

	float minWetnessAngle = saturate(max(SharedData::wetnessEffectsSettings.MinRainWetness, normal.z));
	float rainWetness = SharedData::wetnessEffectsSettings.Wetness * minWetnessAngle * SharedData::wetnessEffectsSettings.MaxRainWetness;
	float puddleWetness = SharedData::wetnessEffectsSettings.PuddleWetness * minWetnessAngle;
	float occlusion = saturate(skyOcclusion * 2.0);
	wetness = saturate(rainWetness * occlusion);

	if (rainWetness > 0.0 || puddleWetness > 0.0) {
		float3 puddleCoords = (absolutePosition * 0.5 + 0.5) * 0.01 / SharedData::wetnessEffectsSettings.PuddleRadius;
		float value = Random::perlinNoise(puddleCoords) * 0.5 + 0.5;
		value = value * ((minWetnessAngle / SharedData::wetnessEffectsSettings.PuddleMaxAngle) * SharedData::wetnessEffectsSettings.MaxPuddleWetness * 0.25) + 0.5;
		value *= lerp(rainWetness, puddleWetness, saturate(value - 0.25));
		puddle = saturate(value * occlusion);
	}
}

float EvaluateSnowCover(float3 absolutePosition, float3 normal, float skyOcclusion)
{
	if (!(FootstepParticles::FrameFlags & FootstepParticles::FrameFlagSnowCover))
		return 0.0;

	float2 uv = absolutePosition.xy * SharedData::snowCoverSettings.mapScale + SharedData::snowCoverSettings.mapOffset;
	float heightThreshold = absolutePosition.z - SharedData::snowCoverSettings.SnowHeightOffset + (SnowMapTexture.SampleLevel(LinearSampler, uv, 0) - 0.5) * SharedData::snowCoverSettings.mapZscale;
	float environment = (heightThreshold + SharedData::snowCoverSettings.SeasonalAltitude) / max(SharedData::snowCoverSettings.BlendSmoothness, 1.0);
	float timeSnowing = SharedData::snowCoverSettings.TimeSnowing;
	float weather = timeSnowing * timeSnowing * timeSnowing * max(500.0, SharedData::snowCoverSettings.SnowingDensity) / 500.0;
	weather = clamp(weather * max(SharedData::snowCoverSettings.minAngle, normal.z), -1.0, 1.0);
	float amount = saturate(max(environment, weather));
	float skylight = smoothstep(0.0, 0.75, skyOcclusion);
	return saturate(skylight * amount * smoothstep(SharedData::snowCoverSettings.minAngle, SharedData::snowCoverSettings.maxAngle, normal.z));
}

float3 GetSurfaceColor(uint surface)
{
	switch (surface) {
	case FootstepParticles::SurfaceDirt:
		return float3(0.33, 0.26, 0.19);
	case FootstepParticles::SurfaceGravel:
		return float3(0.42, 0.40, 0.37);
	case FootstepParticles::SurfaceSand:
		return float3(0.62, 0.55, 0.42);
	case FootstepParticles::SurfaceGrass:
		return float3(0.30, 0.29, 0.19);
	case FootstepParticles::SurfaceMud:
		return float3(0.21, 0.16, 0.11);
	case FootstepParticles::SurfaceSnow:
		return float3(0.88, 0.90, 0.94);
	case FootstepParticles::SurfaceIce:
		return float3(0.78, 0.85, 0.92);
	case FootstepParticles::SurfaceAsh:
		return float3(0.40, 0.39, 0.38);
	default:
		return float3(0.42, 0.41, 0.40);
	}
}

void GetSurfaceCounts(uint surface, out float counts[FootstepParticles::TypeCount])
{
	[unroll] for (uint i = 0; i < FootstepParticles::TypeCount; i++)
		counts[i] = 0.0;

	switch (surface) {
	case FootstepParticles::SurfaceDirt:
		counts[FootstepParticles::TypeDust] = 6.0;
		counts[FootstepParticles::TypeClod] = 5.0;
		break;
	case FootstepParticles::SurfaceGravel:
		counts[FootstepParticles::TypeDust] = 3.0;
		counts[FootstepParticles::TypeClod] = 6.0;
		break;
	case FootstepParticles::SurfaceSand:
		counts[FootstepParticles::TypeDust] = 7.0;
		counts[FootstepParticles::TypeClod] = 7.0;
		break;
	case FootstepParticles::SurfaceGrass:
		counts[FootstepParticles::TypeDust] = 2.0;
		counts[FootstepParticles::TypeClod] = 2.0;
		break;
	case FootstepParticles::SurfaceMud:
		counts[FootstepParticles::TypeClod] = 9.0;
		break;
	case FootstepParticles::SurfaceSnow:
		counts[FootstepParticles::TypeSnow] = 8.0;
		counts[FootstepParticles::TypeClod] = 7.0;
		break;
	case FootstepParticles::SurfaceIce:
		counts[FootstepParticles::TypeSnow] = 2.0;
		counts[FootstepParticles::TypeClod] = 3.0;
		break;
	case FootstepParticles::SurfaceAsh:
		counts[FootstepParticles::TypeDust] = 10.0;
		break;
	case FootstepParticles::SurfaceWater:
		counts[FootstepParticles::TypeDroplet] = 16.0;
		counts[FootstepParticles::TypeMist] = 3.0;
		break;
	default:
		break;
	}
}

bool IsLooseSurface(uint surface)
{
	return surface == FootstepParticles::SurfaceDirt || surface == FootstepParticles::SurfaceSand ||
	       surface == FootstepParticles::SurfaceGrass || surface == FootstepParticles::SurfaceMud ||
	       surface == FootstepParticles::SurfaceGravel;
}

void PlanFootstep(FootstepParticles::FootstepEvent footstep)
{
	float3 groundWS = footstep.Position - FrameBuffer::CameraPosAdjust.xyz;
	float scale = max(footstep.Scale, 0.25);

	float3 sampledColor;
	float3 normal;
	float coverage = SampleGround(groundWS, scale, footstep.Seed, sampledColor, normal);

	float skyOcclusion = GetSkyOcclusion(groundWS + float3(0.0, 0.0, 8.0), normal);

	float wetness;
	float puddle;
	EvaluateWetness(footstep.Position, normal, skyOcclusion, wetness, puddle);
	float snowCover = EvaluateSnowCover(footstep.Position, normal, skyOcclusion);

	uint surface = footstep.Surface;
	float snowWeight = (surface == FootstepParticles::SurfaceSnow || surface == FootstepParticles::SurfaceIce) ? 1.0 : smoothstep(0.3, 0.7, snowCover);
	if (surface == FootstepParticles::SurfaceWater)
		snowWeight = 0.0;

	float counts[FootstepParticles::TypeCount];
	GetSurfaceCounts(surface, counts);

	float snowCounts[FootstepParticles::TypeCount];
	GetSurfaceCounts(FootstepParticles::SurfaceSnow, snowCounts);
	[unroll] for (uint i = 0; i < FootstepParticles::TypeCount; i++)
		counts[i] = lerp(counts[i], max(counts[i], snowCounts[i]), (surface == FootstepParticles::SurfaceSnow || surface == FootstepParticles::SurfaceIce) ? 0.0 : snowWeight);
	if (snowWeight > 0.0 && surface != FootstepParticles::SurfaceSnow && surface != FootstepParticles::SurfaceIce)
		counts[FootstepParticles::TypeDust] *= 1.0 - snowWeight;

	float dryness = 1.0 - smoothstep(0.1, 0.5, wetness);
	counts[FootstepParticles::TypeDust] *= dryness;
	counts[FootstepParticles::TypeSnow] *= 1.0 - 0.5 * smoothstep(0.3, 0.9, wetness);
	if (IsLooseSurface(surface))
		counts[FootstepParticles::TypeClod] += 4.0 * wetness * (1.0 - snowWeight);

	float raining = (FootstepParticles::FrameFlags & FootstepParticles::FrameFlagWetness) ? saturate(SharedData::wetnessEffectsSettings.Raining) : 0.0;
	float splash = max(smoothstep(0.45, 0.85, puddle), smoothstep(0.35, 0.9, wetness) * lerp(0.2, 0.45, raining));
	splash *= 1.0 - 0.75 * snowWeight;
	counts[FootstepParticles::TypeDroplet] += 16.0 * splash;
	counts[FootstepParticles::TypeMist] += 3.0 * smoothstep(0.3, 1.0, splash);

	float strength = footstep.Strength * ((footstep.Flags & FootstepParticles::EventFlagLanding) ? 1.6 : 1.0);
	counts[FootstepParticles::TypeDust] *= FootstepParticles::DustIntensity * strength;
	counts[FootstepParticles::TypeClod] *= lerp(FootstepParticles::DustIntensity, FootstepParticles::SnowIntensity, snowWeight) * strength;
	counts[FootstepParticles::TypeSnow] *= FootstepParticles::SnowIntensity * strength;
	counts[FootstepParticles::TypeDroplet] *= FootstepParticles::SplashIntensity * strength;
	counts[FootstepParticles::TypeMist] *= FootstepParticles::SplashIntensity * strength;

	uint remaining = FootstepParticles::SlotsPerEvent;
	[unroll] for (uint type = 0; type < FootstepParticles::TypeCount; type++)
	{
		uint count = min((uint)round(max(counts[type], 0.0)), remaining);
		gsCounts[type] = count;
		remaining -= count;
	}

	float3 fallbackColor = Color::Diffuse(GetSurfaceColor(surface));
	float sampleWeight = saturate(coverage * 3.0);
	float3 groundColor = lerp(fallbackColor, sampledColor, sampleWeight);
	float3 snowColor = Color::Diffuse(GetSurfaceColor(FootstepParticles::SurfaceSnow));
	if (surface == FootstepParticles::SurfaceIce)
		snowColor = Color::Diffuse(GetSurfaceColor(FootstepParticles::SurfaceIce));
	snowColor = lerp(snowColor, max(sampledColor, snowColor * 0.8), sampleWeight * 0.35);

	gsGroundColor = groundColor;
	gsSnowColor = snowColor;
	gsWaterColor = lerp(Color::Diffuse(float3(0.80, 0.83, 0.86)), groundColor, 0.3 * (1.0 - puddle));
	gsSnowWeight = snowWeight;
	gsWetness = wetness;
}

uint ResolveType(uint localIndex, out bool alive)
{
	uint start = 0;
	[unroll] for (uint type = 0; type < FootstepParticles::TypeCount; type++)
	{
		uint end = start + gsCounts[type];
		if (localIndex < end) {
			alive = true;
			return type;
		}
		start = end;
	}
	alive = false;
	return 0;
}

[numthreads(64, 1, 1)] void main(uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID) {
	uint eventIndex = groupID.x;
	uint localIndex = threadID.x;
	if (eventIndex >= FootstepParticles::EventCount)
		return;

	FootstepParticles::FootstepEvent footstep = Events[eventIndex];

	if (localIndex == 0)
		PlanFootstep(footstep);

	GroupMemoryBarrierWithGroupSync();

	uint slot = (footstep.FirstSlot + localIndex) % FootstepParticles::MaxParticles;
	FootstepParticles::Particle particle = (FootstepParticles::Particle)0;

	bool alive;
	uint type = ResolveType(localIndex, alive);
	if (!alive) {
		Particles[slot] = particle;
		return;
	}

	uint seed = footstep.Seed * 0x9E3779B9u + localIndex * 0x632BE5ABu;
	float r0 = FootstepParticles::Hash(seed, 0);
	float r1 = FootstepParticles::Hash(seed, 1);
	float r2 = FootstepParticles::Hash(seed, 2);
	float r3 = FootstepParticles::Hash(seed, 3);
	float r4 = FootstepParticles::Hash(seed, 4);
	float r5 = FootstepParticles::Hash(seed, 5);
	float r6 = FootstepParticles::Hash(seed, 6);

	float scale = clamp(footstep.Scale, 0.25, 4.0);
	float strength = clamp(footstep.Strength, 0.2, 3.0);
	float angle = r0 * Math::TAU;
	float2 radial = float2(cos(angle), sin(angle));
	float speed = length(footstep.Velocity.xy);
	float2 moveDirection = speed > 5.0 ? footstep.Velocity.xy / speed : footstep.Forward;
	float speedFactor = saturate(speed / 350.0);
	float2 kick = -moveDirection * speedFactor;

	float3 position = footstep.Position + float3(radial * r1 * 5.0 * scale, 1.5 * scale);
	float3 velocity = float3(footstep.Velocity.xy * 0.15, 0.0);
	float3 color = gsGroundColor;
	float size = 1.0;
	float growth = 1.0;
	float lifetime = 1.0;
	float opacity = 1.0;
	uint flags = 0;

	if (type == FootstepParticles::TypeDust) {
		velocity += float3(radial * lerp(15.0, 45.0, r2) * strength + kick * 25.0, lerp(8.0, 30.0, r3) * strength);
		size = lerp(4.0, 7.0, r4) * scale;
		growth = lerp(2.2, 3.4, r5);
		lifetime = lerp(1.1, 2.0, r6);
		opacity = lerp(0.18, 0.32, r3);
		float luminance = Color::RGBToLuminance(gsGroundColor);
		color = lerp(gsGroundColor, luminance.xxx, 0.25) * lerp(1.0, 1.2, r2);
		position.z += 2.0 * scale;
	} else if (type == FootstepParticles::TypeSnow) {
		velocity += float3(radial * lerp(20.0, 55.0, r2) * strength + kick * 30.0, lerp(20.0, 60.0, r3) * strength);
		size = lerp(3.5, 6.0, r4) * scale;
		growth = lerp(1.8, 2.6, r5);
		lifetime = lerp(0.9, 1.6, r6);
		opacity = lerp(0.35, 0.55, r3);
		color = gsSnowColor;
		flags = 0x100u;
		position.z += 2.0 * scale;
	} else if (type == FootstepParticles::TypeClod) {
		bool snowy = r5 < gsSnowWeight;
		velocity += float3(radial * lerp(15.0, 55.0, r2) + kick * lerp(30.0, 90.0, r3), lerp(50.0, 140.0, r4) * strength);
		size = (snowy ? lerp(0.7, 1.8, r6) : lerp(0.5, 1.3, r6)) * scale;
		lifetime = lerp(0.9, 1.6, r1);
		opacity = 1.0;
		color = snowy ? gsSnowColor * lerp(0.9, 1.0, r2) : gsGroundColor * lerp(0.5, 0.8, r2) * lerp(1.0, 0.75, gsWetness);
		flags = snowy ? 0x100u : 0u;
	} else if (type == FootstepParticles::TypeDroplet) {
		velocity += float3(radial * lerp(40.0, 130.0, r2) + kick * 40.0, lerp(90.0, 220.0, r3) * strength);
		position.xy = footstep.Position.xy + radial * lerp(2.0, 8.0, r1) * scale;
		size = lerp(0.35, 0.7, r4) * scale;
		lifetime = lerp(0.35, 0.7, r6);
		opacity = lerp(0.5, 0.8, r5);
		color = gsWaterColor;
	} else {
		velocity += float3(radial * lerp(15.0, 40.0, r2), lerp(15.0, 35.0, r3));
		size = lerp(3.0, 5.0, r4) * scale;
		growth = lerp(1.8, 2.6, r5);
		lifetime = lerp(0.45, 0.8, r6);
		opacity = lerp(0.12, 0.2, r1);
		color = gsWaterColor * 1.1;
	}

	particle.Position = position;
	particle.Age = 0.0;
	particle.Velocity = velocity;
	particle.Lifetime = lifetime * FootstepParticles::LifetimeScale;
	particle.Color = max(color, 0.0);
	particle.Size = size * FootstepParticles::SizeScale;
	particle.Type = type | flags;
	particle.GroundZ = footstep.Position.z;
	particle.Growth = growth;
	particle.Opacity = saturate(opacity * FootstepParticles::Opacity);

	Particles[slot] = particle;
}
