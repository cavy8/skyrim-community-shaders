#pragma once

#include <array>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "MeshExtract.h"

namespace Strands
{
	// --- The bone capsules: the fallback while an actor has no collision mesh ---

	inline constexpr uint32_t kBodySlots = 8;

	/** @brief The bone capsules, by the bone each rides. */
	enum class BodySlot : uint32_t
	{
		Neck,          // neck -> head
		Chest,         // spine 2 -> neck
		Back,          // spine 1 -> spine 2
		Waist,         // spine -> spine 1
		LeftShoulder,  // clavicle -> upper arm
		RightShoulder,
		LeftArm,  // upper arm -> forearm
		RightArm
	};

	/**
	 * The skeleton nodes the bone capsules run between, found from a humanoid head bone. Only valid
	 * inside the hair's draw hooks, like every game object here.
	 */
	struct BodySkeleton
	{
		std::array<RE::NiAVObject*, kBodySlots> start{};  // the bone each capsule rides
		std::array<RE::NiAVObject*, kBodySlots> end{};    // the joint its segment runs to

		bool Has(BodySlot a_slot) const { return start[static_cast<uint32_t>(a_slot)] && end[static_cast<uint32_t>(a_slot)]; }
	};

	/** @brief The capsules' bones from the hair's head bone: its neck, spine, clavicles and arms. */
	BodySkeleton FindBodySkeleton(RE::NiAVObject* a_head);

	// --- The body's distance field (BodySdf.cs.hlsl) ---

	/** @brief Mirrors CollisionVertex in HairStrands/BodySdf.cs.hlsl. */
	struct CollisionVertex
	{
		float3 position;   // its source's skin space (a rigid source's own space)
		uint32_t normal;   // octahedral, two snorm16 (PackCollisionNormal)
		uint32_t bones01;  // palette entries, 16 bits each
		uint32_t bones23;
		uint32_t weights;  // four unorm8
		uint32_t pad;
	};
	static_assert(sizeof(CollisionVertex) == 32);

	/** @brief Mirrors SkinnedCollisionVertex in HairStrands/BodySdf.cs.hlsl (GPU only). */
	struct SkinnedCollisionVertex
	{
		float3 position;  // grid cells, this frame
		float pad0;
		float3 previousPosition;  // grid cells: where it was last frame
		float pad1;
		float3 normal;  // grid axes
		float pad2;
	};
	static_assert(sizeof(SkinnedCollisionVertex) == 48);

	/** @brief Mirrors cbuffer BodySdfCB (b0) in HairStrands/BodySdf.cs.hlsl. */
	struct alignas(16) BodySdfCB
	{
		float4 worldToGrid[3];  // camera-relative position to grid cells
		float4 gridToWorld[3];  // grid cells to camera-relative: the actor's axes times the cell size

		uint32_t gridSize[3];
		uint32_t vertexCount;

		uint32_t triangleCount;
		uint32_t entryCount;
		float outsideBand;  // cells
		float insideScale;  // cells per step of a triangle's inside band byte

		float cellSize;   // units
		float maxMotion;  // units in a frame
		float insideCos;
		float maxExtent;  // cells

		float insideBandCos;
		float pad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(BodySdfCB);
	static_assert(sizeof(BodySdfCB) == 160);

	/** @brief Octahedral unit vector as two snorm16 (HairStrandsBody::UnpackNormal). */
	uint32_t PackCollisionNormal(const float3& a_normal);

	/** @brief A 3D texture with a UAV (and an SRV if a_srv), as the distance fields use. Throws on failure. */
	std::unique_ptr<Texture3D> MakeVolume(const std::array<uint32_t, 3>& a_size, DXGI_FORMAT a_format, bool a_srv, const char* a_name);

	/** @brief BodySdf.cs.hlsl's entry points, compiled. */
	struct BodySdfPrograms
	{
		winrt::com_ptr<ID3D11ComputeShader> skin;
		winrt::com_ptr<ID3D11ComputeShader> splat;
		winrt::com_ptr<ID3D11ComputeShader> finalize;

		bool Ready() const { return skin && splat && finalize; }
	};

	/** @brief This frame's distance field for one actor, as the strand shaders read it (SkinCB, t5, t6, s0). */
	struct BodyFieldView
	{
		std::array<float4, 3> toGrid{};    // camera-relative position to grid cells
		float3 size;                       // cells per axis
		float3 texel;                      // 1 / the field textures' cells per axis
		float trust = 0.0f;                // units: how much deeper than its target (or the surface) a point is believed to be
		std::array<float4, 3> rootMove{};  // a camera-relative point's move with the actor's root over the frame (0: none)
		ID3D11ShaderResourceView* motion = nullptr;
		ID3D11ShaderResourceView* surface = nullptr;
		ID3D11SamplerState* sampler = nullptr;
	};

	struct BodyCollisionStats
	{
		uint32_t actors = 0;       // with a collision mesh
		uint32_t triangles = 0;    // in their collision meshes
		uint32_t fieldsBuilt = 0;  // distance fields built last frame
		uint32_t pendingBuilds = 0;
		uint64_t gpuBytes = 0;
	};

	/**
	 * Hair collision with everything an actor visibly wears: its body, armour and clothes (skinned
	 * meshes), and what hangs on it (shields, weapons, quivers: rigid meshes), all but its head,
	 * which the head field covers.
	 *
	 * When what an actor wears changes, its meshes are copied on the render thread and turned into
	 * one decimated collision mesh on a worker: vertices merged about kClusterSize apart, sheets of
	 * cloth made one-sided and facing away from the body, long triangles split, and each triangle
	 * given how far behind it the mesh is solid. Every frame the mesh is skinned on the GPU with
	 * its sources' current bones and splatted into a narrow-band signed distance field round the
	 * hair's reach, in the actor's own axes (BodySdf.cs.hlsl). The strand simulation and the
	 * followers sample it (Skinning.hlsli).
	 *
	 * Game objects are only read inside Update, called from a hair's draw hooks; everything kept
	 * between frames is keyed by pointer but never dereferenced outside it.
	 */
	class BodyCollision
	{
	public:
		BodyCollision();
		~BodyCollision();

		/** @brief Once per frame before the world renders: statistics, actors gone. */
		void BeginFrame();

		/**
		 * @brief Keeps the actor's collision mesh in step with what it wears and takes its pose this
		 * frame. Render thread, inside a hair's draw hooks; once per actor per frame (later calls in
		 * the frame return at once).
		 * @param a_head The hair's head bone: rigid meshes under it, and skinned triangles wholly on
		 * it (or bones under it), are the head field's.
		 * @param a_face The actor's head mesh (for its neck), or null.
		 * @param a_log  Log builds at info (the player) rather than debug.
		 */
		void Update(RE::Actor* a_actor, RE::NiAVObject* a_head, RE::BSGeometry* a_face, bool a_log);

		/** @brief True if the actor has a collision mesh: the bone capsules are not needed. */
		bool HasMesh(RE::FormID a_actor) const;

		/**
		 * @brief Builds the actor's distance field for this frame on the GPU (once per frame while it
		 * stays in the shared textures) round the hair's reach. Binds compute state freely; the
		 * caller saves and restores it.
		 * @param a_centre Camera-relative centre of the hair's reach.
		 * @param a_reach  How far from it the hair reaches, in units.
		 * @return false if there is no field this frame (no mesh, or nothing within reach).
		 */
		bool Prepare(RE::FormID a_actor, const float3& a_eye, const float3& a_centre, float a_reach, const BodySdfPrograms& a_programs, BodyFieldView& o_view);

		/** @brief Frees every collision mesh and the shared field (feature off). */
		void Reset();

		BodyCollisionStats GetStats() const { return stats; }

	private:
		struct ActorBody;
		struct LiveSource;

		/** @brief Takes a finished build into the actor's collision mesh (GPU buffers). */
		void TakeBuild(ActorBody& a_body);
		/** @brief Copies this frame's sources for a build (render thread). */
		void StartBuild(ActorBody& a_body, const std::vector<LiveSource>& a_sources, RE::NiAVObject* a_root, RE::NiAVObject* a_head, uint64_t a_signature);
		/** @brief Starts the build's worker once every copy is on the CPU. */
		void PollBuild(ActorBody& a_body);
		/** @brief The shared buffers and textures, grown to fit. */
		bool EnsureShared(uint32_t a_vertices, const std::array<uint32_t, 3>& a_cells);

		std::unordered_map<RE::FormID, std::unique_ptr<ActorBody>> actors;

		// Shared by every actor, rebuilt for each one as its hair is simulated: the skinned
		// collision vertices, the cells' nearest triangles, and the field.
		std::unique_ptr<Buffer> skinned;
		uint32_t skinnedCapacity = 0;
		std::unique_ptr<Texture3D> cells;
		std::unique_ptr<Texture3D> motion;
		std::unique_ptr<Texture3D> surface;
		uint32_t fieldCapacity[3]{};
		std::unique_ptr<ConstantBuffer> constants;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		RE::FormID fieldActor = 0;         // whose field the textures hold
		uint32_t fieldFrame = UINT32_MAX;  // and from which frame
		float fieldReach = 0.0f;           // how far round the hair it reaches
		BodyFieldView fieldView;

		BodyCollisionStats stats;
		uint32_t fieldsThisFrame = 0;
	};
}
