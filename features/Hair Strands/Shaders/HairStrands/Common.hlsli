#ifndef __HAIR_STRANDS_COMMON_HLSLI__
#define __HAIR_STRANDS_COMMON_HLSLI__

// Buffer layouts shared by the strand compute shaders and the strand vertex shader.
// Mirror Strands::RestPoint / StrandInfo (StrandGenerator.h) and Strands::SkinnedPoint /
// GuidePoint (StrandRenderer.h).
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
		uint Guide;  // the simulated strand this one follows (itself for a guide)
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

	// One simulated guide point. Positions are camera-relative: current ones to the camera of
	// the simulation that wrote them, previous ones to the previous frame's camera.
	struct GuidePoint
	{
		float4 Rotation;  // shortest arc from the target's tangent to the simulated tangent
		float3 Position;
		float Pad0;
		float3 Velocity;  // units per second
		float Pad1;
		float3 Target;  // where skinning alone puts the point
		float Pad2;
		float3 PreviousPosition;
		float Pad3;
		float3 PreviousTarget;
		float Pad4;
	};
}

#endif  // __HAIR_STRANDS_COMMON_HLSLI__
