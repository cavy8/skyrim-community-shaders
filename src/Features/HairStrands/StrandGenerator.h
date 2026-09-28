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
		float random;  // uniform [0, 1), stable per strand
		uint32_t clump;
		float clumpRandom;
	};
	static_assert(sizeof(StrandInfo) == 16);

	/** @brief A generated strand asset: strandCount strands of pointsPerStrand points each. */
	struct StrandAssetData
	{
		uint32_t pointsPerStrand = 0;
		std::vector<RestPoint> points;
		std::vector<StrandInfo> strands;  // shuffled, so any prefix is an even thinning

		float averageLength = 0.0f;
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
	}

	/**
	 * @brief Converts a hair mesh into strands following its texture flow.
	 *
	 * Flow is the direction the chosen texture axis (V by default) runs across each
	 * triangle; with FlowAxis::Auto each connected piece is oriented to point away from
	 * the head. Strands are streamlines of that flow traced across the welded mesh, seeded
	 * along upstream boundary edges (roots), plus fill streamlines through any triangles
	 * the roots missed, or scattered over the surface for very short hair. Pure CPU; safe
	 * to run on a worker thread.
	 *
	 * @return false with o_error set if the mesh has no usable flow.
	 */
	bool GenerateStrands(const HairMeshData& a_mesh, const StrandStyle& a_style, StrandAssetData& o_asset, std::string& o_error);
}
