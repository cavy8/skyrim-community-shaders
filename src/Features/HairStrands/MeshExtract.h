#pragma once

#include <string>
#include <vector>

namespace Strands
{
	/**
	 * The hair texture's alpha at a reduced size: where the cards actually show hair. Hair
	 * cards are mostly transparent (gaps between locks, tapered tips, whole empty regions of
	 * beard and brow meshes), so the card geometry alone overstates the hair's shape.
	 */
	struct CoverageMask
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> alpha;

		bool Empty() const { return alpha.empty(); }

		/** @brief Bilinear alpha in [0, 1] at a texture coordinate, wrapping like the hair's sampler. */
		float Sample(float a_u, float a_v) const
		{
			if (alpha.empty())
				return 1.0f;
			if (!std::isfinite(a_u) || !std::isfinite(a_v))
				return 0.0f;
			const float x = (a_u - std::floor(a_u)) * width - 0.5f;
			const float y = (a_v - std::floor(a_v)) * height - 0.5f;
			const float fx = std::floor(x), fy = std::floor(y);
			const float tx = x - fx, ty = y - fy;
			const auto wrap = [](int32_t a_index, uint32_t a_size) {
				const auto size = static_cast<int32_t>(a_size);
				return static_cast<uint32_t>(((a_index % size) + size) % size);
			};
			const uint32_t x0 = wrap(static_cast<int32_t>(fx), width), x1 = wrap(static_cast<int32_t>(fx) + 1, width);
			const uint32_t y0 = wrap(static_cast<int32_t>(fy), height), y1 = wrap(static_cast<int32_t>(fy) + 1, height);
			const auto at = [&](uint32_t a_x, uint32_t a_y) { return alpha[static_cast<size_t>(a_y) * width + a_x] / 255.0f; };
			return std::lerp(std::lerp(at(x0, y0), at(x1, y0), tx), std::lerp(at(x0, y1), at(x1, y1), tx), ty);
		}
	};

	/**
	 * One mip of the hair's diffuse texture on its way to the CPU: copied to a staging
	 * texture on the render thread, mapped once the GPU is done with it, decoded on a worker.
	 */
	struct CoverageReadback
	{
		winrt::com_ptr<ID3D11Texture2D> staging;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t width = 0;
		uint32_t height = 0;
		size_t rowPitch = 0;
		size_t slicePitch = 0;
		std::vector<uint8_t> bytes;
	};

	enum class ReadbackStatus
	{
		Pending,
		Done,
		Failed
	};

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

		CoverageMask coverage;  // empty: every part of the cards counts as hair

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

	/**
	 * @brief Starts copying the pass's diffuse texture (one mip, at most 512 texels across) to the CPU.
	 * Render thread only. Does not wait for the GPU.
	 * @return false with o_error set if the pass has no diffuse texture with alpha.
	 */
	bool BeginCoverageReadback(const RE::BSRenderPass* a_pass, CoverageReadback& o_readback, std::string& o_error);
	/** @brief Maps the staging copy once the GPU has written it. Render thread only. */
	ReadbackStatus PollCoverageReadback(CoverageReadback& io_readback);
	/** @brief Decodes a finished readback (any format, block-compressed included) into alpha. Any thread. */
	bool DecodeCoverage(const CoverageReadback& a_readback, CoverageMask& o_mask, std::string& o_error);
}
