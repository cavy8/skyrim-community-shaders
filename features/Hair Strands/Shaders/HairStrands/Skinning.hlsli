#ifndef __HAIR_STRANDS_SKINNING_HLSLI__
#define __HAIR_STRANDS_SKINNING_HLSLI__

// Shared by the strand compute shaders: StrandSim.cs.hlsl (guide simulation) and
// StrandSkin.cs.hlsl (skinning, and following the guides).

#include "HairStrands/Common.hlsli"

#define HAIR_STRANDS_MAX_COLLIDERS 8
// Texels per side of the head field's octahedral map (kHeadFieldSize).
#define HAIR_STRANDS_HEAD_FIELD_SIZE 64
// Body colliders (BodyField.h): kBodySlots, and each map's columns round its segment, rows per end
// cap and rows along the segment.
#define HAIR_STRANDS_MAX_BODY_COLLIDERS 8
#define HAIR_STRANDS_BODY_FIELD_COLUMNS 32
#define HAIR_STRANDS_BODY_FIELD_CAP_ROWS 6
#define HAIR_STRANDS_BODY_FIELD_SIDE_ROWS 12
#define HAIR_STRANDS_BODY_FIELD_ROWS (2 * HAIR_STRANDS_BODY_FIELD_CAP_ROWS + HAIR_STRANDS_BODY_FIELD_SIDE_ROWS)

#define HAIR_STRANDS_FLAG_FOLLOW 1      // strands follow their simulated guides
#define HAIR_STRANDS_FLAG_RESET 2       // guides restart from their targets
#define HAIR_STRANDS_FLAG_COLLIDE 4     // guides keep out of the colliders
#define HAIR_STRANDS_FLAG_HEAD_FIELD 8  // every strand keeps out of the head field (t4)
#define HAIR_STRANDS_FLAG_BODY_FIELD 16  // every strand keeps out of the body colliders (t5)

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
	uint BodyColliderCount;

	// TressFX's four wind vectors (g_Wind .. g_Wind3), units/s^2 per unit^2 of segment.
	float4 Wind[4];

	// Capsules, camera-relative: (end A, radius), (end B, unused). A sphere has A = B.
	float4 Colliders[HAIR_STRANDS_MAX_COLLIDERS * 2];

	// Body colliders: per collider, its field-to-camera rows this frame (3), then last frame's,
	// relative to last frame's camera (3). A bone's rotation and uniform scale after the field's axes.
	float4 BodyFrames[HAIR_STRANDS_MAX_BODY_COLLIDERS * 6];
	// Per body collider: its segment's length and the largest radius in its map (field units), and
	// which map is its (asuint).
	float4 BodyShapes[HAIR_STRANDS_MAX_BODY_COLLIDERS];
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
// The body colliders' maps, built from the actor's worn meshes (BodyField.h): per collider, rows
// (from the pole of the cap at the segment's start, along its side, to the pole of the cap at its
// end) of columns (round it) of (radius, its slope along the column, along the row). A radius is
// the distance from the segment to the outermost surface in that direction; 0 where there is none.
StructuredBuffer<float3> BodyField : register(t5);

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

	// A styled shape up to this deep inside a body collider (field units) rests as styled; deeper, it
	// rests at this depth. The colliders sit a margin (kBodyFieldMargin) outside the body, so hair
	// styled on the bare body stays put and hair styled into armour lies on it.
	static const float BodyRestDepth = 0.25;

	// A body collider's field space, where its segment runs from the origin along +Z.
	struct BodyFrame
	{
		float3x3 toCamera;
		float3x3 toField;
		float3 origin;
	};

	BodyFrame MakeBodyFrame(float3x4 a_rows)
	{
		BodyFrame frame;
		frame.toCamera = (float3x3)a_rows;
		// A rotation times the bone's uniform scale: the inverse is the transpose over the scale squared.
		frame.toField = transpose(frame.toCamera) / max(dot(frame.toCamera[0], frame.toCamera[0]), 1e-12);
		frame.origin = float3(a_rows[0].w, a_rows[1].w, a_rows[2].w);
		return frame;
	}

	// Body collider a_collider's frame this frame (a_previous 0) or last (1).
	float3x4 LoadBodyRows(uint a_collider, uint a_previous)
	{
		const uint row = a_collider * 6 + a_previous * 3;
		return float3x4(BodyFrames[row], BodyFrames[row + 1], BodyFrames[row + 2]);
	}

	// Continuous (column, row) of field-space point a_q round the segment (0, 0, 0)-(0, 0, a_length)
	// (BodyFieldTexel in BodyField.cpp).
	float2 BodyFieldTexel(float3 a_q, float a_length)
	{
		const float capScale = HAIR_STRANDS_BODY_FIELD_CAP_ROWS / 1.57079633;
		const float rho = length(a_q.xy);
		float row;
		if (a_q.z < 0.0)
			row = atan2(rho, -a_q.z) * capScale;
		else if (a_q.z > a_length)
			row = HAIR_STRANDS_BODY_FIELD_ROWS - atan2(rho, a_q.z - a_length) * capScale;
		else
			row = HAIR_STRANDS_BODY_FIELD_CAP_ROWS + HAIR_STRANDS_BODY_FIELD_SIDE_ROWS * a_q.z / max(a_length, 1e-4);
		return float2((atan2(a_q.y, a_q.x) * 0.159154943 + 1.0) * HAIR_STRANDS_BODY_FIELD_COLUMNS, row);
	}

	// A body collider's radius in the direction of field-space point a_q, and its slopes along the
	// column and the row: bilinear, wrapping round the segment.
	float3 BodySurface(uint a_map, float3 a_q, float a_length)
	{
		const float2 texel = BodyFieldTexel(a_q, a_length) - 0.5;
		const float2 base = floor(texel);
		const float2 f = texel - base;
		const uint c0 = (uint)base.x % HAIR_STRANDS_BODY_FIELD_COLUMNS;
		const uint c1 = (c0 + 1) % HAIR_STRANDS_BODY_FIELD_COLUMNS;
		const uint r0 = (uint)clamp((int)base.y, 0, HAIR_STRANDS_BODY_FIELD_ROWS - 1);
		const uint r1 = (uint)clamp((int)base.y + 1, 0, HAIR_STRANDS_BODY_FIELD_ROWS - 1);
		const uint start = a_map * HAIR_STRANDS_BODY_FIELD_ROWS * HAIR_STRANDS_BODY_FIELD_COLUMNS;
		const float3 top = lerp(BodyField[start + r0 * HAIR_STRANDS_BODY_FIELD_COLUMNS + c0], BodyField[start + r0 * HAIR_STRANDS_BODY_FIELD_COLUMNS + c1], f.x);
		const float3 bottom = lerp(BodyField[start + r1 * HAIR_STRANDS_BODY_FIELD_COLUMNS + c0], BodyField[start + r1 * HAIR_STRANDS_BODY_FIELD_COLUMNS + c1], f.x);
		return lerp(top, bottom, f.y);
	}

	// How far field-space point a_q lies inside body collider a_collider's surface less a_rest, to
	// first order: along the surface's normal, the radial depth over the gradient's length (a
	// surface nearly along the radius, such as the top of the shoulders round the spine, is pushed
	// up, not sideways). o_normal: the outward normal (field space); o_radial: the radius less the
	// point's distance from the segment (0 when the collider does not reach the point).
	float BodyDepth(uint a_collider, float3 a_q, float a_rest, out float3 o_normal, out float o_radial)
	{
		o_normal = 0;
		o_radial = 0;
		const float4 shape = BodyShapes[a_collider];
		const float segmentLength = shape.x;
		const float radius = distance(a_q, float3(0, 0, clamp(a_q.z, 0.0, segmentLength)));
		if (radius >= shape.y)
			return 0.0;
		const float3 surface = BodySurface(asuint(shape.z), a_q, segmentLength);
		if (!(surface.x > 0.0))
			return 0.0;
		o_radial = surface.x - radius;

		float sinTheta, cosTheta;
		sincos(atan2(a_q.y, a_q.x), sinTheta, cosTheta);
		const float3 around = float3(-sinTheta, cosTheta, 0.0);
		const float slopeTheta = surface.y * (HAIR_STRANDS_BODY_FIELD_COLUMNS * 0.159154943);
		float3 gradient;
		if (a_q.z >= 0.0 && a_q.z <= segmentLength) {
			// The side: the radius round the segment and along it.
			gradient = float3(cosTheta, sinTheta, 0.0) - around * (slopeTheta / surface.x) - float3(0.0, 0.0, surface.z * HAIR_STRANDS_BODY_FIELD_SIDE_ROWS / max(segmentLength, 1e-4));
		} else {
			// A cap: the radius from the segment's end, by the angle from the cap's pole.
			const bool atStart = a_q.z < 0.0;
			const float pole = atStart ? -1.0 : 1.0;
			float sinPhi, cosPhi;
			sincos(atan2(length(a_q.xy), atStart ? -a_q.z : a_q.z - segmentLength), sinPhi, cosPhi);
			const float slopePhi = surface.z * (HAIR_STRANDS_BODY_FIELD_CAP_ROWS / 1.57079633) * (atStart ? 1.0 : -1.0);
			const float3 outward = float3(sinPhi * cosTheta, sinPhi * sinTheta, pole * cosPhi);
			const float3 fromPole = float3(cosPhi * cosTheta, cosPhi * sinTheta, -pole * sinPhi);
			gradient = outward - fromPole * (slopePhi / surface.x) - around * (slopeTheta / (surface.x * max(sinPhi, 0.25)));
		}
		const float gradientLength = max(length(gradient), 1e-6);
		o_normal = gradient / gradientLength;
		return (surface.x - a_rest - radius) / gradientLength;
	}

	// Keeps camera-relative point io_p out of body collider a_collider on frame a_frame, as TressFX's
	// signed distance field collision: a point inside is put back on the surface along its normal.
	// a_target is its target: a styled shape up to BodyRestDepth inside rests as styled; deeper, at
	// that depth. Returns true if the point moved; then o_q is where it went in field space and
	// o_normal the surface's normal there (camera space).
	bool CollideBody(uint a_collider, BodyFrame a_frame, inout float3 io_p, float3 a_target, out float3 o_q, out float3 o_normal)
	{
		o_q = mul(a_frame.toField, io_p - a_frame.origin);
		o_normal = 0;
		const float4 shape = BodyShapes[a_collider];
		if (distance(o_q, float3(0, 0, clamp(o_q.z, 0.0, shape.x))) >= shape.y)
			return false;
		float3 normal;
		float targetRadial;
		BodyDepth(a_collider, mul(a_frame.toField, a_target - a_frame.origin), 0.0, normal, targetRadial);
		float radial;
		const float depth = BodyDepth(a_collider, o_q, min(max(targetRadial, 0.0), BodyRestDepth), normal, radial);
		if (!(depth > 0.0))
			return false;
		o_q += normal * depth;
		o_normal = normalize(mul(a_frame.toCamera, normal));
		io_p = a_frame.origin + mul(a_frame.toCamera, o_q);
		return true;
	}
}

#endif  // __HAIR_STRANDS_SKINNING_HLSLI__
