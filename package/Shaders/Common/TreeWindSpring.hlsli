#ifndef __TREE_WIND_SPRING_DEPENDENCY_HLSL__
#define __TREE_WIND_SPRING_DEPENDENCY_HLSL__

#include "Common/DampedSpring.hlsli"

namespace TreeWindSpring
{
	static const uint FieldCount = 3u;
	static const uint TransientHeightCount = 3u;

	struct FieldData
	{
		float2 FieldMinimum;
		float2 PreviousFieldMinimum;
		float FieldHeight;
		float FrameTime;
		float FieldSize;
		uint TextureSize;
		uint Initialize;
		uint FieldAvailable;
		float MaxDistance;
		float PreviousFieldHeight;
	};

#if defined(TREE_WIND_SPRING_COMPUTE)
	cbuffer SpringField : register(b0)
#else
	cbuffer SpringField : register(b3)
#endif
	{
		FieldData Fields[FieldCount];
		uint ActiveField;
		float SpringFrequency;
		float SpringDamping;
		float GustScale;
		float GustSoftLimit;
		uint TransientFieldMask;
		float TransientSpringFrequency;
		float TransientSpringDamping;
	};

	float GetTransientHeightHalfRange(uint fieldIndex)
	{
		return clamp(Fields[fieldIndex].MaxDistance * 0.0625f, 1024.0f, 4096.0f);
	}

#if !defined(TREE_WIND_SPRING_COMPUTE)
	// The GPU-cached local spring-field response (per-cell textures built by TreeWindSpringCS)
	// isn't wired into any vertex shader yet -- see the Wind tracking-doc entry. Registers t111+
	// and s14 also collide with Lighting.hlsl's existing SampShadowMaskSampler (s14), so this
	// falls back to the same "plain ambient field" response every caller already treats as the
	// out-of-cache case, rather than declaring unbound texture/sampler resources here.

	/** @brief Structural response and immediate leaf velocity; always empty until the spring-field cache is wired in. */
	bool TrySampleCurrentTransient(float3 worldPosition, out float4 transientSample)
	{
		transientSample = 0.0f.xxxx;
		return false;
	}

	/** @brief Structural response and immediate leaf velocity; always empty until the spring-field cache is wired in. */
	bool TrySamplePreviousTransient(float3 worldPosition, out float4 transientSample)
	{
		transientSample = 0.0f.xxxx;
		return false;
	}

	/** @brief Plain ambient wind velocity at this world position; the spring-field cache is not wired in. */
	float2 SampleCurrent(float2 worldPosition)
	{
		return SharedData::WindFieldAmbient.xy;
	}

	/** @brief Plain previous ambient wind velocity at this world position; the spring-field cache is not wired in. */
	float2 SamplePrevious(float2 worldPosition)
	{
		return SharedData::WindFieldPreviousAmbient.xy;
	}
#endif
}

#endif  // __TREE_WIND_SPRING_DEPENDENCY_HLSL__
