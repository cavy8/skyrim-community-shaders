#pragma once

#include <string>
#include <vector>

namespace Strands
{
	/**
	 * A CPU copy of a skinned hair shape in its bind pose (the shape's skin space), with
	 * bone indices already resolved from partition-local to skin-instance bone indices.
	 * Copied once on the render thread; the strand generator then runs on a worker.
	 */
	struct HairMeshData
	{
		std::vector<float3> positions;
		std::vector<float3> normals;  // empty if the shape has none
		std::vector<float2> uvs;
		std::vector<std::array<uint16_t, 4>> boneIndices;
		std::vector<std::array<float, 4>> boneWeights;
		std::vector<uint32_t> indices;  // triangle list

		std::vector<std::string> boneNames;     // per skin-instance bone
		std::vector<float3> boneBindPositions;  // each bone's origin in skin space

		uint32_t TriangleCount() const { return static_cast<uint32_t>(indices.size() / 3); }
	};

	/**
	 * @brief Copies a skinned BSTriShape or BSDynamicTriShape into a HairMeshData.
	 * @param a_geometry The drawn hair geometry. Must be skinned (NiSkinInstance with partitions).
	 * @param o_mesh     Receives the copy.
	 * @param o_error    Receives the reason on failure.
	 * @return false if the geometry has no CPU-side vertex data this code understands.
	 */
	bool ExtractHairMesh(RE::BSGeometry* a_geometry, HairMeshData& o_mesh, std::string& o_error);
}
