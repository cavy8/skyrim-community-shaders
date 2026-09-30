#pragma once

#include <string>
#include <vector>

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
	};
	static_assert(sizeof(StrandInfo) == 16);

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

		uint32_t StrandCount() const { return static_cast<uint32_t>(strands.size()); }
	};

	namespace GeneratorLimits
	{
		inline constexpr uint32_t kMaxStrands = 40000;
		inline constexpr uint32_t kMinPointsPerStrand = 4;
		inline constexpr uint32_t kMaxPointsPerStrand = 32;
		inline constexpr float kMaxStrandLength = 200.0f;
		inline constexpr float kMinStrandLength = 0.25f;
		inline constexpr uint32_t kStrandsPerGuide = 8;
		inline constexpr uint32_t kMinGuides = 64;
		inline constexpr uint32_t kMaxGuides = 4096;
	}

	/**
	 * @brief Converts a hair mesh into strands following its texture flow.
	 *
	 * Flow is the direction the chosen texture axis runs across each triangle. With
	 * FlowAxis::Auto it comes from the flow map where there is one; elsewhere each UV island
	 * (one card's strip of the atlas) follows the way the strands are painted in that part of
	 * the texture. Islands welded together turn together, away from the head where they hang
	 * free, and otherwise like the cards that sample the same texels. Double-sided cards keep
	 * one side. Strands are streamlines of that flow traced
	 * across the welded mesh, seeded
	 * along upstream boundary edges (roots), plus fill streamlines through any triangles
	 * the roots missed, or scattered over the surface for very short hair. Each strand is
	 * then given the guide strand it follows when simulated. Pure CPU; safe to run on a
	 * worker thread.
	 *
	 * @return false with o_error set if the mesh has no usable flow.
	 */
	bool GenerateStrands(const HairMeshData& a_mesh, const StrandStyle& a_style, StrandAssetData& o_asset, std::string& o_error);
}
