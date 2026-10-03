#pragma once

#include <array>
#include <memory>
#include <vector>

#include "BodySdf.h"
#include "CardsToStrands/CardsToStrands.h"
#include "StrandGenerator.h"

namespace Strands
{
	/** @brief Mirrors cbuffer CardFieldCB (b1) in HairStrands/CardField.hlsli. */
	struct alignas(16) CardFieldCB
	{
		float4 toGrid[3];    // camera-relative position to grid cells (cell centres at +0.5)
		float3 gridSize;     // cells per axis
		float minClearance;  // units: the least a point is kept off the cards (where its target lies closer)
		float maxClearance;  // units: how far off the cards a point is kept (less where its target lies closer)
		float trust;         // units: how far from the cards the field reaches
		float cellSize;      // units
		float pad;
	};
	STATIC_ASSERT_ALIGNAS_16(CardFieldCB);
	static_assert(sizeof(CardFieldCB) == 80);

	/** @brief This frame's card field for one hair, as the strand shaders read it (b1, t7, t8). */
	struct CardFieldView
	{
		ID3D11Buffer* constants = nullptr;
		ID3D11ShaderResourceView* motion = nullptr;
		ID3D11ShaderResourceView* surface = nullptr;
	};

	/**
	 * The cards a hair keeps (braids, ties, buns, the hair gathered into them) as a collision mesh
	 * for its strands, in the GPU buffers BodySdf.cs.hlsl builds a field from. Built once per asset.
	 */
	struct CardCollisionMesh
	{
		std::unique_ptr<Buffer> vertices;   // CollisionVertex, the cards' own palette bones
		std::unique_ptr<Buffer> triangles;  // uint4: three vertices, inside band 0 (a card is a sheet)
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		// Skin-space box round the vertices on no chain joint, which ride the head; empty (low > high)
		// when every kept card hangs on a chain.
		float3 staticLow;
		float3 staticHigh;
		// Per chain: the furthest any of its vertices lies from its joints, in skin units.
		std::vector<float> chainReach;

		uint64_t GpuBytes() const { return static_cast<uint64_t>(vertexCount) * sizeof(CollisionVertex) + static_cast<uint64_t>(triangleCount) * 16; }
	};

	/**
	 * @brief The collision mesh of a hair's kept cards, or null if it keeps none. Throws if the GPU
	 * buffers cannot be created.
	 */
	std::unique_ptr<CardCollisionMesh> BuildCardCollisionMesh(const std::vector<CardVertex>& a_vertices, const std::vector<uint32_t>& a_indices, const std::vector<CardsToStrands::ChainCurve>& a_chains);

	/** @brief Where this frame's card field goes, for CardField::Prepare. */
	struct CardFieldRequest
	{
		const CardCollisionMesh* mesh = nullptr;
		// The hair's palette (StrandRenderer's paletteData): bones x 3 rows this frame, relative to this
		// frame's camera, then bones x 3 last frame's, relative to last frame's camera.
		const std::vector<float4>* palette = nullptr;
		uint32_t bones = 0;
		uint32_t headBone = 0;
		float3 previousToCurrent;  // last frame's camera - this frame's camera
		// The joints of each chain at its last step, camera-relative (CardsToStrands::ChainSimulator).
		std::vector<const std::vector<CardsToStrands::Vec3>*> chainJoints;
		float3 reachCentre;  // camera-relative: the field is not needed further than reach from it
		float reach = 0.0f;
	};

	/**
	 * Strand collision with the cards a hair keeps. Every frame a hair with kept cards is simulated,
	 * its cards are skinned with the hair's own palette (so braids on chains are where they swing
	 * to) and splatted into a narrow-band distance field round them, in the head's axes, by the
	 * body field's kernels (BodySdf.cs.hlsl). Cards are sheets: the field is the distance either side
	 * of them, with no inside. The strand simulation and the followers read it (CardField.hlsli).
	 * The textures are shared by every hair, rebuilt for each as it is simulated.
	 */
	class CardField
	{
	public:
		/**
		 * @brief Builds the field for this frame and fills the view the strand shaders bind. Binds
		 * compute state freely; the caller saves and restores it.
		 * @return false if there is no field (nothing within reach, or the GPU resources failed).
		 */
		bool Prepare(const CardFieldRequest& a_request, const BodySdfPrograms& a_programs, CardFieldView& o_view);

		/** @brief Frees the shared resources (feature off). */
		void Reset();

		/** @brief Fields built since the last call. */
		uint32_t TakeFieldCount();

		uint64_t GpuBytes() const;

	private:
		bool EnsureShared(uint32_t a_vertices, uint32_t a_entries, const std::array<uint32_t, 3>& a_cells);

		std::unique_ptr<Buffer> palette;
		uint32_t paletteCapacity = 0;  // entries
		std::unique_ptr<Buffer> skinned;
		uint32_t skinnedCapacity = 0;
		std::unique_ptr<Texture3D> cells;
		std::unique_ptr<Texture3D> motion;
		std::unique_ptr<Texture3D> surface;
		std::array<uint32_t, 3> fieldCapacity{};
		std::unique_ptr<ConstantBuffer> buildConstants;  // BodySdfCB
		std::unique_ptr<ConstantBuffer> viewConstants;   // CardFieldCB
		uint32_t fieldsBuilt = 0;
	};
}
