#ifndef __FOOTSTEP_PARTICLES_COMMON_HLSLI__
#define __FOOTSTEP_PARTICLES_COMMON_HLSLI__

namespace FootstepParticles
{
	static const uint SlotsPerEvent = 64;

	static const uint TypeDust = 0;
	static const uint TypeSnow = 1;
	static const uint TypeClod = 2;
	static const uint TypeDroplet = 3;
	static const uint TypeMist = 4;
	static const uint TypeCount = 5;

	static const uint SurfaceHard = 0;
	static const uint SurfaceDirt = 1;
	static const uint SurfaceGravel = 2;
	static const uint SurfaceSand = 3;
	static const uint SurfaceGrass = 4;
	static const uint SurfaceMud = 5;
	static const uint SurfaceSnow = 6;
	static const uint SurfaceIce = 7;
	static const uint SurfaceAsh = 8;
	static const uint SurfaceWater = 9;

	static const uint FrameFlagWetness = 1u << 0;
	static const uint FrameFlagSnowCover = 1u << 1;
	static const uint FrameFlagFog = 1u << 2;
	static const uint FrameFlagInterior = 1u << 3;

	static const uint EventFlagLanding = 1u << 0;

	struct Particle
	{
		float3 Position;
		float Age;
		float3 Velocity;
		float Lifetime;
		float3 Color;
		float Size;
		uint Type;
		float GroundZ;
		float Growth;
		float Opacity;
	};

	struct FootstepEvent
	{
		float3 Position;
		uint FirstSlot;
		float3 Velocity;
		uint Surface;
		float2 Forward;
		float Strength;
		float Scale;
		uint Seed;
		uint Flags;
		float2 Pad;
	};

	cbuffer FootstepFrame : register(b0)
	{
		float4 FogNearColor;
		float4 FogFarColor;
		float4 FogParams;
		float DeltaTime;
		float Gravity;
		uint EventCount;
		uint MaxParticles;
		float DustIntensity;
		float SnowIntensity;
		float SplashIntensity;
		float SizeScale;
		float LifetimeScale;
		float Opacity;
		float SoftDistance;
		uint FrameFlags;
		float2 Wind;
		float Time;
		float FramePad;
	};

	float Hash(uint seed)
	{
		seed ^= seed >> 16;
		seed *= 0x7feb352du;
		seed ^= seed >> 15;
		seed *= 0x846ca68bu;
		seed ^= seed >> 16;
		return float(seed) * (1.0 / 4294967296.0);
	}

	float Hash(uint seed, uint salt)
	{
		return Hash(seed * 0x9E3779B9u + salt * 0x85EBCA6Bu + 0x68E31DA4u);
	}
}

#endif
