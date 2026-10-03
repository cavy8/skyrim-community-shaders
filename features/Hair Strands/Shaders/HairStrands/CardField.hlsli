#ifndef __HAIR_STRANDS_CARD_FIELD_HLSLI__
#define __HAIR_STRANDS_CARD_FIELD_HLSLI__

// The cards a hair keeps (braids, ties, buns, the hair gathered into them) as a distance field,
// built every frame by BodySdf.cs.hlsl's kernels from the cards skinned with the hair's own palette
// (CardField.cpp), so a braid on a chain is where it swings to. A card is a sheet with nothing
// behind it: each cell holds the distance to its nearest card (never negative) and the direction
// away from it on the cell's side.
//
// Linear filtering would blur that at the card itself: the cells either side point opposite ways,
// so their mean distance never reaches 0 and their mean direction vanishes there. Each cell's value
// is instead carried to the point along the cell's own plane (its nearest card, flat at that
// distance), the cells beyond the card turned to face the same way as the nearest cell, and those
// weighed as linear filtering would. The result goes through 0 at the card and changes sign across
// it, so a point reads which side of the card it is on, however close.

#include "HairStrands/Skinning.hlsli"

// Mirrors Strands::CardFieldCB (CardField.h).
cbuffer CardFieldCB : register(b1)
{
	float4 CardToGrid[3];    // camera-relative position to grid cells (cell centres at +0.5)
	float3 CardGridSize;     // cells per axis
	float CardMinClearance;  // units: the least a point is kept off the cards (where its target lies closer)
	float CardMaxClearance;  // units: how far off the cards a point is kept (less where its target lies closer)
	float CardTrust;         // units: how far from the cards the field reaches
	float CardCellSize;      // units
	float CardPad;
};

// As BodyMotion and BodySurface (Skinning.hlsli), from BodySdf.cs.hlsl's Finalize: per cell (the
// cards' move over the frame, W) and (distance in units, direction away from the nearest card);
// W is 1 where the field has a value and 0 elsewhere.
Texture3D<float4> CardMotion : register(t7);
Texture3D<float4> CardSurface : register(t8);

namespace HairStrandsCards
{
	// The cards near camera-relative point a_p: the distance to them (units), the direction away from
	// them on the point's side, and their move over the frame. False where the field has no value.
	bool SampleCards(float3 a_p, out float o_distance, out float3 o_normal, out float3 o_move)
	{
		o_distance = 0;
		o_normal = 0;
		o_move = 0;
		const float3x4 toGrid = float3x4(CardToGrid[0], CardToGrid[1], CardToGrid[2]);
		const float3 cell = mul(toGrid, float4(a_p, 1.0));
		if (any(cell < 0.5) || any(cell > CardGridSize - 0.5))
			return false;
		const float3 centred = cell - 0.5;
		const int3 base = (int3)floor(centred);
		const float3 f = centred - (float3)base;
		const int3 last = (int3)CardGridSize - 1;

		// The cell weighing most that has a value: the others are turned to face its way. Two passes
		// over the eight cells rather than arrays of them, which fxc keeps in indexable memory.
		float best = 0.0;
		float3 facing = 0;
		uint k;
		[unroll] for (k = 0; k < 8; ++k)
		{
			const int3 corner = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
			const int3 c = min(base + corner, last);
			const float3 t = corner != 0 ? f : 1.0 - f;
			const float weight = t.x * t.y * t.z * saturate(CardMotion.Load(int4(c, 0)).w);
			if (weight > best) {
				best = weight;
				facing = CardSurface.Load(int4(c, 0)).yzw;
			}
		}
		if (!(best > 0.0))
			return false;

		// Cell offsets to units: the grid's axes are the head's, scaled by 1 / CardCellSize.
		const float3x3 axes = (float3x3)toGrid;
		const float toUnits = CardCellSize * CardCellSize;
		float total = 0.0;
		float side = 0.0;  // the point's distance along facing from the cards
		float3 normal = 0;
		float3 move = 0;
		[unroll] for (k = 0; k < 8; ++k)
		{
			const int3 corner = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
			const int3 c = min(base + corner, last);
			const float3 t = corner != 0 ? f : 1.0 - f;
			const float4 motion = CardMotion.Load(int4(c, 0));
			const float weight = t.x * t.y * t.z * saturate(motion.w);
			const float4 surface = CardSurface.Load(int4(c, 0));
			const float3 offset = mul(cell - ((float3)(base + corner) + 0.5), axes) * toUnits;
			const float turn = dot(surface.yzw, facing) < 0.0 ? -weight : weight;
			total += weight;
			side += turn * (surface.x + dot(offset, surface.yzw));
			normal += turn * surface.yzw;
			move += weight * motion.xyz;
		}
		if (!(total > HairStrandsSkin::MinBodyWeight))
			return false;
		side /= total;
		o_move = move / total;
		o_distance = abs(side);
		o_normal = HairStrandsSkin::SafeNormalize(normal, facing) * (side < 0.0 ? -1.0 : 1.0);
		return any(o_normal != 0);
	}

	// How far the cards near camera-relative point a_p, as they are a_f of the way through the frame,
	// move by the frame's end, less a snap already carried; nothing where the field has no value.
	float3 CardsAhead(float3 a_p, float a_f, float3 a_snapMove = (float3)0)
	{
		float distance;
		float3 normal, move;
		return SampleCards(a_p, distance, normal, move) ? (move - a_snapMove) * (1.0 - a_f) : (float3)0;
	}

	// How far off the cards a point whose target is a_target is kept: as far as its target lies (a
	// styled shape lying on a bun rests where it is styled), but no less than CardMinClearance and no
	// more than CardMaxClearance.
	float CardClearance(float3 a_target)
	{
		float distance;
		float3 normal, move;
		if (!SampleCards(a_target, distance, normal, move))
			return CardMaxClearance;
		return clamp(distance, CardMinClearance, CardMaxClearance);
	}

	// Keeps camera-relative point io_p a_clearance off the cards: a point closer is put back along
	// the direction away from them. The field is this frame's; a_ahead is the cards' move from the
	// point's time to the frame's end (CardsAhead). Returns true on contact, with the distance read
	// there, the direction and the cards' move over the frame.
	bool CollideCards(inout float3 io_p, float3 a_ahead, float a_clearance, out float o_distance, out float3 o_normal, out float3 o_move)
	{
		if (!SampleCards(io_p + a_ahead, o_distance, o_normal, o_move))
			return false;
		if (!(o_distance < a_clearance))
			return false;
		io_p += o_normal * (a_clearance - o_distance);
		return true;
	}

	// True if a point that ended a step at a_end, a_endNormal being the direction away from the card
	// it is near, came there through a card from a_start (where it read a_startDistance and
	// a_startNormal): it lies behind the plane of the card it started off, on the far side of a card
	// facing the other way. Across the inside of a braid's tube, the far wall faces the other way too,
	// but the point stays in front of the near wall's plane.
	bool CrossedCard(float3 a_start, float a_startDistance, float3 a_startNormal, float3 a_end, float3 a_endNormal)
	{
		const float3 onCard = a_start - a_startNormal * a_startDistance;
		return dot(a_endNormal, a_startNormal) < 0.0 && dot(a_end - onCard, a_startNormal) < 0.0;
	}
}

#endif  // __HAIR_STRANDS_CARD_FIELD_HLSLI__
