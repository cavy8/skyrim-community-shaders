#ifndef RAINBOWS_COMMON_HLSLI
#define RAINBOWS_COMMON_HLSLI

namespace Rainbows
{
	struct Interpolants
	{
		float4 Position : SV_POSITION;
		// A point on the view ray, kept homogeneous so it interpolates exactly across the screen.
		float4 ViewPoint : TEXCOORD0;
		nointerpolation float3 Eye : TEXCOORD1;
	};
}

#endif
