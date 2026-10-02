#pragma once

#include <string>
#include <vector>

#include "CardsToStrands/CardsToStrands.h"
#include "MeshExtract.h"
#include "StrandStyle.h"

namespace Strands
{
	/** @brief One strand control point in the bind pose. Mirrors RestPoint in HairStrands/Common.hlsli. */
	struct RestPoint
	{
		float3 position;
		float u;
		float3 normal;
		float v;
		uint32_t bones01;  // skin-instance bone indices 0 and 1 (16 bits each)
		uint32_t bones23;
		uint32_t weights;  // four unorm8 weights
		float t;           // arclength fraction, 0 at the root
	};
	static_assert(sizeof(RestPoint) == 48);

	/** @brief Per-strand data. Mirrors StrandInfo in HairStrands/Common.hlsli. */
	struct StrandInfo
	{
		float length;
		float random;    // uniform [0, 1), stable per strand
		uint32_t guide;  // the simulated strand this one follows (itself for a guide)
		float clumpRandom;
		float width;  // relative: scales the style's root and tip widths (1 for converted hair; the designer's Set Thickness)
	};
	static_assert(sizeof(StrandInfo) == 20);

	/** @brief Mirrors CardVertex in HairStrands/Common.hlsli: a vertex of the cards kept as cards (braids, ties, gathered hair). */
	struct CardVertex
	{
		float3 position;  // bind pose, skin space
		float u;
		float3 normal;
		float v;
		float3 tangent;    // the first row of the Lighting shader's TBN
		uint32_t bones01;  // palette bones 0 and 1 (16 bits each): skin-instance bones, then chain joints
		float3 bitangent;  // the second row
		uint32_t bones23;
		uint32_t weights;  // four unorm8 weights
		uint32_t pad0;
		uint32_t pad1;
		uint32_t pad2;
	};
	static_assert(sizeof(CardVertex) == 80);

	/** @brief A generated strand asset: strandCount strands of pointsPerStrand points each. */
	struct StrandAssetData
	{
		uint32_t pointsPerStrand = 0;
		std::vector<RestPoint> points;
		std::vector<StrandInfo> strands;  // shuffled, so any prefix is an even thinning

		// The first guideCount strands are the simulated guides (an even thinning, like any
		// prefix); every strand names the guide it follows.
		uint32_t guideCount = 0;
		// Head collider, in skin space: a sphere round the skull centre, just inside the hair.
		int32_t headBone = -1;  // skin-instance bone of the head, -1 if the mesh has none
		float3 headCentre;
		float headRadius = 0.0f;

		float averageLength = 0.0f;
		float flowMapShare = 0.0f;  // of the converted card area, the part whose flow came from the flow map
		SeedMode seedingUsed = SeedMode::Roots;
		uint32_t convertedTriangles = 0;
		uint32_t totalTriangles = 0;
		CardsToStrands::Stats conversion;  // how the card guides were bound to the scalp

		// The cards kept as cards (braids, ties, buns, the hair gathered into them), drawn in place
		// of their part of the hair; and the chains the hanging braids among them swing on. A
		// chain's joints are palette bones from chainBoneBase on, after the skin instance's.
		std::vector<CardVertex> cardVertices;
		std::vector<uint32_t> cardIndices;
		std::vector<CardsToStrands::ChainCurve> chains;
		uint32_t chainBoneBase = 0;
		uint32_t chainBoneCount = 0;

		std::string sourceFile;  // the .skhair file it was loaded from; empty if converted here

		uint32_t StrandCount() const { return static_cast<uint32_t>(strands.size()); }
	};

	/**
	 * @brief Converts a hair mesh into strands that grow from the scalp.
	 *
	 * Adapts the game's mesh and style to the engine-agnostic CardsToStrands module, which
	 * does the work (docs/development/hair-cards-to-strands.md), and packs its result for the
	 * GPU. Flow follows the flow map, else the way each atlas strip's strands are painted,
	 * oriented away from the head. Streamlines of that flow traced along the cards become card
	 * guides; a scalp fitted to the innermost hair is where every strand grows from, and card
	 * hair that starts away from it continues the rooted hair it lies on. Each card guide grows a
	 * clump of strands. Then every strand is given the guide strand it follows when simulated.
	 * Pure CPU; safe to run on a worker thread.
	 *
	 * @return false with o_error set if the mesh has no usable flow.
	 */
	bool GenerateStrands(const HairMeshData& a_mesh, const StrandStyle& a_style, StrandAssetData& o_asset, std::string& o_error);
}
