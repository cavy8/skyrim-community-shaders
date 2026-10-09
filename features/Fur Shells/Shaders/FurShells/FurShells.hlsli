#ifndef __FUR_SHELLS_HLSLI__
#define __FUR_SHELLS_HLSLI__

namespace FurShells
{
	cbuffer FurShellData : register(b13)
	{
		float Length;
		float ShellCount;
		float Droop;
		float RootThreshold;
		float TipThreshold;
		float RootDarkening;
		float ShellColor;
		float RootTest;
	};

#if defined(PSHADER)
	Texture2D<float4> TexShell : register(t122);
	Texture2D<float4> TexDepth : register(t123);

	bool IsRootCovered(float4 rootPosition, float rootSlope)
	{
		float2 uv = rootPosition.xy / rootPosition.w * float2(0.5, -0.5) + 0.5;
		float sceneDepth = SharedData::GetScreenDepth(TexDepth.Load(SharedData::ConvertUVToSampleCoord(uv)).x);
		float bias = 0.03 + rootSlope + 2e-8 * rootPosition.w * rootPosition.w;
		return RootTest > 0.0 && rootPosition.w > 0.0 && all(uv == saturate(uv)) && sceneDepth < rootPosition.w - bias;
	}
#endif

#if defined(VSHADER) && defined(MODELSPACENORMALS)
	Texture2D<float4> TexModelNormal : register(t122);

	float3 GetModelNormal(float2 uv)
	{
		uint width, height, levels;
		TexModelNormal.GetDimensions(0, width, height, levels);
		uint mip = min(uint(max(int(firstbithigh(max(width, height))), 9) - 9), max(levels, 1u) - 1u);
		uint2 size = max(uint2(width, height) >> mip, 1u);
		uint2 texel = min(uint2(frac(uv) * float2(size)), size - 1u);
		float3 normal = TexModelNormal.Load(int3(texel, mip)).xzy * 2.0 - 1.0;
		return normal * rsqrt(max(dot(normal, normal), 1e-6));
	}
#endif

	float GetShell(uint instanceID)
	{
		return saturate((ShellCount - float(instanceID)) / max(ShellCount, 1.0));
	}
}

#endif  // __FUR_SHELLS_HLSLI__
