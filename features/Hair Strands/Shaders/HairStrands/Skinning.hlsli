#ifndef __HAIR_STRANDS_SKINNING_HLSLI__
#define __HAIR_STRANDS_SKINNING_HLSLI__

// Shared by the strand compute shaders: StrandSim.cs.hlsl (guide simulation) and
// StrandSkin.cs.hlsl (skinning, and following the guides).

#include "HairStrands/Common.hlsli"

#define HAIR_STRANDS_MAX_COLLIDERS 8
// Texels per side of the head field's octahedral map (kHeadFieldSize).
#define HAIR_STRANDS_HEAD_FIELD_SIZE 64
#define HAIR_STRANDS_FLAG_FOLLOW 1       // strands follow their simulated guides
#define HAIR_STRANDS_FLAG_RESET 2        // guides restart from their targets
#define HAIR_STRANDS_FLAG_COLLIDE 4      // guides keep out of the colliders
#define HAIR_STRANDS_FLAG_HEAD_FIELD 8   // every strand keeps out of the head field (t4)
#define HAIR_STRANDS_FLAG_BODY_FIELD 16  // every strand keeps off the body's distance field (t5, t6)
#define HAIR_STRANDS_FLAG_CARD_FIELD 32  // every strand keeps off the kept cards' field (CardField.hlsli)

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
	float BodyMinClearance;  // units: the least a point is kept off the body (where its target lies closer)

	// TressFX's four wind vectors (g_Wind .. g_Wind3), units/s^2 per unit^2 of segment.
	float4 Wind[4];

	// Capsules, camera-relative: (end A, radius), (end B, unused). A sphere has A = B.
	float4 Colliders[HAIR_STRANDS_MAX_COLLIDERS * 2];

	// The body's distance field (BodySdf.cs.hlsl), this frame's: camera-relative position to grid
	// cells (cell centres at +0.5).
	float4 BodyToGrid[3];
	float3 BodyGridSize;     // cells per axis
	float BodyMaxClearance;  // units: how far off the body a point is kept (less where its target lies closer)
	float3 BodyTexel;        // 1 / the field textures' cells per axis
	float BodyTrust;         // units: how much deeper than its target (or the surface) a point is believed to be
	// A camera-relative point's move with the actor's root over the frame (last frame's root pose to
	// this frame's); 0 without last frame's.
	float4 BodyRootMove[3];

	uint ChainBoneBase;  // palette bones from here on are the joints of chains (hanging braids)
	uint3 SkinPad;
};

StructuredBuffer<HairStrands::RestPoint> RestPoints : register(t0);
// BoneCount 3x4 skin-to-world rows (translation relative to this frame's camera),
// then BoneCount rows for the previous frame (relative to the previous frame's camera). The
// skin instance's bones come first, then the joints of any chains (from ChainBoneBase).
StructuredBuffer<float4> Palette : register(t1);
StructuredBuffer<HairStrands::StrandInfo> Strands : register(t2);
// The actor's head mesh, rigid on the head bone: per direction from HeadFieldCentre (an
// octahedral map, HAIR_STRANDS_HEAD_FIELD_SIZE texels a side), the distance in skin units to
// its outermost surface; 0 where the head has none (the neck opening).
StructuredBuffer<float> HeadField : register(t4);
// The body's distance field, built this frame from what the actor wears (BodySdf.cs.hlsl): per cell
// (the surface's move over the frame x W, W) and (signed distance in units x W, outward normal x W).
// W is 1 where the field has a value and 0 elsewhere, so filtered values divided by the filtered W
// are averages of the cells that have one.
Texture3D<float4> BodyMotion : register(t5);
Texture3D<float4> BodySurface : register(t6);
SamplerState BodySampler : register(s0);  // linear, clamped

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
	// its full skinning either way. Hair growing from a hanging braid rides on its chain: by its
	// weight on chain joints, the target is the full skinning whatever Guidance is.
	void TargetSkin(HairStrands::RestPoint a_rest, float3x4 a_current, float3x4 a_previous, out float3x4 o_current, out float3x4 o_previous)
	{
		const uint4 bones = uint4(a_rest.Bones01 & 0xFFFF, a_rest.Bones01 >> 16, a_rest.Bones23 & 0xFFFF, a_rest.Bones23 >> 16);
		const float4 weights = float4(a_rest.Weights & 0xFF, (a_rest.Weights >> 8) & 0xFF, (a_rest.Weights >> 16) & 0xFF, a_rest.Weights >> 24) / 255.0;
		const float chain = dot(weights, bones >= ChainBoneBase.xxxx ? 1.0 : 0.0);
		const float follow = lerp(Guidance, 1.0, saturate(chain));
		o_current = lerp(LoadBone(HeadBone, 0), a_current, follow);
		o_previous = lerp(LoadBone(HeadBone, BoneCount * 3), a_previous, follow);
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

	// Filtered weight a body field sample needs: less is mostly cells the field does not reach.
	static const float MinBodyWeight = 0.25;

	// A camera-relative point's grid cell in the body's distance field.
	float3 BodyCell(float3 a_p)
	{
		return mul(float3x4(BodyToGrid[0], BodyToGrid[1], BodyToGrid[2]), float4(a_p, 1.0));
	}

	// The body's surface near camera-relative point a_p: the signed distance (units, negative inside),
	// the outward normal and the surface's move over the frame. False where the field has no value.
	bool SampleBody(float3 a_p, out float o_distance, out float3 o_normal, out float3 o_move)
	{
		o_distance = 0;
		o_normal = 0;
		o_move = 0;
		const float3 cell = BodyCell(a_p);
		if (any(cell < 0.5) || any(cell > BodyGridSize - 0.5))
			return false;
		const float3 uvw = cell * BodyTexel;
		const float4 motion = BodyMotion.SampleLevel(BodySampler, uvw, 0);
		if (!(motion.w > MinBodyWeight))
			return false;
		const float4 surface = BodySurface.SampleLevel(BodySampler, uvw, 0);
		o_move = motion.xyz / motion.w;
		o_distance = surface.x / motion.w;
		o_normal = SafeNormalize(surface.yzw, 0);
		return any(o_normal != 0);
	}

	// How far the surface near camera-relative point a_p, as it is a_f of the way through the frame,
	// moves by the frame's end, where the field is. The actor's root carries the point first (all of
	// a run or a turn); the surface's own move over the frame is read where that puts it. At a_p
	// itself the field may not reach: the body there is the rest of the frame's move on.
	float3 BodyAhead(float3 a_p, float a_f)
	{
		const float3 root = mul(float3x4(BodyRootMove[0], BodyRootMove[1], BodyRootMove[2]), float4(a_p, 1.0)) * (1.0 - a_f);
		const float3 cell = BodyCell(a_p + root);
		if (any(cell < 0.5) || any(cell > BodyGridSize - 0.5))
			return root;
		const float4 motion = BodyMotion.SampleLevel(BodySampler, cell * BodyTexel, 0);
		return motion.w > MinBodyWeight ? motion.xyz / motion.w * (1.0 - a_f) : root;
	}

	// For a point whose target is a_target: x, how far off the body it is kept, as far as its target
	// lies (its styled shape rests where it is styled) but no less than BodyMinClearance and no more
	// than BodyMaxClearance (a target inside the body or what it wears rests BodyMinClearance outside
	// it); y, the deepest it is believed to lie, BodyTrust below its target or the surface. Deeper,
	// the field is wrong (past a thin part, beyond the reach of its near side) more often than the
	// point is that deep, and pushing it out would throw it through the part.
	float2 BodyLimits(float3 a_target)
	{
		float distance;
		float3 normal, move;
		if (!SampleBody(a_target, distance, normal, move))
			return float2(BodyMaxClearance, -BodyTrust);
		return float2(clamp(distance, BodyMinClearance, BodyMaxClearance), min(distance, 0.0) - BodyTrust);
	}

	// Keeps camera-relative point io_p a_limits.x off the body (BodyLimits), as TressFX's signed
	// distance field collision: a point closer (or inside, but no deeper than a_limits.y) is put back
	// along the surface's normal. The field is this frame's; a_ahead is the surface's move from the
	// point's time to the frame's end (BodyAhead). Returns true on contact, with the distance read
	// there, the normal and the surface's move over the frame.
	bool CollideBody(inout float3 io_p, float3 a_ahead, float2 a_limits, out float o_distance, out float3 o_normal, out float3 o_move)
	{
		if (!SampleBody(io_p + a_ahead, o_distance, o_normal, o_move))
			return false;
		if (!(o_distance < a_limits.x) || o_distance < a_limits.y)
			return false;
		io_p += o_normal * (a_limits.x - o_distance);
		return true;
	}
}

#endif  // __HAIR_STRANDS_SKINNING_HLSLI__
