#pragma once

#include <array>
#include <string>
#include <vector>

namespace Strands
{
	/**
	 * Body colliders fitted to the actor's own body and what it wears. Each rides one bone of the
	 * upper body and is a capsule round that bone's segment whose radius changes with direction:
	 * a map of the outermost surface of the worn triangles skinned to the bone (or to bones under
	 * it, such as breasts and pauldrons), taken in the bind pose. The rows run from the pole of the
	 * cap at the segment's start, along its side, to the pole of the cap at its end; the columns go
	 * round it. The maps are built on a worker when the worn meshes change and sampled by
	 * HairStrandsSkin::CollideBody.
	 */
	inline constexpr uint32_t kBodySlots = 8;           // HAIR_STRANDS_MAX_BODY_COLLIDERS
	inline constexpr uint32_t kBodyFieldColumns = 32;   // HAIR_STRANDS_BODY_FIELD_COLUMNS: round the segment
	inline constexpr uint32_t kBodyFieldCapRows = 6;    // HAIR_STRANDS_BODY_FIELD_CAP_ROWS: each end cap, pole to side
	inline constexpr uint32_t kBodyFieldSideRows = 12;  // HAIR_STRANDS_BODY_FIELD_SIDE_ROWS: along the segment
	inline constexpr uint32_t kBodyFieldRows = 2 * kBodyFieldCapRows + kBodyFieldSideRows;
	inline constexpr uint32_t kBodyFieldTexels = kBodyFieldColumns * kBodyFieldRows;

	/** @brief The body colliders, by the bone each rides. */
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
	 * The skeleton nodes the body colliders run between, found from a humanoid head bone. Only valid
	 * inside the hair's draw hooks, like every game object here.
	 */
	struct BodySkeleton
	{
		std::array<RE::NiAVObject*, kBodySlots> start{};  // the bone each collider rides
		std::array<RE::NiAVObject*, kBodySlots> end{};    // the joint its segment runs to

		bool Has(BodySlot a_slot) const { return start[static_cast<uint32_t>(a_slot)] && end[static_cast<uint32_t>(a_slot)]; }
		/**
		 * @brief The collider a skeleton node's part of the body belongs to: the nearest collider bone
		 * at or above it. -1 for the head and forearms (a segment's far joint that starts no collider)
		 * and anything under them, and for bones outside the upper body.
		 */
		int32_t SlotOf(const RE::NiAVObject* a_node) const;
	};

	/** @brief The colliders' bones from the hair's head bone: its neck, spine, clavicles and arms. */
	BodySkeleton FindBodySkeleton(RE::NiAVObject* a_head);

	/** @brief One collider's shape, fixed on its bone. */
	struct BodyColliderShape
	{
		bool present = false;
		RE::NiMatrix3 boneFromField;  // field axes in the bone's space (columns); the segment runs along +Z from the bone's origin
		float length = 0.0f;          // segment length, in the bone's units
		float bound = 0.0f;           // largest radius in the map: points further out skip it
		float coverage = 0.0f;        // share of the map with a surface
	};

	/** @brief The colliding triangles of one worn mesh, in its skin space (bind pose). */
	struct BodyMesh
	{
		std::vector<float3> positions;
		std::vector<uint32_t> indices;                            // triangle list
		std::vector<uint8_t> slots;                               // per triangle: a bit per collider it belongs to
		std::array<std::array<float4, 3>, kBodySlots> toField{};  // per collider: rows taking skin space to its field space
	};

	/** @brief Everything the build needs, copied from game objects on the render thread. */
	struct BodyFieldInput
	{
		std::array<BodyColliderShape, kBodySlots> shapes;
		std::vector<BodyMesh> meshes;
	};

	struct BodyFieldData
	{
		std::array<BodyColliderShape, kBodySlots> shapes;
		std::vector<float> field;  // per collider, rows x columns of (radius, its slope along the column, along the row); radius 0: no surface
		std::string summary;       // per collider coverage, for the log
		std::string error;         // set: unusable, the bone capsules stay (the reason, for the log)
	};

	/**
	 * @brief The actor's worn meshes that hair can rest on: every visible, skinned, lit geometry
	 * under its 3D except head parts, plus its head mesh (for the neck). Render thread.
	 * @param a_faceNode The actor's face node: head parts under it are skipped.
	 * @param a_face     The actor's head mesh, or null.
	 * @param o_signature Changes when any of them is put on, taken off, shown or hidden.
	 */
	std::vector<RE::BSGeometry*> FindBodyMeshes(RE::NiAVObject* a_root, const RE::NiAVObject* a_faceNode, RE::BSGeometry* a_face, uint64_t& o_signature);

	/**
	 * @brief Copies a_meshes' triangles for a_skeleton's colliders (render thread).
	 * @return false with o_error set if no triangle belongs to a collider.
	 */
	bool PrepareBodyField(const BodySkeleton& a_skeleton, const std::vector<RE::BSGeometry*>& a_meshes, BodyFieldInput& o_input, std::string& o_error);

	/** @brief Builds the colliders' maps from a prepared input. Any thread. */
	void BuildBodyField(const BodyFieldInput& a_input, BodyFieldData& o_data);

	/** @brief Continuous (column, row) of field-space point a_q round a segment of length a_length (HairStrandsSkin::BodyFieldTexel). */
	float2 BodyFieldTexel(const float3& a_q, float a_length);
}
