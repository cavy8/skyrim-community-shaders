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

	// One simulated guide point. Positions are relative to the camera of the simulation that
	// wrote them; offsets are from the point's target (its skinned rest position) and so hold
	// for any camera.
	struct GuidePoint
	{
		float4 Rotation;  // shortest arc from the target's tangent to the drawn tangent
		float3 Position;  // TressFX's g_HairVertexPositions: at the last step
		float Pad0;
		float3 PreviousPosition;  // g_HairVertexPositionsPrev: at the step before (Verlet history)
		float Pad1;
		float3 PreviousPreviousPosition;  // g_HairVertexPositionsPrevPrev (read for the second point)
		float Pad2;
		float3 StepOffset;  // Position - target at the last step
		float Pad3;
		float3 PreviousStepOffset;  // the same at the step before
		float Pad4;
		float3 Offset;  // drawn this frame: the step offsets interpolated to the frame
		float Pad5;
		float3 PreviousOffset;  // drawn last frame (motion vectors)
		float Pad6;
	};
}

#endif  // __HAIR_STRANDS_COMMON_HLSLI__
