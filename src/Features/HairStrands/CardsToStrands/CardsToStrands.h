#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Hair cards to strands: converts a textured hair-card mesh into strand curves that grow from
 * the scalp.
 *
 * This module is engine-agnostic on purpose. It depends on the C++ standard library only (its
 * own small vector types, no Skyrim, DirectX or logging types), so the same conversion runs
 * in-game (StrandGenerator.cpp adapts the game's mesh and style to it) and in standalone
 * tools such as a hair designer or tools/hair_cards_to_strands. See
 * docs/development/hair-cards-to-strands.md for the algorithm.
 */
namespace CardsToStrands
{
	struct Vec2
	{
		float x = 0.0f;
		float y = 0.0f;

		constexpr Vec2() = default;
		constexpr Vec2(float a_x, float a_y) :
			x(a_x), y(a_y) {}

		Vec2 operator+(const Vec2& a_o) const { return { x + a_o.x, y + a_o.y }; }
		Vec2 operator-(const Vec2& a_o) const { return { x - a_o.x, y - a_o.y }; }
		Vec2 operator-() const { return { -x, -y }; }
		Vec2 operator*(float a_s) const { return { x * a_s, y * a_s }; }
		Vec2 operator/(float a_s) const { return { x / a_s, y / a_s }; }
		Vec2& operator+=(const Vec2& a_o)
		{
			x += a_o.x;
			y += a_o.y;
			return *this;
		}
		bool operator==(const Vec2&) const = default;
		float Dot(const Vec2& a_o) const { return x * a_o.x + y * a_o.y; }
		float Length() const { return std::sqrt(x * x + y * y); }
		void Normalize()
		{
			const float l = Length();
			if (l > 0.0f) {
				x /= l;
				y /= l;
			}
		}
	};

	struct Vec3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;

		constexpr Vec3() = default;
		constexpr explicit Vec3(float a_v) :
			x(a_v), y(a_v), z(a_v) {}
		constexpr Vec3(float a_x, float a_y, float a_z) :
			x(a_x), y(a_y), z(a_z) {}

		Vec3 operator+(const Vec3& a_o) const { return { x + a_o.x, y + a_o.y, z + a_o.z }; }
		Vec3 operator-(const Vec3& a_o) const { return { x - a_o.x, y - a_o.y, z - a_o.z }; }
		Vec3 operator-() const { return { -x, -y, -z }; }
		Vec3 operator*(float a_s) const { return { x * a_s, y * a_s, z * a_s }; }
		Vec3 operator/(float a_s) const { return { x / a_s, y / a_s, z / a_s }; }
		Vec3& operator+=(const Vec3& a_o)
		{
			x += a_o.x;
			y += a_o.y;
			z += a_o.z;
			return *this;
		}
		Vec3& operator-=(const Vec3& a_o)
		{
			x -= a_o.x;
			y -= a_o.y;
			z -= a_o.z;
			return *this;
		}
		Vec3& operator*=(float a_s)
		{
			x *= a_s;
			y *= a_s;
			z *= a_s;
			return *this;
		}
		bool operator==(const Vec3&) const = default;
		float operator[](int a_i) const { return a_i == 0 ? x : (a_i == 1 ? y : z); }

		float Dot(const Vec3& a_o) const { return x * a_o.x + y * a_o.y + z * a_o.z; }
		Vec3 Cross(const Vec3& a_o) const { return { y * a_o.z - z * a_o.y, z * a_o.x - x * a_o.z, x * a_o.y - y * a_o.x }; }
		float LengthSquared() const { return Dot(*this); }
		float Length() const { return std::sqrt(LengthSquared()); }
		void Normalize()
		{
			const float l = Length();
			if (l > 0.0f)
				*this = *this / l;
		}
		Vec3 Normalized() const
		{
			Vec3 v = *this;
			v.Normalize();
			return v;
		}
		static Vec3 Lerp(const Vec3& a_a, const Vec3& a_b, float a_t) { return a_a + (a_b - a_a) * a_t; }
		static Vec3 Min(const Vec3& a_a, const Vec3& a_b) { return { std::min(a_a.x, a_b.x), std::min(a_a.y, a_b.y), std::min(a_a.z, a_b.z) }; }
		static Vec3 Max(const Vec3& a_a, const Vec3& a_b) { return { std::max(a_a.x, a_b.x), std::max(a_a.y, a_b.y), std::max(a_a.z, a_b.z) }; }
	};

	/**
	 * The hair texture's alpha (where the cards show hair) and its luminance x alpha (the
	 * painted streaks, for the way strands run), usually at a reduced size. Empty: every part
	 * of the cards counts as hair.
	 */
	struct CoverageImage
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> alpha;
		std::vector<uint8_t> shade;

		bool Empty() const { return alpha.empty(); }
		/** @brief Bilinear alpha in [0, 1], wrapping like the hair's sampler; 1 when empty. */
		float Sample(float a_u, float a_v) const;
	};

	/**
	 * A flow map: RG x 2 - 1 is the direction from tip to root in texture space (as Skyrim's
	 * Hair Specular reads it); black texels have none.
	 */
	struct FlowImage
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> rg;  // two bytes per texel

		bool Empty() const { return rg.empty(); }
		/** @brief The tip-to-root direction (nearest texel, wrapping); zero where black. */
		Vec2 Sample(float a_u, float a_v) const;
	};

	/** @brief A hair-card mesh in its bind pose. Units are arbitrary but the defaults assume Skyrim units (about 1.4 cm). +Z is up. */
	struct CardMesh
	{
		std::vector<Vec3> positions;
		std::vector<Vec3> normals;  // empty if the mesh has none
		// The tangent frame the normal map is read in (Skyrim's per-vertex bitangent and tangent: the
		// first and second rows of its TBN); empty if the mesh has none. Only the cards kept as cards use them.
		std::vector<Vec3> tangents;
		std::vector<Vec3> bitangents;
		std::vector<Vec2> uvs;
		std::vector<std::array<uint16_t, 4>> boneIndices;  // may be empty: every point on bone 0
		std::vector<std::array<float, 4>> boneWeights;
		std::vector<uint32_t> indices;  // triangle list

		std::vector<std::string> boneNames;  // a bone named like "NPC Head" marks the skull
		std::vector<Vec3> boneBindPositions;

		CoverageImage coverage;
		FlowImage flow;

		uint32_t TriangleCount() const { return static_cast<uint32_t>(indices.size() / 3); }
	};

	/** @brief Which texture direction runs from root to tip. Auto: the flow map, else the painted streaks of each UV island, oriented away from the head. */
	enum class FlowAxis : uint32_t
	{
		Auto,
		V,
		NegV,
		U,
		NegU
	};

	/** @brief Where strands start. */
	enum class Seeding : uint32_t
	{
		Auto,   // Scalp, or Area when the cards trace out very short
		Scalp,  // every strand grows from the scalp; card hair that starts away from it continues the hair it lies on
		Area    // short strands scattered over the cards near the scalp (buzz cuts, fuzz)
	};

	struct UVRect
	{
		float minU = 0.0f;
		float minV = 0.0f;
		float maxU = 0.0f;
		float maxV = 0.0f;

		bool Contains(float a_u, float a_v) const { return a_u >= minU && a_u <= maxU && a_v >= minV && a_v <= maxV; }
	};

	/** @brief What a triangle of the cards becomes. */
	enum class Region : uint8_t
	{
		Strands,  // loose hair: its cards are replaced by strands
		Cards,    // stays a card, skinned as authored: hair gathered into a tie or a braid, braids lying on the head, ties, buns
		Chain     // stays a card, swinging on a simulated chain: a braid or twist hanging free
	};

	/** @brief A triangle's region as chosen by hand (a designer, a style), or Auto to let the conversion decide. */
	enum class RegionChoice : uint8_t
	{
		Auto,
		Strands,
		Cards,
		Chain
	};

	struct Settings
	{
		Seeding seeding = Seeding::Auto;
		FlowAxis flowAxis = FlowAxis::Auto;
		float density = 20.0f;           // strands per unit of card width
		float segmentLength = 1.0f;      // control-point spacing
		float lengthScale = 1.0f;        // fraction of each strand's length kept
		float volume = 0.15f;            // lift off the card towards the tip
		float layerJitter = 0.12f;       // random lift at the root, for depth
		float clumpStrength = 0.25f;     // pull of each clump's strands towards its card guide, towards the tip
		float clumpSize = 1.5f;          // card width each card guide (one clump) stands for
		float clumpTwist = 0.0f;         // turns per unit round the card guide (locs, twists)
		float shortLength = 1.2f;        // strand length with Area seeding
		float coverageThreshold = 0.3f;  // texture alpha below this has no hair (0: ignore the texture)
		float tipVariation = 0.15f;      // strands end up to this fraction short of their card guide's tip
		uint32_t seed = 1;
		std::vector<UVRect> excludeUV;  // triangles whose UV centre lies in one stay cards (Region::Cards)
		std::vector<UVRect> chainUV;    // triangles whose UV centre lies in one hang on a chain (Region::Chain)
		// Find the parts that are not loose hair (braids, twists, ties, buns, the hair gathered into
		// them) and keep them as cards; off, every triangle not chosen by hand becomes strands.
		bool keepWoven = true;
		// Per mesh triangle, a region chosen by hand; empty, or Auto, lets the conversion decide.
		// Wins over excludeUV and chainUV.
		std::vector<RegionChoice> triangleRegions;
	};

	namespace Limits
	{
		inline constexpr uint32_t kMaxStrands = 40000;
		inline constexpr uint32_t kMaxCardGuides = 16384;
		inline constexpr uint32_t kMinPointsPerStrand = 4;
		inline constexpr uint32_t kMaxPointsPerStrand = 32;
		inline constexpr float kMaxStrandLength = 200.0f;
		inline constexpr float kMinStrandLength = 0.25f;
		inline constexpr uint32_t kStrandsPerGuide = 8;  // simulated guides: one per this many strands
		inline constexpr uint32_t kMinGuides = 64;
		inline constexpr uint32_t kMaxGuides = 4096;
		inline constexpr uint32_t kMaxChains = 16;
		inline constexpr uint32_t kMaxChainJoints = 16;  // per chain
	}

	/** @brief One strand control point in the bind pose. */
	struct StrandPoint
	{
		Vec3 position;
		Vec3 normal;  // the card's shading normal, facing away from the head
		Vec2 uv;      // where the strand samples the hair texture
		std::array<uint16_t, 4> bones{};
		std::array<float, 4> weights{};  // strongest first, summing to 1
		float t = 0.0f;                  // arclength fraction, 0 at the root
	};

	struct Strand
	{
		float length = 0.0f;
		float random = 0.0f;       // uniform [0, 1), stable per strand
		uint32_t guide = 0;        // the simulated strand this one follows (itself for a guide)
		float clumpRandom = 0.0f;  // uniform [0, 1), shared by a clump
		uint32_t cardGuide = 0;    // the card guide (clump) it was grown around
		bool scalpRooted = false;  // its root lies on the fitted scalp
	};

	/**
	 * The scalp the strands grow from: a radius per direction round a centre, fitted to the
	 * innermost layer of the hair over the top of the head, a sphere elsewhere.
	 */
	struct Scalp
	{
		static constexpr int kAzimuthBins = 24;
		static constexpr int kElevationBins = 12;

		Vec3 centre;
		float sphereRadius = 0.0f;
		std::array<float, kAzimuthBins * kElevationBins> radii{};
		bool fitted = false;

		/** @brief The scalp's distance from the centre in a unit direction. */
		float Radius(const Vec3& a_direction) const;
		/** @brief Height of a point above the scalp (negative inside). */
		float Height(const Vec3& a_point) const;
		/** @brief The point of the scalp in a point's direction from the centre. */
		Vec3 Project(const Vec3& a_point) const;
	};

	/** @brief What became of a card guide (a streamline traced along the cards) when binding it to the scalp. */
	enum class GuideKind : uint8_t
	{
		Free,       // not bound (Area seeding)
		Rooted,     // starts on the scalp
		Merged,     // started away from it; continues the hair rooted on the scalp that it lies on
		Bridged,    // started a little off the scalp; joined to it directly
		Continued,  // started where rooted hair ends, which carries on into it (a layer starting mid-length); grows no clump of its own
		Dropped,    // started far from the scalp and from any hair rooted on it
		Gathered,   // hair gathered into a tie or a braid (pulled tight to the head): stays cards, grows no strands
		Tied        // starts at a tie or at the end of a braid: its strands grow from there (a ponytail's tail, a braid's tuft)
	};

	/** @brief A card guide as bound: the centre line of one clump of strands. */
	struct GuideCurve
	{
		std::vector<Vec3> path;  // root to tip; for a bound guide, the root is on the scalp (or on its tie)
		GuideKind kind = GuideKind::Free;
		uint32_t strands = 0;  // strands grown round it
		int32_t tie = -1;      // Tied: the tie it grows from (Result::ties)
	};

	/**
	 * A braid or twist hanging free, simulated as a chain of rigid segments. Its cards are skinned
	 * to the chain's joints: joint j is bone firstBone + j, numbered after the mesh's own bones.
	 * Joints from the root up to pinnedJoints - 1 lie on the head and move with parentBone.
	 */
	struct ChainCurve
	{
		std::vector<Vec3> joints;  // bind pose, root first
		uint32_t pinnedJoints = 1;
		float radius = 0.0f;  // the braid's thickness round its joints, for collision
		int32_t parentBone = -1;
		uint32_t firstBone = 0;
		uint32_t triangles = 0;  // mesh triangles skinned to it
	};

	/** @brief Where hair is gathered: a ponytail's tie, or a braid's end. Tied card guides grow strands from it. */
	struct Tie
	{
		Vec3 centre;
		float radius = 0.0f;
		int32_t chain = -1;     // the chain whose end it is; -1: on the head
		uint32_t gathered = 0;  // card guides gathered into it
		uint32_t tails = 0;     // card guides growing from it
	};

	/** @brief A vertex of the cards kept as cards (Region::Cards and Chain), skinned for drawing. */
	struct CardVertex
	{
		Vec3 position;  // bind pose
		Vec3 normal;
		Vec3 tangent;    // CardMesh::tangents, else along U
		Vec3 bitangent;  // CardMesh::bitangents, else along V
		Vec2 uv;
		std::array<uint16_t, 4> bones{};  // mesh bones, or chain joints from Result::chainBoneBase
		std::array<float, 4> weights{};
		uint32_t source = 0;  // the mesh vertex it copies
	};

	struct Stats
	{
		uint32_t totalTriangles = 0;
		uint32_t convertedTriangles = 0;
		float flowMapShare = 0.0f;     // of the converted card area, the part whose flow came from the flow map
		uint32_t cardGuides = 0;       // streamlines traced along the cards
		uint32_t redundantGuides = 0;  // dropped for running alongside a longer one
		uint32_t rootedGuides = 0;     // starting on the scalp
		uint32_t mergedGuides = 0;     // starting away from it, continuing the hair they lie on
		uint32_t continuedGuides = 0;  // starting where rooted hair ends, which carries on into them
		uint32_t bridgedGuides = 0;    // starting a little off the scalp, joined to it directly
		uint32_t droppedGuides = 0;    // starting far from the scalp and from any hair rooted on it
		uint32_t gatheredGuides = 0;   // gathered into a tie or a braid, kept as cards
		uint32_t tiedGuides = 0;       // growing from a tie or a braid's end
		uint32_t cardTriangles = 0;    // kept as cards on their own bones (Region::Cards)
		uint32_t chainTriangles = 0;   // kept as cards on a chain (Region::Chain)
		uint32_t wovenPieces = 0;      // braids, twists, ties and buns found
		Seeding seedingUsed = Seeding::Scalp;
	};

	struct Result
	{
		uint32_t pointsPerStrand = 0;
		std::vector<StrandPoint> points;  // strand-major, pointsPerStrand each
		std::vector<Strand> strands;      // shuffled, so any prefix is an even thinning

		uint32_t guideCount = 0;  // the first guideCount strands are the simulated guides
		int32_t headBone = -1;    // index of the skull bone, -1 if none
		Vec3 headCentre;
		float headRadius = 0.0f;  // a head collider sphere round headCentre, just inside the strands
		float averageLength = 0.0f;
		Scalp scalp;
		std::vector<GuideCurve> guides;  // the card guides left after dropping repeats; Strand::cardGuide indexes these

		// What each mesh triangle became. Strand points grown from a chain's end are skinned to its
		// joints, so with chains the bone palette runs past the mesh's bones: bones chainBoneBase to
		// chainBoneBase + chainBoneCount - 1 are chain joints.
		std::vector<Region> triangleRegions;
		std::vector<ChainCurve> chains;
		std::vector<Tie> ties;
		uint32_t chainBoneBase = 0;
		uint32_t chainBoneCount = 0;
		// The triangles kept as cards (Cards and Chain), as a mesh to draw: Chain vertices skinned to
		// their chain's joints, the rest as authored.
		std::vector<CardVertex> cardVertices;
		std::vector<uint32_t> cardIndices;
		Stats stats;

		uint32_t StrandCount() const { return static_cast<uint32_t>(strands.size()); }
	};

	/**
	 * @brief Converts a hair-card mesh into strands. Pure CPU, no global state; safe on any thread.
	 * @return false with o_error set if the mesh has no usable flow, or nothing could be traced or kept.
	 */
	bool Convert(const CardMesh& a_mesh, const Settings& a_settings, Result& o_result, std::string& o_error);

	/**
	 * An affine transform as the three rows of its 3x4 matrix: a point maps to
	 * (rows[0] . (p, 1), rows[1] . (p, 1), rows[2] . (p, 1)). A skinning palette's layout.
	 */
	struct Affine
	{
		std::array<std::array<float, 4>, 3> rows{ { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } } };

		Vec3 Apply(const Vec3& a_p) const { return ApplyLinear(a_p) + Vec3(rows[0][3], rows[1][3], rows[2][3]); }
		Vec3 ApplyLinear(const Vec3& a_v) const
		{
			return { rows[0][0] * a_v.x + rows[0][1] * a_v.y + rows[0][2] * a_v.z, rows[1][0] * a_v.x + rows[1][1] * a_v.y + rows[1][2] * a_v.z,
				rows[2][0] * a_v.x + rows[2][1] * a_v.y + rows[2][2] * a_v.z };
		}
	};

	/** @brief How a chain (ChainCurve) moves. Units and seconds; per-step values are for steps of 1/60 s. */
	struct ChainSettings
	{
		float gravity = 400.0f;      // units/s^2, down (-Z)
		float damping = 0.08f;       // share of its velocity relative to the head a joint loses per step
		float stiffness = 0.2f;      // pull back towards the braid's styled shape per step: 0 limp, 1 rigid
		float inertia = 0.6f;        // share of the head's acceleration a joint does not follow: 0 rides the head, 1 free
		float maxInertia = 1600.0f;  // units/s^2: the most a jolt of the head (a snap turn, a stagger) throws a joint
		float dftlDamping = 0.9f;    // share of the length constraints' pull on the next joint taken out of a joint's velocity
		uint32_t iterations = 4;     // constraint passes per step
	};

	/** @brief A capsule a chain keeps out of (a sphere when a == b), in the chain's space. */
	struct ChainCollider
	{
		Vec3 a;
		Vec3 b;
		float radius = 0.0f;
	};

	/**
	 * Simulates one ChainCurve: points at its joints, the pinned ones carried by the parent bone.
	 * The others move with the parent bone, plus a velocity of their own relative to it: gravity,
	 * the part of the head's acceleration they do not follow, damped, so walking or running
	 * carries the braid along instead of blowing it back. Each is pulled towards the styled shape
	 * as its parent segment carries it (TressFX's local shape constraint, on both ends of a
	 * segment), segments are kept at their length from the root down (follow the leader, with
	 * DFTL's velocity correction), and pushed out of colliders. Each joint becomes a bone the
	 * braid's cards are skinned to. Works in whatever space the parent transforms are given in
	 * (the game: camera-relative, shifted with Translate).
	 */
	class ChainSimulator
	{
	public:
		/** @brief Puts the chain at rest in its styled shape; a_parent is the parent bone's skin-to-world transform. */
		void Reset(const ChainCurve& a_chain, const Affine& a_parent);
		/** @brief One step of a_dt seconds; a_parent is the parent bone's transform at the step's end. Resets if never set. */
		void Step(const ChainCurve& a_chain, const Affine& a_parent, float a_dt, const ChainSettings& a_settings, const ChainCollider* a_colliders, size_t a_colliderCount);
		/** @brief Moves the whole state, for a change of the space it is kept in (a camera-relative origin moving). */
		void Translate(const Vec3& a_delta);
		/**
		 * @brief Each joint's bone: the skin-to-world transform taking the bind pose to the chain
		 * as drawn, a_alpha of the way from the step before the last to the last, carried by the
		 * parent bone as it is now (a_parent), so the chain follows the head between steps.
		 */
		void Bones(const ChainCurve& a_chain, const Affine& a_parent, float a_alpha, std::vector<Affine>& o_bones) const;
		bool Started() const { return !position.empty(); }
		/** @brief The joints at the last step. */
		const std::vector<Vec3>& Joints() const { return position; }

	private:
		std::vector<Vec3> position;
		std::vector<Vec3> velocity;    // each joint's own move over the last step, beyond its target's (relative to the head)
		std::vector<Vec3> target;      // where the parent bone alone carried each joint at the last step
		std::vector<Vec3> targetMove;  // and how far that moved over the last step
		std::vector<Vec3> offset;      // position - target, at the last step
		std::vector<Vec3> previousOffset;
		bool moved = false;  // targetMove is known (a step since the reset)
	};
}
