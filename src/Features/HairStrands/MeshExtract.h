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
		std::vector<uint8_t> shade;  // luminance x alpha: the painted strands' streaks, for the way they run

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
	 * The hair's flow map, where it has one: the texture in slot 7 that Hair Specular reads when
	 * the material has the back-lighting flag. RG x 2 - 1 is the direction from tip to root in
	 * texture space; black texels have none.
	 */
	struct FlowMap
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> rg;  // two bytes per texel

		bool Empty() const { return rg.empty(); }

		/** @brief The tip-to-root direction at a texture coordinate (nearest texel, wrapping); zero where the map is black. */
		float2 Sample(float a_u, float a_v) const
		{
			if (rg.empty() || !std::isfinite(a_u) || !std::isfinite(a_v))
				return float2::Zero;
			const auto x = std::min(static_cast<uint32_t>((a_u - std::floor(a_u)) * width), width - 1);
			const auto y = std::min(static_cast<uint32_t>((a_v - std::floor(a_v)) * height), height - 1);
			const uint8_t* texel = &rg[(static_cast<size_t>(y) * width + x) * 2];
			if (texel[0] <= 2 && texel[1] <= 2)
				return float2::Zero;
			return { texel[0] / 127.5f - 1.0f, texel[1] / 127.5f - 1.0f };
		}
	};

	/**
	 * The hair texture's colour for strands, at the coverage mask's size. Strands have no
	 * alpha, and a strand's texture coordinates cross transparent texels (gaps between painted
	 * locks, tapered tips), whose colour is whatever the artist left there: black in many hair
	 * textures. Here every texel is filled from the painted hair around it, weighted by alpha.
	 */
	struct StrandColourImage
	{
		uint32_t width = 0;
		uint32_t height = 0;
		bool srgb = false;                        // in the source texture's colour space
		std::vector<std::vector<uint32_t>> mips;  // RGBA8 (alpha 255), width x height down to 1 x 1

		bool Empty() const { return mips.empty(); }
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
	 * Copied once on the render thread; the strand generator then runs on a worker. Body
	 * collision decodes worn meshes into it too (without names, coverage or flow).
	 */
	struct HairMeshData
	{
		std::vector<float3> positions;
		std::vector<float3> normals;  // empty if the shape has none
		// The tangent frame the normal map is read in, as the Lighting vertex shader builds it: the
		// first row of its TBN (the bitangent spread over the position's, normal's and tangent's
		// fourth components) and the second (the tangent's xyz). Empty if the shape has none.
		std::vector<float3> tangents;
		std::vector<float3> bitangents;
		std::vector<float2> uvs;
		std::vector<std::array<uint16_t, 4>> boneIndices;
		std::vector<std::array<float, 4>> boneWeights;
		std::vector<uint32_t> indices;  // triangle list

		std::vector<std::string> boneNames;     // per skin-instance bone
		std::vector<float3> boneBindPositions;  // each bone's origin in skin space

		CoverageMask coverage;  // empty: every part of the cards counts as hair
		FlowMap flow;           // empty: flow follows the texture

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
	 * Where each attribute sits in an SSE vertex. SSE packs vertex attributes in a fixed order,
	 * so offsets follow from the descriptor's flags alone (its offset nibbles are not trusted).
	 */
	struct VertexLayout
	{
		int32_t position = -1;  // three floats (and the bitangent's x); -1: kept elsewhere (dynamic shapes)
		int32_t uv = -1;        // two halves
		int32_t normal = -1;    // three unorm8 in [-1, 1]
		int32_t tangent = -1;   // three unorm8 in [-1, 1] (with a normal only)
		int32_t skinning = -1;  // four half weights, four bone bytes
		uint32_t stride = 0;
	};

	/** @brief The layout a vertex descriptor's flags give. */
	VertexLayout GetVertexLayout(const RE::BSGraphics::VertexDesc& a_desc);
	/** @brief The stride a vertex descriptor declares (its size nibble): GetVertexLayout's must agree. */
	uint32_t GetDeclaredStride(const RE::BSGraphics::VertexDesc& a_desc);

	/** @brief One GPU buffer's first bytes on their way to the CPU, for meshes whose CPU copy is gone. */
	struct BufferReadback
	{
		winrt::com_ptr<ID3D11Buffer> staging;
		uint32_t size = 0;
		std::vector<uint8_t> bytes;
	};

	/**
	 * @brief Starts copying a_size bytes from the start of a_buffer to the CPU. Render thread only.
	 * Does not wait for the GPU.
	 */
	bool BeginBufferReadback(ID3D11Buffer* a_buffer, uint32_t a_size, const char* a_name, BufferReadback& o_readback, std::string& o_error);
	/** @brief Maps the staging copy once the GPU has written it. Render thread only. */
	ReadbackStatus PollBufferReadback(BufferReadback& io_readback);

	/**
	 * A skinned shape's vertex data as the game keeps it, copied on the render thread (the game
	 * objects are only valid there) and decoded on any thread by DecodeSkinnedMesh.
	 */
	struct SkinnedMeshCopy
	{
		RE::BSGraphics::VertexDesc vertexDesc{};
		uint32_t vertexCount = 0;
		uint32_t boneCount = 0;
		std::vector<uint8_t> vertices;                      // vertexCount x the layout's stride
		BufferReadback vertexReadback;                      // in flight in their place when no partition kept a CPU copy
		std::vector<float3> dynamicPositions;               // a dynamic shape's (head parts): kept outside the vertex data
		std::vector<float> dynamicBitangentX;               // and the fourth component kept with each
		std::vector<uint32_t> indices;                      // triangle list, shape vertex indices
		std::vector<int32_t> vertexPartition;               // the partition whose bone table a vertex uses; -1: in no triangle
		std::vector<std::vector<uint16_t>> partitionBones;  // per partition: its bones as skin-instance bones
	};

	/**
	 * @brief Copies a skinned BSTriShape or BSDynamicTriShape's vertex data. Render thread only.
	 * @param a_allowReadback Without a CPU copy of the vertices, start reading the GPU's back
	 * (see PollMeshCopy) rather than fail.
	 */
	bool CopySkinnedMesh(RE::BSGeometry* a_geometry, bool a_allowReadback, SkinnedMeshCopy& o_copy, std::string& o_error);
	/**
	 * @brief Decodes a complete copy into positions, normals, texture coordinates, bone indices
	 * (skin-instance bones), weights and triangles. Any thread.
	 * @param a_requireUV Fail if the vertices have no texture coordinates.
	 */
	bool DecodeSkinnedMesh(const SkinnedMeshCopy& a_copy, bool a_requireUV, HairMeshData& o_mesh, std::string& o_error);

	/** A rigid (unskinned) BSTriShape's vertex data, copied on the render thread. */
	struct RigidMeshCopy
	{
		RE::BSGraphics::VertexDesc vertexDesc{};
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		std::vector<uint8_t> vertices;  // vertexCount x the layout's stride
		std::vector<uint16_t> indices;  // triangle list
		BufferReadback vertexReadback;  // in flight in their place when the shape kept no CPU copy
		BufferReadback indexReadback;
	};

	/** @brief Copies a rigid BSTriShape's vertex data, or starts reading it back. Render thread only. */
	bool CopyRigidMesh(RE::BSGeometry* a_geometry, RigidMeshCopy& o_copy, std::string& o_error);
	/** @brief Decodes a complete copy (every vertex on bone 0 with weight 1). Any thread. */
	bool DecodeRigidMesh(const RigidMeshCopy& a_copy, HairMeshData& o_mesh, std::string& o_error);

	/** @brief Advances a copy's readbacks, if any; Done once all its data is on the CPU. Render thread only. */
	ReadbackStatus PollMeshCopy(SkinnedMeshCopy& io_copy);
	ReadbackStatus PollMeshCopy(RigidMeshCopy& io_copy);

	/**
	 * @brief Starts copying the pass's diffuse texture (one mip, at most 512 texels across) to the CPU.
	 * Render thread only. Does not wait for the GPU.
	 * @return false with o_error set if the pass has no diffuse texture with alpha.
	 */
	bool BeginCoverageReadback(const RE::BSRenderPass* a_pass, CoverageReadback& o_readback, std::string& o_error);
	/**
	 * @brief Starts copying the pass's flow map (one mip, at most 512 texels across) to the CPU.
	 * Render thread only. Does not wait for the GPU.
	 * @return false if the pass has no flow map (o_error empty) or it cannot be copied (o_error set).
	 */
	bool BeginFlowReadback(const RE::BSRenderPass* a_pass, CoverageReadback& o_readback, std::string& o_error);
	/** @brief Maps the staging copy once the GPU has written it. Render thread only. */
	ReadbackStatus PollCoverageReadback(CoverageReadback& io_readback);
	/**
	 * @brief Decodes a finished readback (any format, block-compressed included). Any thread.
	 * @param o_mask   Receives the alpha and the alpha-weighted luminance.
	 * @param o_colour Receives the colour with transparent texels filled; left empty if nothing is painted.
	 */
	bool DecodeCoverage(const CoverageReadback& a_readback, CoverageMask& o_mask, StrandColourImage& o_colour, std::string& o_error);
	/** @brief Decodes a finished flow map readback. Any thread. */
	bool DecodeFlow(const CoverageReadback& a_readback, FlowMap& o_flow, std::string& o_error);
}
