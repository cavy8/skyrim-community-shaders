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
		std::vector<UVRect> excludeUV;  // triangles whose UV centre lies in one stay cards
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
		Continued,  // started where rooted hair ends, which carries on into it (a ponytail below its tie); grows no clump of its own
		Dropped     // started far from the scalp and from any hair rooted on it
	};

	/** @brief A card guide as bound: the centre line of one clump of strands. */
	struct GuideCurve
	{
		std::vector<Vec3> path;  // root to tip; for a bound guide, the root is on the scalp
		GuideKind kind = GuideKind::Free;
		uint32_t strands = 0;  // strands grown round it
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
		Stats stats;

		uint32_t StrandCount() const { return static_cast<uint32_t>(strands.size()); }
	};

	/**
	 * @brief Converts a hair-card mesh into strands. Pure CPU, no global state; safe on any thread.
	 * @return false with o_error set if the mesh has no usable flow or no strand could be traced.
	 */
	bool Convert(const CardMesh& a_mesh, const Settings& a_settings, Result& o_result, std::string& o_error);
}
