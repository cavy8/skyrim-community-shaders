#ifndef __HAIR_STRANDS_COMMON_HLSLI__
#define __HAIR_STRANDS_COMMON_HLSLI__

// Buffer layouts shared by the strand skinning compute shader and the strand vertex
// shader. Mirror HairStrands::RestPoint / StrandInfo (StrandGenerator.h) and
// HairStrands::SkinnedPoint (StrandRenderer.h).
namespace HairStrands
{
	struct RestPoint
	{
		float3 Position;  // bind pose, skin space
		float U;
		float3 Normal;
		float V;
		uint Bones01;  // skin-instance bone indices, 16 bits each
		uint Bones23;
		uint Weights;  // four unorm8 weights
		float T;       // arclength fraction, 0 at the root
	};

	struct StrandInfo
	{
		float Length;
		float Random;
		uint Clump;
		float ClumpRandom;
	};

	struct SkinnedPoint
	{
		float3 Position;  // camera-relative, this frame
		float Pad0;
		float3 PreviousPosition;  // relative to the previous frame's camera
		float Pad1;
		float3 Normal;
		float Pad2;
	};
}

#endif  // __HAIR_STRANDS_COMMON_HLSLI__
