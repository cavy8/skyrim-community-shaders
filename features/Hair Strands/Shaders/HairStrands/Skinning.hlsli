#ifndef __HAIR_STRANDS_SKINNING_HLSLI__
#define __HAIR_STRANDS_SKINNING_HLSLI__

// Shared by the strand compute shaders: StrandSim.cs.hlsl (guide simulation) and
// StrandSkin.cs.hlsl (skinning, and following the guides).

#include "HairStrands/Common.hlsli"

#define HAIR_STRANDS_MAX_COLLIDERS 8
// Texels per side of the head field's octahedral map (kHeadFieldSize).
#define HAIR_STRANDS_HEAD_FIELD_SIZE 64

#define HAIR_STRANDS_FLAG_FOLLOW 1      // strands follow their simulated guides
#define HAIR_STRANDS_FLAG_RESET 2       // guides restart from their targets
#define HAIR_STRANDS_FLAG_COLLIDE 4     // guides keep out of the colliders
#define HAIR_STRANDS_FLAG_HEAD_FIELD 8  // every strand keeps out of the head field (t4)

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

	float3 EyeShift;  // camera of the stored simulation state - this frame's camera
	uint Steps;       // simulation steps ending in this frame (0 while paused)

	float3 PreviousToCurrent;  // the previous frame's camera - this frame's camera
	float FirstStep;           // fraction of the frame at which the first step ends

	float StepFraction;      // fraction of the frame one step lasts
	float DisplayAlpha;      // time since the last step, in steps: how far to draw towards it
	float StepTime;          // seconds per step
	float TeleportDistance;  // a root jumping further than this in a frame restarts its strand

	// TressFX's simulation settings (TressFXSimulationSettings), in units and per step.
	float Damping;
	float LocalStiffness;
	float GlobalStiffness;
	float GlobalRange;

	float Gravity;  // units/s^2, down
	float VspCoeff;
	float VspAccelThreshold;   // units per step^2
	float ClampPositionDelta;  // units per step

	uint LocalIterations;
	uint LengthIterations;
	float TipSeparation;
	uint ColliderCount;

	float3 HeadFieldCentre;  // skin space: where the head field's directions start
	float HeadFieldPad;

	// TressFX's four wind vectors (g_Wind .. g_Wind3), units/s^2 per unit^2 of segment.
	float4 Wind[4];

	// Capsules, camera-relative: (end A, radius), (end B, unused). A sphere has A = B.
	float4 Colliders[HAIR_STRANDS_MAX_COLLIDERS * 2];
};

StructuredBuffer<HairStrands::RestPoint> RestPoints : register(t0);
// BoneCount 3x4 skin-to-world rows (translation relative to this frame's camera),
// then BoneCount rows for the previous frame (relative to the previous frame's camera).
StructuredBuffer<float4> Palette : register(t1);
StructuredBuffer<HairStrands::StrandInfo> Strands : register(t2);
// The actor's head mesh, rigid on the head bone: per direction from HeadFieldCentre (an
// octahedral map, HAIR_STRANDS_HEAD_FIELD_SIZE texels a side), the distance in skin units to
// its outermost surface; 0 where the head has none (the neck opening).
StructuredBuffer<float> HeadField : register(t4);

namespace HairStrandsSkin
{
	// A point is never pushed deeper than its target already lies inside a collider, and never
	// left deeper than this fraction of the collider's radius: the styled shape never collides.
	static const float MinColliderDepth = 0.5;

	float3x3 Inverse(float3x3 a_m)
	{
		const float3 c0 = cross(a_m[1], a_m[2]);
		const float3 c1 = cross(a_m[2], a_m[0]);
		const float3 c2 = cross(a_m[0], a_m[1]);
		const float det = dot(a_m[0], c0);
		if (abs(det) < 1e-12)
			return float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
		return transpose(float3x3(c0, c1, c2)) / det;
	}
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

	// Octahedral map of a unit direction to [0, 1]^2 (StrandRenderer's OctahedralUV).
	float2 HeadFieldUV(float3 a_direction)
	{
		const float3 d = a_direction / max(abs(a_direction.x) + abs(a_direction.y) + abs(a_direction.z), 1e-8);
		float2 p = d.xy;
		if (d.z < 0.0)
			p = (1.0 - abs(d.yx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
		return p * 0.5 + 0.5;
	}

	// Distance from HeadFieldCentre to the head's surface along a unit direction (skin units).
	float HeadSurface(float3 a_direction)
	{
		const int size = HAIR_STRANDS_HEAD_FIELD_SIZE;
		const float2 texel = HeadFieldUV(a_direction) * size - 0.5;
		const float2 base = floor(texel);
		const float2 f = texel - base;
		const int2 a = clamp(int2(base), 0, size - 1);
		const int2 b = clamp(int2(base) + 1, 0, size - 1);
		const float top = lerp(HeadField[a.y * size + a.x], HeadField[a.y * size + b.x], f.x);
		const float bottom = lerp(HeadField[b.y * size + a.x], HeadField[b.y * size + b.x], f.x);
		return lerp(top, bottom, f.y);
	}

	// The head bone's frame, which the head field is rigid in: skin space to camera-relative and back.
	struct HeadFrame
	{
		float3x3 fromSkin;
		float3x3 toSkin;
		float3 origin;
	};

	HeadFrame LoadHeadFrame(uint a_base)
	{
		const float3x4 head = LoadBone(HeadBone, a_base);
		HeadFrame frame;
		frame.fromSkin = (float3x3)head;
		frame.toSkin = Inverse(frame.fromSkin);
		frame.origin = float3(head[0].w, head[1].w, head[2].w);
		return frame;
	}

	// How far a camera-relative point lies inside the head (skin units, 0 outside).
	float HeadDepth(float3 a_p, HeadFrame a_head)
	{
		const float3 q = mul(a_head.toSkin, a_p - a_head.origin) - HeadFieldCentre;
		const float distance = length(q);
		return distance > 1e-6 ? max(HeadSurface(q / distance) - distance, 0.0) : 0.0;
	}

	// Pushes a camera-relative point out of the head along the direction from the field's centre,
	// to no less depth than a_targetDepth (its target's, from HeadDepth).
	float3 CollideHead(float3 a_p, float a_targetDepth, HeadFrame a_head)
	{
		const float3 q = mul(a_head.toSkin, a_p - a_head.origin) - HeadFieldCentre;
		const float distance = length(q);
		const float3 direction = SafeNormalize(q, float3(0, 0, 1));
		const float surface = HeadSurface(direction);
		const float allowed = max(surface - a_targetDepth, surface * MinColliderDepth);
		if (!(surface > 0.0) || distance >= allowed)
			return a_p;
		return a_head.origin + mul(a_head.fromSkin, HeadFieldCentre + direction * allowed);
	}
}

#endif  // __HAIR_STRANDS_SKINNING_HLSLI__
