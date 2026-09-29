#ifndef __HAIR_STRANDS_SKINNING_HLSLI__
#define __HAIR_STRANDS_SKINNING_HLSLI__

// Shared by the strand compute shaders: StrandSim.cs.hlsl (guide simulation) and
// StrandSkin.cs.hlsl (skinning, and following the guides).

#include "HairStrands/Common.hlsli"

#define HAIR_STRANDS_MAX_COLLIDERS 8

#define HAIR_STRANDS_FLAG_FOLLOW 1   // strands follow their simulated guides
#define HAIR_STRANDS_FLAG_RESET 2    // guides restart from their targets
#define HAIR_STRANDS_FLAG_COLLIDE 4  // guides keep out of the colliders

// Mirrors Strands::SkinCB (StrandRenderer.h).
cbuffer SkinCB : register(b0)
{
	uint PointCount;  // points skinned (the drawn strands)
	uint BoneCount;
	uint PointsPerStrand;
	uint GuideCount;

	uint HeadBone;  // skin-instance bone the head is
	uint Flags;
	float SimWeight;  // 0: skinning only, 1: fully simulated
	float Guidance;   // 0: targets ride the head rigidly, 1: every bone (SMP) moves them

	float3 Gravity;  // units/s^2: the part of gravity the styled shape does not already hang under
	float StepTime;  // seconds per step; Steps steps make up the frame

	float3 EyeShift;          // camera of the stored simulation state - this frame's camera
	float RootStiffness;      // pull towards the target per 1/60 s, at the root
	float3 PreviousEyeShift;  // camera of the stored simulation state - the previous frame's camera
	float TipStiffness;

	float3 Wind;          // units/s^2 at the tip, before gusts
	float BendStiffness;  // per 1/60 s

	float VelocityKeep;  // fraction of velocity left after a step's air drag
	float Carry;         // fraction of the bones' motion the state moves with directly (1 - inertia)
	float Time;
	float TeleportDistance;  // a root jumping further than this in a frame restarts its strand

	uint Iterations;  // constraint iterations per step
	uint ColliderCount;
	uint Steps;          // 0 while paused
	float SwingDamping;  // velocity relative to the target lost per 1/60 s

	// Capsules, camera-relative: (end A, radius), (end B, unused). A sphere has A = B.
	float4 Colliders[HAIR_STRANDS_MAX_COLLIDERS * 2];
};

StructuredBuffer<HairStrands::RestPoint> RestPoints : register(t0);
// BoneCount 3x4 skin-to-world rows (translation relative to this frame's camera),
// then BoneCount rows for the previous frame (relative to the previous frame's camera).
StructuredBuffer<float4> Palette : register(t1);
StructuredBuffer<HairStrands::StrandInfo> Strands : register(t2);

namespace HairStrandsSkin
{
	float3x4 LoadBone(uint a_bone, uint a_base)
	{
		uint row = a_base + a_bone * 3;
		return float3x4(Palette[row], Palette[row + 1], Palette[row + 2]);
	}

	// Skin-to-camera-relative transforms of a point, this frame and last, as the game skins the cards.
	void Skin(HairStrands::RestPoint a_rest, out float3x4 o_current, out float3x4 o_previous)
	{
		uint4 bones = uint4(a_rest.Bones01 & 0xFFFF, a_rest.Bones01 >> 16, a_rest.Bones23 & 0xFFFF, a_rest.Bones23 >> 16);
		bones = min(bones, (BoneCount - 1).xxxx);
		float4 weights = float4(a_rest.Weights & 0xFF, (a_rest.Weights >> 8) & 0xFF, (a_rest.Weights >> 16) & 0xFF, a_rest.Weights >> 24) / 255.0;

		o_current = 0;
		o_previous = 0;
		[unroll] for (uint i = 0; i < 4; ++i)
		{
			o_current += LoadBone(bones[i], 0) * weights[i];
			o_previous += LoadBone(bones[i], BoneCount * 3) * weights[i];
		}
	}

	// The simulation's target: the styled shape carried by the head alone, moved towards the
	// full skinning (SMP and any other bones) by Guidance. Hair skinned to the head only gets
	// its full skinning either way.
	void TargetSkin(float3x4 a_current, float3x4 a_previous, out float3x4 o_current, out float3x4 o_previous)
	{
		o_current = lerp(LoadBone(HeadBone, 0), a_current, Guidance);
		o_previous = lerp(LoadBone(HeadBone, BoneCount * 3), a_previous, Guidance);
	}

	float3 Rotate(float4 a_q, float3 a_v)
	{
		return a_v + 2.0 * cross(a_q.xyz, cross(a_q.xyz, a_v) + a_q.w * a_v);
	}

	// The shortest rotation taking unit vector a_from to unit vector a_to.
	float4 ShortestArc(float3 a_from, float3 a_to)
	{
		const float d = dot(a_from, a_to);
		if (d < -0.9999) {
			const float3 axis = abs(a_from.x) < 0.9 ? cross(a_from, float3(1, 0, 0)) : cross(a_from, float3(0, 1, 0));
			return float4(normalize(axis), 0.0);
		}
		return normalize(float4(cross(a_from, a_to), 1.0 + d));
	}

	float3 SafeNormalize(float3 a_v, float3 a_fallback)
	{
		const float lengthSquared = dot(a_v, a_v);
		return lengthSquared > 1e-12 ? a_v * rsqrt(lengthSquared) : a_fallback;
	}
}

#endif  // __HAIR_STRANDS_SKINNING_HLSLI__
