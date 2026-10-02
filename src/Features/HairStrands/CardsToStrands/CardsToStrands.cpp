#include "CardsToStrands.h"

#include <cfloat>
#include <functional>
#include <numeric>
#include <queue>
#include <random>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace CardsToStrands
{
	float CoverageImage::Sample(float a_u, float a_v) const
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

	Vec2 FlowImage::Sample(float a_u, float a_v) const
	{
		if (rg.empty() || !std::isfinite(a_u) || !std::isfinite(a_v))
			return {};
		const auto x = std::min(static_cast<uint32_t>((a_u - std::floor(a_u)) * width), width - 1);
		const auto y = std::min(static_cast<uint32_t>((a_v - std::floor(a_v)) * height), height - 1);
		const uint8_t* texel = &rg[(static_cast<size_t>(y) * width + x) * 2];
		if (texel[0] <= 2 && texel[1] <= 2)
			return {};
		return { texel[0] / 127.5f - 1.0f, texel[1] / 127.5f - 1.0f };
	}

	namespace
	{
		constexpr float kPi = 3.14159265358979f;
		const Vec3 kZero{};
		const Vec3 kUnitZ{ 0.0f, 0.0f, 1.0f };
	}

	float Scalp::Radius(const Vec3& a_direction) const
	{
		if (!fitted)
			return sphereRadius;
		// Bilinear over bin centres; azimuth wraps, elevation clamps.
		const float azimuth = std::atan2(a_direction.y, a_direction.x);
		const float elevation = std::asin(std::clamp(a_direction.z, -1.0f, 1.0f));
		const float ax = (azimuth + kPi) / (2.0f * kPi) * kAzimuthBins - 0.5f;
		const float ey = std::clamp((elevation + 0.5f * kPi) / kPi * kElevationBins - 0.5f, 0.0f, kElevationBins - 1.0f);
		const float fx = std::floor(ax), fy = std::floor(ey);
		const float tx = ax - fx, ty = ey - fy;
		const auto a0 = ((static_cast<int>(fx) % kAzimuthBins) + kAzimuthBins) % kAzimuthBins;
		const auto a1 = (a0 + 1) % kAzimuthBins;
		const auto e0 = static_cast<int>(fy);
		const auto e1 = std::min(e0 + 1, kElevationBins - 1);
		const auto at = [&](int a_a, int a_e) { return radii[a_e * kAzimuthBins + a_a]; };
		return std::lerp(std::lerp(at(a0, e0), at(a1, e0), tx), std::lerp(at(a0, e1), at(a1, e1), tx), ty);
	}

	float Scalp::Height(const Vec3& a_point) const
	{
		const Vec3 d = a_point - centre;
		const float length = d.Length();
		if (length < 1e-6f)
			return -sphereRadius;
		return length - Radius(d / length);
	}

	Vec3 Scalp::Project(const Vec3& a_point) const
	{
		Vec3 d = a_point - centre;
		const float length = d.Length();
		d = length > 1e-6f ? d / length : kUnitZ;
		return centre + d * Radius(d);
	}

	namespace
	{
		constexpr float kWeldScale = 1000.0f;        // weld positions closer than 1/1000 unit
		constexpr float kSkullCentreOffset = 5.0f;   // head bone (skull base) to skull centre, up
		constexpr float kRootEntryThreshold = 0.3f;  // how squarely flow must enter a root edge
		constexpr float kFoldThreshold = -0.2f;      // neighbour normals this opposed end a strand
		constexpr float kShortHairLength = 1.0f;     // Auto seeding: short hair if half the traced length lies in card guides shorter than this
		constexpr uint32_t kMaxStepsPerStrand = 4096;
		constexpr uint32_t kMaxCrossingsPerStep = 64;  // triangles one step may cross (slivers)
		constexpr float kRootBudgetShare = 0.85f;      // of the card guide cap, the rest left for fill guides
		constexpr float kCoverageGap = 0.5f;           // a transparent stretch longer than this ends a strand
		constexpr uint32_t kCoverageProbeGrid = 4;     // barycentric grid probing a triangle's texels
		constexpr uint32_t kProbeCount = (kCoverageProbeGrid + 1) * (kCoverageProbeGrid + 2) / 2;
		constexpr float kIslandAxisRatio = 1.5f;       // Auto: a UV island flows along U only when this much longer that way
		constexpr float kIslandUVScale = 4096.0f;      // UVs closer than 1/4096 join one island
		constexpr float kFlowMapMinStrength = 0.3f;    // a shorter mean flow-map vector over a triangle gives no flow
		constexpr float kPaintedAxisCoherence = 0.2f;  // streaks less aligned than this leave a strip's axis to its shape
		constexpr uint32_t kFlowVoteCells = 64;        // texture cells a side, pooling the root-to-tip votes of every card sampling them
		constexpr float kSeamMinLength = 0.25f;        // welded seam length x agreement below which two islands turn independently
		constexpr float kSeamMinAgreement = 0.5f;      // and the share of it that must agree in sign
		constexpr float kConfidentVote = 0.3f;         // a piece's mean away-and-down preference (hanging free) that settles its direction by itself
		constexpr float kScalpShare = 0.1f;            // the innermost share of the hair's area, whose distance from the skull centre is the scalp's
		constexpr float kHangingFrom = 1.3f;           // hair hangs free from this many scalp radii out, fully from kHangingFull
		constexpr float kHangingFull = 1.8f;
		constexpr float kGuideSearchCell = 2.0f;        // grid cell for finding a strand's simulated guide
		constexpr int32_t kGuideSearchRings = 4;        // widest grid search before trying every guide
		constexpr float kHeadRadiusPercentile = 0.02f;  // share of strand points allowed inside the head collider
		constexpr float kMinHeadRadius = 2.0f;
		constexpr float kMaxHeadRadius = 10.0f;

		// Scalp fit (FitScalp).
		constexpr float kCapMinZ = -0.2f;            // directions this far below the skull centre's horizon do not shape the scalp
		constexpr float kScalpPercentile = 0.1f;     // the inner share of the hair in a direction that lies on the scalp
		constexpr float kScalpMinBinWeight = 0.02f;  // of a bin's share of the cap's area, below which the bin has no data
		constexpr float kScalpCentrePrior = 0.1f;    // pull of the sphere fit's centre towards the skull centre
		constexpr float kScalpMaxCentreShift = 4.0f;
		constexpr float kScalpBelowSphere = 0.8f;  // a direction's scalp radius stays within these multiples of the sphere's
		constexpr float kScalpAboveSphere = 1.15f;
		constexpr float kMinScalpRadius = 3.0f;
		constexpr float kMaxScalpRadius = 14.0f;

		// Binding card guides to the scalp (BindToScalp).
		constexpr float kAttachHeight = 1.25f;    // a card guide starting this close above the scalp grows from it
		constexpr float kMergeRadius = 4.0f;      // how far from hair already rooted a card guide may start and still continue it
		constexpr float kMergeAlignment = 0.3f;   // and how closely the two must run alike (cosine)
		constexpr float kMergeTurnCost = 3.0f;    // units of distance a full turn away costs when choosing what to continue
		constexpr float kContinueRadius = 2.5f;   // rooted hair ending this close to where a card guide starts carries on into it
		constexpr uint32_t kMergePasses = 8;      // hair continuing hair continuing hair...
		constexpr float kMaxBridgeHeight = 5.0f;  // a card guide left unattached this close to the scalp is joined to it directly
		constexpr float kMinMergedTail = 0.5f;    // a continuing card guide shorter than this adds nothing

		// Growing strands round card guides (BuildStrands).
		constexpr float kMinGuideSpacing = 0.4f;  // card width per card guide: clumpSize, clamped
		constexpr float kMaxGuideSpacing = 2.0f;
		constexpr float kRootBlend = 2.0f;          // arclength over which a strand's root offset blends into its clump offset
		constexpr float kMinCoverageShare = 0.35f;  // a sparsely painted card guide keeps at least this share of its strands
		constexpr float kFullCoverage = 0.6f;       // mean alpha along a card guide that counts as fully painted
		constexpr float kMaxUVOffset = 0.25f;
		constexpr float kScalpRootShare = 0.5f;   // strands rooted where their card guide starts; the rest along the stretch it lies on the scalp
		constexpr float kMaxRootAlong = 0.6f;     // and no further along it than this share of its length, so none is a stub
		constexpr float kRedundantRadius = 0.5f;  // x spacing: a card guide running this close to a longer one...
		constexpr float kRedundantShare = 0.8f;   // ...over this share of its length repeats it
		constexpr float kFragmentLength = 0.35f;  // a card guide shorter than this share of its sheet's long hair...
		constexpr float kFragmentRadius = 1.0f;   // ...running within this x spacing of a longer one...
		constexpr float kFragmentShare = 0.5f;    // ...over this share of its length is a fragment of it

		// Hair that is not loose (FindWoven, BindToScalp, LabelTriangles). Spreads are twice the
		// standard deviation along a piece's principal axes.
		constexpr float kContactRadius = 0.75f;     // sheets this close touch
		constexpr uint32_t kSideBins = 6;           // directions a piece's surface faces round an axis, folded over (a card's two faces are one)
		constexpr float kSideShare = 0.08f;         // of its area, facing one way, for that way to count
		constexpr uint32_t kTubeSides = 5;          // a braid or twist built as a tube faces every way round its length...
		constexpr float kTubeMaxWidth = 2.75f;      // ...is this thin (a ponytail rolled into one sheet is thicker)...
		constexpr float kTubeElongation = 1.5f;     // ...this much longer than thick...
		constexpr float kTubeAlignment = 0.6f;      // ...and its hair runs along it (a curl's spiral runs round it)
		constexpr float kTubeClosed = 0.75f;        // a tube surrounds its axis: of its cross-sections' directions, this share holds hair
		constexpr float kPlaitStreaks = 0.4f;       // a texture whose painted streaks agree less than this shows a plait...
		constexpr float kPlaitTubeMaxWidth = 4.5f;  // ...and a tube textured so may be this thick
		constexpr float kSliceLength = 1.5f;        // cross-sections, this thick
		constexpr uint32_t kSliceBins = 12;         // directions round the axis in a cross-section
		constexpr float kLobeMaxLength = 4.0f;      // a plait's lobes are short, curved pieces...
		constexpr uint32_t kLobeSides = 3;
		constexpr uint32_t kMinLobes = 4;        // ...several of them...
		constexpr float kLocalRadius = 3.0f;     // the hair round a point, for how wide a bundle is there...
		constexpr float kBraidWidth = 2.0f;      // ...a braid or plait this narrow (a sheaf of cards, a mass of curls, is wider)
		constexpr float kPlaitHanging = 0.3f;    // lobes make a plait if this share of them hangs free
		constexpr float kRingMaxWidth = 4.0f;    // a tie or band is a short ring round a tail
		constexpr float kRingFlatness = 0.6f;    // its height, at most this share of its width
		constexpr float kRingFacing = 0.4f;      // its surface faces out round its axis (a flat card faces along it)
		constexpr float kAbsorbShare = 0.6f;     // a small piece this much in contact with a braid is part of it
		constexpr float kHangingHeight = 2.0f;   // woven hair this far off the scalp hangs free (pinned up to half of it)...
		constexpr float kBelowHead = -0.6f;      // ...and so does any below the skull, down the neck and back (direction z from its centre)
		constexpr float kMinChainLength = 3.0f;  // shorter hanging braids stay with the head
		constexpr float kChainSegment = 2.0f;    // a chain's joint spacing, at least
		constexpr float kChainBin = 1.0f;        // arclength per centre-line sample
		constexpr float kGatherReach = 1.5f;     // hair ending this close to a braid on the head, or a chain's root, is gathered into it
		constexpr float kTuftStart = 0.7f;       // hair starting past this share of a chain's length grows from it
		constexpr float kTieRadius = 2.5f;       // tails starting this close together share a tie
		constexpr float kTieReach = 2.5f;        // rooted hair ending this close to where tails start runs into their tie
		constexpr uint32_t kMinTieFeeders = 4;   // card guides gathered into a tie, at least...
		constexpr uint32_t kMinTieTails = 6;     // a tie holds a tail of this many card guides at least (a few carrying on below others are a layer)
		constexpr float kMinTieSpread = 4.0f;    // ...coming from roots this far apart...
		constexpr float kTieConvergence = 2.0f;  // ...converging from roots this many times further apart than their tips...
		constexpr float kMaxTieHeight = 3.0f;    // ...at a tie this close to the scalp

		struct Triangle
		{
			std::array<uint32_t, 3> v{};                    // source vertex indices
			std::array<uint32_t, 3> w{};                    // welded vertex ids
			std::array<int32_t, 3> neighbor{ -1, -1, -1 };  // across edge i = (v[i], v[i+1])
			Vec3 normal;
			Vec3 flow;
			Vec3 dPdu;  // surface change per unit of U, and of V
			Vec3 dPdv;
			float area = 0.0f;
			uint64_t positionKey = 0;  // the same for a triangle and its back face
			int32_t component = -1;
			int32_t sameAs = -1;   // an earlier triangle on the same welded vertices (dropped as its copy)
			float streaks = 1.0f;  // how clearly the painted strands its UV island samples run one way (PaintedAxes coherence)
			bool fromFlowMap = false;
			bool valid = false;  // converted: flow, tracing, strands
		};

		/** A braid, twist, tie or bun: woven hair that stays cards, swinging on a chain when it hangs free. */
		struct WovenPiece
		{
			std::vector<uint32_t> tris;
			// Found without choices, the triangles of it chosen as something else: no longer its
			// own, but still where it lies (its chain runs through them, hair ending there is gathered).
			std::vector<uint32_t> chosenAway;
			Region region = Region::Cards;  // Cards or Chain
			int32_t chain = -1;
			int32_t line = -1;    // its centre line in lines when it hangs as a braid, chain or not (all of it chosen away)
			bool forced = false;  // chosen by hand
		};

		struct TraceSample
		{
			Vec3 position;
			uint32_t tri;
			float length;
		};

		/** One point of a card guide's path: on a card triangle, or (scalp connections) off it, taking that triangle's attributes. */
		struct PathSample
		{
			Vec3 position;
			uint32_t tri;
			float s;  // arclength from the path's start
		};

		/** A streamline traced along the cards, later extended to start on the scalp. Strands grow round it as a clump. */
		struct CardGuide
		{
			std::vector<PathSample> path;
			float coverage = 1.0f;  // mean texture alpha along the card part
			int32_t component = -1;
			GuideKind kind = GuideKind::Free;
			int32_t ancestor = -1;  // the rooted guide whose scalp root it shares
			int32_t tie = -1;       // Tied: the tie it grows from
		};

		struct PointAttributes
		{
			Vec2 uv;
			Vec3 normal;
			std::array<uint16_t, 4> bones{};
			std::array<float, 4> weights{};
		};

		Vec3 Barycentric(const Vec3& a_p, const Vec3& a_a, const Vec3& a_b, const Vec3& a_c)
		{
			const Vec3 v0 = a_b - a_a, v1 = a_c - a_a, v2 = a_p - a_a;
			const float d00 = v0.Dot(v0), d01 = v0.Dot(v1), d11 = v1.Dot(v1);
			const float d20 = v2.Dot(v0), d21 = v2.Dot(v1);
			const float denom = d00 * d11 - d01 * d01;
			if (std::abs(denom) < 1e-12f)
				return { 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f };
			const float v = (d11 * d20 - d01 * d21) / denom;
			const float w = (d00 * d21 - d01 * d20) / denom;
			return { 1.0f - v - w, v, w };
		}

		Vec3 ClampBarycentric(Vec3 a_b)
		{
			a_b.x = std::max(a_b.x, 0.0f);
			a_b.y = std::max(a_b.y, 0.0f);
			a_b.z = std::max(a_b.z, 0.0f);
			const float sum = a_b.x + a_b.y + a_b.z;
			return sum > 1e-6f ? a_b / sum : Vec3{ 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f };
		}

		uint32_t Mix(uint32_t a_x)
		{
			a_x ^= a_x >> 16;
			a_x *= 0x7FEB352Du;
			a_x ^= a_x >> 15;
			a_x *= 0x846CA68Bu;
			a_x ^= a_x >> 16;
			return a_x;
		}

		float Hash01(uint32_t a_x) { return (Mix(a_x) >> 8) * (1.0f / 16777216.0f); }

		/**
		 * Uniform numbers in [0, 1) drawn for one thing (a triangle, a root edge, a card guide) from
		 * its own key, not from a stream shared by the whole mesh: a region chosen elsewhere, which
		 * changes how many numbers other things draw, does not change these.
		 */
		class KeyedRandom
		{
		public:
			KeyedRandom(uint32_t a_stream, uint32_t a_key, uint32_t a_seed) :
				state(Mix(Mix(a_key ^ Mix(a_stream * 0x9E3779B9u)) ^ a_seed)) {}
			float operator()()
			{
				state = Mix(state + 0x9E3779B9u);
				return (state >> 8) * (1.0f / 16777216.0f);
			}

		private:
			uint32_t state;
		};

		float Smoothstep(float a_x)
		{
			a_x = std::clamp(a_x, 0.0f, 1.0f);
			return a_x * a_x * (3.0f - 2.0f * a_x);
		}

		uint64_t CellKey(int32_t a_x, int32_t a_y, int32_t a_z)
		{
			return (static_cast<uint64_t>(a_x) & 0x1FFFFF) | ((static_cast<uint64_t>(a_y) & 0x1FFFFF) << 21) | ((static_cast<uint64_t>(a_z) & 0x1FFFFF) << 42);
		}

		/** Points in a hashed grid, for neighbourhood queries. */
		template <class T>
		class PointGrid
		{
		public:
			explicit PointGrid(float a_cell) :
				cell(a_cell) {}

			void Insert(const Vec3& a_p, const T& a_value) { cells[Key(a_p)].push_back(a_value); }

			/** @brief Calls a_f(value) for every entry in the cells within a_radius of a_p. */
			template <class F>
			void Query(const Vec3& a_p, float a_radius, F&& a_f) const
			{
				const int32_t r = static_cast<int32_t>(std::ceil(a_radius / cell));
				const int32_t cx = Coord(a_p.x), cy = Coord(a_p.y), cz = Coord(a_p.z);
				for (int32_t dz = -r; dz <= r; ++dz)
					for (int32_t dy = -r; dy <= r; ++dy)
						for (int32_t dx = -r; dx <= r; ++dx)
							if (const auto it = cells.find(CellKey(cx + dx, cy + dy, cz + dz)); it != cells.end())
								for (const T& value : it->second)
									a_f(value);
			}

		private:
			int32_t Coord(float a_v) const { return static_cast<int32_t>(std::floor(a_v / cell)); }
			uint64_t Key(const Vec3& a_p) const { return CellKey(Coord(a_p.x), Coord(a_p.y), Coord(a_p.z)); }

			float cell;
			std::unordered_map<uint64_t, std::vector<T>> cells;
		};

		/** Eigenvalues (largest first) and unit eigenvectors of a symmetric 3x3 matrix, by Jacobi rotations. */
		void SymmetricEigen(std::array<std::array<double, 3>, 3> a_m, std::array<double, 3>& o_values, std::array<Vec3, 3>& o_vectors)
		{
			std::array<std::array<double, 3>, 3> v{ { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } } };
			for (int sweep = 0; sweep < 32; ++sweep) {
				if (std::abs(a_m[0][1]) + std::abs(a_m[0][2]) + std::abs(a_m[1][2]) < 1e-14)
					break;
				for (int p = 0; p < 2; ++p) {
					for (int q = p + 1; q < 3; ++q) {
						if (std::abs(a_m[p][q]) < 1e-18)
							continue;
						// Numerical Recipes' rotation: A' = J^T A J zeroes A'[p][q].
						const double theta = (a_m[q][q] - a_m[p][p]) / (2.0 * a_m[p][q]);
						const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
						const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
						for (int k = 0; k < 3; ++k) {
							const double kp = a_m[k][p], kq = a_m[k][q];
							a_m[k][p] = c * kp - s * kq;
							a_m[k][q] = s * kp + c * kq;
						}
						for (int k = 0; k < 3; ++k) {
							const double pk = a_m[p][k], qk = a_m[q][k];
							a_m[p][k] = c * pk - s * qk;
							a_m[q][k] = s * pk + c * qk;
						}
						for (int k = 0; k < 3; ++k) {
							const double kp = v[k][p], kq = v[k][q];
							v[k][p] = c * kp - s * kq;
							v[k][q] = s * kp + c * kq;
						}
					}
				}
			}
			std::array<int, 3> order{ 0, 1, 2 };
			std::ranges::sort(order, [&](int a, int b) { return a_m[a][a] > a_m[b][b]; });
			for (int i = 0; i < 3; ++i) {
				const int k = order[i];
				o_values[i] = a_m[k][k];
				o_vectors[i] = Vec3(static_cast<float>(v[0][k]), static_cast<float>(v[1][k]), static_cast<float>(v[2][k])).Normalized();
			}
		}

		/** True where hair hangs free of the head: well off the scalp, or below the skull (down the neck and back). */
		bool Hangs(const Scalp& a_scalp, const Vec3& a_p)
		{
			if (a_scalp.Height(a_p) >= kHangingHeight)
				return true;
			const Vec3 d = a_p - a_scalp.centre;
			const float length = d.Length();
			return length > 1e-6f && d.z / length < kBelowHead;
		}

		/** Union-find over indices. */
		class DisjointSets
		{
		public:
			explicit DisjointSets(size_t a_count) :
				parent(a_count)
			{
				std::iota(parent.begin(), parent.end(), 0u);
			}
			uint32_t Find(uint32_t a_x)
			{
				while (parent[a_x] != a_x)
					a_x = parent[a_x] = parent[parent[a_x]];
				return a_x;
			}
			void Unite(uint32_t a_a, uint32_t a_b) { parent[Find(a_a)] = Find(a_b); }

		private:
			std::vector<uint32_t> parent;
		};

		/** Solves a small dense linear system in place (Gaussian elimination, partial pivoting). */
		template <int N>
		bool Solve(std::array<std::array<double, N + 1>, N>& a_m, std::array<double, N>& o_x)
		{
			for (int c = 0; c < N; ++c) {
				int pivot = c;
				for (int r = c + 1; r < N; ++r)
					if (std::abs(a_m[r][c]) > std::abs(a_m[pivot][c]))
						pivot = r;
				if (std::abs(a_m[pivot][c]) < 1e-12)
					return false;
				std::swap(a_m[c], a_m[pivot]);
				for (int r = 0; r < N; ++r) {
					if (r == c)
						continue;
					const double f = a_m[r][c] / a_m[c][c];
					for (int k = c; k <= N; ++k)
						a_m[r][k] -= f * a_m[c][k];
				}
			}
			for (int r = 0; r < N; ++r)
				o_x[r] = a_m[r][N] / a_m[r][r];
			return true;
		}

		/**
		 * The direction the painted strands run across the hair texture: the structure tensor of
		 * its alpha-weighted luminance, smoothed over a few texels. Strands are streaks, so the
		 * brightness changes fastest across them.
		 */
		class PaintedAxes
		{
		public:
			explicit PaintedAxes(const CoverageImage& a_mask)
			{
				if (a_mask.width < 8 || a_mask.height < 8 || a_mask.shade.size() != static_cast<size_t>(a_mask.width) * a_mask.height)
					return;
				width = a_mask.width;
				height = a_mask.height;
				const size_t count = static_cast<size_t>(width) * height;
				xx.resize(count);
				xy.resize(count);
				yy.resize(count);
				const auto at = [&](uint32_t a_x, uint32_t a_y) { return a_mask.shade[static_cast<size_t>(a_y) * width + a_x] / 255.0f; };
				for (uint32_t y = 0; y < height; ++y) {
					for (uint32_t x = 0; x < width; ++x) {
						const float gx = at((x + 1) % width, y) - at((x + width - 1) % width, y);
						const float gy = at(x, (y + 1) % height) - at(x, (y + height - 1) % height);
						const size_t i = static_cast<size_t>(y) * width + x;
						xx[i] = gx * gx;
						xy[i] = gx * gy;
						yy[i] = gy * gy;
					}
				}
				for (auto* channel : { &xx, &xy, &yy })
					Smooth(*channel);
			}

			bool Empty() const { return xx.empty(); }

			/** @brief Adds the tensor (xx, xy, yy) at a texture coordinate, times a_weight, to io_sum. */
			void Accumulate(const Vec2& a_uv, float a_weight, Vec3& io_sum) const
			{
				if (!std::isfinite(a_uv.x) || !std::isfinite(a_uv.y))
					return;
				const auto x = std::min(static_cast<uint32_t>((a_uv.x - std::floor(a_uv.x)) * width), width - 1);
				const auto y = std::min(static_cast<uint32_t>((a_uv.y - std::floor(a_uv.y)) * height), height - 1);
				const size_t i = static_cast<size_t>(y) * width + x;
				io_sum += Vec3(xx[i], xy[i], yy[i]) * a_weight;
			}

			/**
			 * @brief The strand direction (either sign) in texture coordinates for a summed tensor.
			 * @param o_coherence How clearly the streaks agree: 0 none, 1 all parallel.
			 */
			Vec2 Axis(const Vec3& a_tensor, float& o_coherence) const
			{
				const float trace = a_tensor.x + a_tensor.z;
				const float spread = std::sqrt((a_tensor.x - a_tensor.z) * (a_tensor.x - a_tensor.z) + 4.0f * a_tensor.y * a_tensor.y);
				o_coherence = trace > 1e-12f ? spread / trace : 0.0f;
				const float across = 0.5f * std::atan2(2.0f * a_tensor.y, a_tensor.x - a_tensor.z);
				// At right angles to the gradient, from texels to texture units.
				Vec2 axis(-std::sin(across) / width, std::cos(across) / height);
				axis.Normalize();
				return axis;
			}

		private:
			/** Box filter over (2r+1)^2 texels, wrapping like the hair's sampler. */
			void Smooth(std::vector<float>& io_values) const
			{
				const auto radius = static_cast<int32_t>(std::max(1u, std::max(width, height) / 85));
				std::vector<float> line;
				const auto pass = [&](uint32_t a_count, uint32_t a_lines, size_t a_step, size_t a_lineStep) {
					const auto n = static_cast<int32_t>(a_count);
					const auto wrap = [&](int32_t a_i) { return static_cast<size_t>(((a_i % n) + n) % n) * a_step; };
					line.resize(a_count);
					for (uint32_t l = 0; l < a_lines; ++l) {
						float* values = io_values.data() + l * a_lineStep;
						float sum = 0.0f;
						for (int32_t k = -radius; k <= radius; ++k)
							sum += values[wrap(k)];
						for (uint32_t i = 0; i < a_count; ++i) {
							line[i] = sum;
							sum += values[wrap(static_cast<int32_t>(i) + radius + 1)] - values[wrap(static_cast<int32_t>(i) - radius)];
						}
						for (uint32_t i = 0; i < a_count; ++i)
							values[i * a_step] = line[i];
					}
				};
				pass(width, height, 1, width);
				pass(height, width, width, 1);
			}

			uint32_t width = 0;
			uint32_t height = 0;
			std::vector<float> xx, xy, yy;
		};

		class Generator
		{
		public:
			Generator(const CardMesh& a_mesh, const Settings& a_settings) :
				mesh(a_mesh), settings(a_settings), rng(a_settings.seed * 0x9E3779B9u + static_cast<uint32_t>(a_mesh.positions.size())),
				useCoverage(!a_mesh.coverage.Empty() && a_settings.coverageThreshold > 0.0f),
				spacing(std::clamp(a_settings.clumpSize, kMinGuideSpacing, kMaxGuideSpacing))
			{}

			bool Run(Result& o_result, std::string& o_error);

		private:
			// Card analysis.
			void Weld();
			void BuildTriangles();
			void BuildAdjacency();
			void BuildComponents();
			void DropBackFaces();
			int32_t FindHeadBone() const;
			Vec3 FindHeadCentre() const;
			/**
			 * With FlowAxis::Auto, sets each triangle's root-to-tip flow: from the flow map where
			 * it has a direction, elsewhere from the part of the texture each card samples. Returns
			 * the share of the card area the flow map set.
			 */
			float ChooseFlow();
			void BuildVertexFields();

			template <class F>
			void ForEachProbe(const Triangle& a_tri, F&& a_f) const;
			Vec3 SurfaceDirection(const Triangle& a_tri, const Vec2& a_uv) const;
			Vec3 FlowAt(const Triangle& a_tri, const Vec3& a_bary) const;
			Vec3 Position(uint32_t a_vertex) const { return mesh.positions[a_vertex]; }

			/**
			 * Follows the flow (sign +1) or against it (-1) from a point on a triangle. Calls
			 * a_onSample(position, triangle, length) at the start, after every step and at the
			 * end. Returns the length travelled; o_end* receive where it stopped.
			 */
			template <class F>
			float Trace(uint32_t a_tri, Vec3 a_pos, float a_sign, float a_maxLength, F&& a_onSample, uint32_t* o_endTri = nullptr, Vec3* o_endPos = nullptr) const;

			// Card guides.
			void SeedRoots();
			void SeedFill();
			void SeedArea(const Scalp& a_scalp);
			/**
			 * Traces a card guide from a point and keeps it if long enough. With a coverage mask
			 * the guide is cut to the painted part of its streamline; a_findCoverage lets that part
			 * start downstream of the seed (roots on a transparent card edge), otherwise the seed
			 * itself must be painted.
			 */
			bool TryAddGuide(uint32_t a_tri, const Vec3& a_pos, float a_maxLength, bool a_countVisits, bool a_findCoverage);
			float Coverage(uint32_t a_tri, const Vec3& a_pos) const;
			bool Covered(uint32_t a_tri, const Vec3& a_pos) const { return !useCoverage || Coverage(a_tri, a_pos) >= settings.coverageThreshold; }
			bool HasCoverage(uint32_t a_tri) const;

			/**
			 * Drops card guides that run alongside a longer one on the same sheet, within half a spacing, for most of
			 * their length: fill streamlines between two root guides, fragments starting on a
			 * card's side edge. Each kept guide then stands for `spacing` of card width.
			 */
			void PruneRedundant(Stats& o_stats);

			// The scalp, and binding card guides to it.
			void FitScalp(Scalp& o_scalp) const;
			void BindToScalp(const Scalp& a_scalp, Stats& o_stats);
			/**
			 * Before binding: card guides ending at woven hair on the head (or at a chain's root) are
			 * gathered into it, and floating guides starting at woven hair grow from it (a braid's tuft).
			 */
			void BindToWoven(const Scalp& a_scalp, std::vector<uint32_t>& io_floating);
			/**
			 * Finds ponytail ties: a floating card guide that bound hair from across the scalp ends
			 * at, close to the scalp. That hair is gathered into the tie and stays cards; the tail
			 * grows strands from the tie.
			 */
			void FindTies(const Scalp& a_scalp, std::vector<uint32_t>& io_floating);
			/**
			 * Card guides with an end within reach of an attach point (Settings::attachPoints) grow
			 * from it, by that end: Tied, their path reversed if need be and starting at the point.
			 * o_pointOf gets each guide's point, -1 for none.
			 */
			void AttachGuides(std::vector<int32_t>& o_pointOf);
			/** @brief A tie for each attach point that has hair, holding that hair. */
			void TieAttached(const std::vector<int32_t>& a_pointOf);
			/** @brief True if most of a card guide's own path lies over triangles chosen as Strands: it is never gathered. */
			bool ChosenAsStrands(uint32_t a_guide) const;

			// Hair that is not loose.
			/** @brief Each triangle's region as chosen by hand: Settings::triangleRegions, excludeUV, chainUV. */
			void ResolveChoices();
			/**
			 * Finds woven hair: braids and twists built as tubes or as stacked lobes, ties and bands,
			 * buns, and the triangles chosen by hand. Each connected piece of it stays cards, on a
			 * chain when it hangs free of the scalp. Its triangles take no further part in the
			 * conversion (no flow tracing, no strands).
			 */
			void FindWoven(const Scalp& a_scalp);
			/** @brief A hanging piece's centre line as a chain; false if it is too short to swing. */
			bool BuildChain(const WovenPiece& a_piece, const Scalp& a_scalp, ChainCurve& o_chain) const;
			/** @brief Arclength from a chain's root to its point nearest a_p; o_length is the chain's length, o_pinned its pinned part's. */
			float ChainPosition(const ChainCurve& a_chain, const Vec3& a_p, float& o_length, float& o_pinned) const;
			/** @brief The bones (chain joints, by bone number) and weights a point hanging from a chain follows. */
			void ChainSkin(uint32_t a_chain, const Vec3& a_p, std::array<uint16_t, 4>& o_bones, std::array<float, 4>& o_weights) const;
			/** @brief Each mesh triangle's region, from the woven pieces and the card guides as bound. */
			void LabelTriangles(Result& o_result);
			/** @brief The triangles kept as cards, as a mesh to draw. */
			void BuildCards(Result& o_result) const;
			uint16_t DominantBone(uint32_t a_tri) const;
			bool IsWoven(uint32_t a_tri) const { return a_tri < wovenOf.size() && wovenOf[a_tri] >= 0; }
			Vec3 InitialTangent(const CardGuide& a_guide) const;
			void Recompute(std::vector<PathSample>& io_path) const;

			// Strands.
			PointAttributes Attributes(uint32_t a_tri, const Vec3& a_pos) const;
			Vec3 LiftNormal(uint32_t a_tri, const Vec3& a_pos) const;
			void BuildStrands(const Scalp& a_scalp, Result& o_result);
			void Shuffle(Result& o_result);
			/** @brief Picks the simulated guide strands (a prefix of the shuffled list) and the guide each strand follows. */
			void AssignGuides(Result& o_result) const;
			/** @brief Fits the head collider: a sphere round the skull centre, just inside the strands. */
			void FitHeadCollider(Result& o_result) const;

			const CardMesh& mesh;
			const Settings& settings;
			std::mt19937 rng;
			const bool useCoverage;
			const float spacing;

			std::vector<uint32_t> weldId;      // per source vertex
			std::vector<uint32_t> positionId;  // per source vertex, ignoring normals
			uint32_t weldedCount = 0;
			std::vector<Triangle> tris;
			std::vector<Vec3> weldedFlow;
			std::vector<Vec3> weldedNormal;   // geometric, area weighted
			std::vector<Vec3> shadingNormal;  // per source vertex: authored, else geometric
			Vec3 headCentre;
			float step = 0.5f;

			std::vector<uint16_t> visits;      // card guides through each triangle
			std::vector<uint32_t> visitStamp;  // last guide counted per triangle
			uint32_t currentStamp = 0;
			std::vector<uint32_t> crossed;
			std::vector<TraceSample> samples;          // the last traced streamline
			mutable std::vector<uint32_t> traceStamp;  // last trace that entered each triangle
			mutable uint32_t traceId = 0;

			std::vector<CardGuide> guides;
			uint32_t guideBudget = Limits::kMaxCardGuides;

			// Hair that is not loose.
			std::vector<RegionChoice> choice;  // per triangle
			std::vector<Vec3> triCentre;
			std::vector<float> triHeight;  // above the scalp
			std::vector<bool> triHangs;    // hangs free of the head (Hangs)
			std::vector<int32_t> wovenOf;  // per triangle: its woven piece, -1 for none
			std::vector<WovenPiece> woven;
			std::vector<ChainCurve> chains;
			std::vector<ChainCurve> lines;  // hanging pieces' centre lines: every chain's, and those of pieces chosen away
			std::vector<Tie> ties;
			uint32_t chainBoneBase = 0;
			std::vector<std::vector<PathSample>> ownPaths;  // each card guide's own path, before binding extends it
			std::vector<Region> regionOf;                   // per triangle (LabelTriangles)
			std::vector<int32_t> chainOf;
		};

		void Generator::Weld()
		{
			const auto count = static_cast<uint32_t>(mesh.positions.size());
			weldId.resize(count);
			positionId.resize(count);
			std::unordered_map<uint64_t, uint32_t> positions;
			positions.reserve(count);
			std::vector<std::vector<uint32_t>> sides;  // per position, the first vertex of each side
			uint32_t positionCount = 0;
			for (uint32_t v = 0; v < count; ++v) {
				const auto& p = mesh.positions[v];
				const auto qx = static_cast<int64_t>(std::lround(p.x * kWeldScale)) & 0x1FFFFF;
				const auto qy = static_cast<int64_t>(std::lround(p.y * kWeldScale)) & 0x1FFFFF;
				const auto qz = static_cast<int64_t>(std::lround(p.z * kWeldScale)) & 0x1FFFFF;
				const uint64_t positionKey = static_cast<uint64_t>(qx) | (static_cast<uint64_t>(qy) << 21) | (static_cast<uint64_t>(qz) << 42);
				positionId[v] = positions.try_emplace(positionKey, positionCount).first->second;
				if (positionId[v] == positionCount)
					++positionCount;

				// Back faces of double-sided cards share positions but face the other way; keep
				// them apart so each side stays a manifold sheet. A vertex joins the side at its
				// position that faces its way: comparing normals with each other, not with fixed
				// axes, so a card that curves round the head stays one sheet.
				if (positionId[v] >= sides.size())
					sides.resize(positionId[v] + 1);
				auto& positionSides = sides[positionId[v]];
				const auto side = std::ranges::find_if(positionSides, [&](uint32_t a_first) { return mesh.normals.empty() || mesh.normals[a_first].Dot(mesh.normals[v]) > 0.0f; });
				if (side != positionSides.end()) {
					weldId[v] = weldId[*side];
				} else {
					weldId[v] = weldedCount++;
					positionSides.push_back(v);
				}
			}
		}

		void Generator::BuildTriangles()
		{
			const uint32_t triCount = mesh.TriangleCount();
			tris.resize(triCount);
			std::unordered_map<uint64_t, uint32_t> seen;
			seen.reserve(triCount);

			for (uint32_t t = 0; t < triCount; ++t) {
				auto& tri = tris[t];
				bool inRange = true;
				for (int i = 0; i < 3; ++i) {
					tri.v[i] = mesh.indices[t * 3 + i];
					inRange = inRange && tri.v[i] < mesh.positions.size();
				}
				if (!inRange)
					continue;
				for (int i = 0; i < 3; ++i)
					tri.w[i] = weldId[tri.v[i]];
				const Vec3 a = Position(tri.v[0]), b = Position(tri.v[1]), c = Position(tri.v[2]);
				Vec3 n = (b - a).Cross(c - a);
				const float twiceArea = n.Length();
				if (twiceArea < 1e-8f)
					continue;
				tri.normal = n / twiceArea;
				tri.area = 0.5f * twiceArea;

				// A back face is converted once. One on the front's own welded vertices is the same
				// sheet: keep its first copy. One on vertices of its own is a sheet of its own,
				// dropped a whole side at a time (DropBackFaces).
				const auto tripleKey = [](std::array<uint32_t, 3> a_ids) {
					std::ranges::sort(a_ids);
					return (static_cast<uint64_t>(a_ids[0]) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(a_ids[1]) * 0xC2B2AE3D27D4EB4Full) ^ (static_cast<uint64_t>(a_ids[2]) * 0x165667B19E3779F9ull);
				};
				tri.positionKey = tripleKey({ positionId[tri.v[0]], positionId[tri.v[1]], positionId[tri.v[2]] });
				if (const auto [it, inserted] = seen.try_emplace(tripleKey(tri.w), t); !inserted) {
					tri.sameAs = static_cast<int32_t>(it->second);
					continue;
				}

				const auto& ua = mesh.uvs[tri.v[0]];
				const auto& ub = mesh.uvs[tri.v[1]];
				const auto& uc = mesh.uvs[tri.v[2]];

				// Solve dP = dPdu du + dPdv dv over the triangle's two edges.
				const Vec3 e1 = b - a, e2 = c - a;
				const float du1 = ub.x - ua.x, dv1 = ub.y - ua.y, du2 = uc.x - ua.x, dv2 = uc.y - ua.y;
				const float det = du1 * dv2 - du2 * dv1;
				if (std::abs(det) < 1e-10f)
					continue;
				const Vec3 dPdu = (e1 * dv2 - e2 * dv1) / det;
				const Vec3 dPdv = (e2 * du1 - e1 * du2) / det;
				Vec3 flow;
				switch (settings.flowAxis) {
				case FlowAxis::NegV:
					flow = -dPdv;
					break;
				case FlowAxis::U:
					flow = dPdu;
					break;
				case FlowAxis::NegU:
					flow = -dPdu;
					break;
				default:
					flow = dPdv;
					break;
				}
				flow -= tri.normal * flow.Dot(tri.normal);
				const float length = flow.Length();
				if (length < 1e-6f)
					continue;
				tri.flow = flow / length;
				tri.dPdu = dPdu;
				tri.dPdv = dPdv;
				tri.valid = true;
			}
		}

		template <class F>
		void Generator::ForEachProbe(const Triangle& a_tri, F&& a_f) const
		{
			const Vec2 a = mesh.uvs[a_tri.v[0]], b = mesh.uvs[a_tri.v[1]], c = mesh.uvs[a_tri.v[2]];
			for (uint32_t i = 0; i <= kCoverageProbeGrid; ++i) {
				for (uint32_t j = 0; i + j <= kCoverageProbeGrid; ++j) {
					const float u = static_cast<float>(i) / kCoverageProbeGrid, v = static_cast<float>(j) / kCoverageProbeGrid;
					a_f(a * (1.0f - u - v) + b * u + c * v);
				}
			}
		}

		Vec3 Generator::SurfaceDirection(const Triangle& a_tri, const Vec2& a_uv) const
		{
			Vec3 dir = a_tri.dPdu * a_uv.x + a_tri.dPdv * a_uv.y;
			dir -= a_tri.normal * dir.Dot(a_tri.normal);
			const float length = dir.Length();
			return length > 1e-6f ? dir / length : kZero;
		}

		float Generator::ChooseFlow()
		{
			if (settings.flowAxis != FlowAxis::Auto)
				return 0.0f;

			// A flow map points from tip to root in texture space (as Hair Specular reads it).
			// Where its mean over a triangle has a clear direction, that is the flow.
			float mapArea = 0.0f, totalArea = 0.0f;
			for (auto& tri : tris) {
				if (!tri.valid)
					continue;
				totalArea += tri.area;
				if (mesh.flow.Empty())
					continue;
				Vec2 sum;
				ForEachProbe(tri, [&](const Vec2& a_uv) { sum += mesh.flow.Sample(a_uv.x, a_uv.y); });
				const Vec2 mean = sum / static_cast<float>(kProbeCount);
				const Vec3 dir = SurfaceDirection(tri, -mean);
				if (mean.Length() < kFlowMapMinStrength || dir == kZero)
					continue;
				tri.flow = dir;
				tri.fromFlowMap = true;
				mapArea += tri.area;
			}

			// Everywhere else the flow comes from the part of the texture a card samples. Each UV
			// island is one card's strip of the atlas; vertices sharing a position and a UV are
			// one island vertex.
			const auto count = static_cast<uint32_t>(mesh.positions.size());
			std::vector<uint32_t> parent(count);
			std::iota(parent.begin(), parent.end(), 0u);
			const auto find = [&](uint32_t a_x) {
				while (parent[a_x] != a_x)
					a_x = parent[a_x] = parent[parent[a_x]];
				return a_x;
			};
			const auto unite = [&](uint32_t a_a, uint32_t a_b) { parent[find(a_a)] = find(a_b); };
			std::unordered_map<uint64_t, uint32_t> firstVertex;
			firstVertex.reserve(count);
			for (uint32_t v = 0; v < count; ++v) {
				const auto qu = static_cast<uint64_t>(std::lround(mesh.uvs[v].x * kIslandUVScale)) & 0xFFFFF;
				const auto qv = static_cast<uint64_t>(std::lround(mesh.uvs[v].y * kIslandUVScale)) & 0xFFFFF;
				const auto [it, inserted] = firstVertex.try_emplace((static_cast<uint64_t>(positionId[v]) << 40) | (qu << 20) | qv, v);
				if (!inserted)
					unite(v, it->second);
			}
			for (const auto& tri : tris) {
				if (tri.valid) {
					unite(tri.v[0], tri.v[1]);
					unite(tri.v[1], tri.v[2]);
				}
			}

			struct Island
			{
				float unitsPerU = 0.0f;  // area weighted sums
				float unitsPerV = 0.0f;
				float uMin = FLT_MAX, uMax = -FLT_MAX, vMin = FLT_MAX, vMax = -FLT_MAX;
				Vec3 tensor;  // the painted streaks it samples, area weighted
				Vec2 axis;    // root to tip in texture space, once chosen
				float streaks = 1.0f;
			};
			std::unordered_map<uint32_t, uint32_t> islandIds;
			std::vector<Island> islands;
			std::vector<uint32_t> islandOf(tris.size(), 0);
			const PaintedAxes painted(mesh.coverage);
			for (uint32_t t = 0; t < tris.size(); ++t) {
				const auto& tri = tris[t];
				if (!tri.valid)
					continue;
				const auto [it, inserted] = islandIds.try_emplace(find(tri.v[0]), static_cast<uint32_t>(islands.size()));
				if (inserted)
					islands.emplace_back();
				islandOf[t] = it->second;
				auto& island = islands[it->second];
				island.unitsPerU += tri.dPdu.Length() * tri.area;
				island.unitsPerV += tri.dPdv.Length() * tri.area;
				for (uint32_t v : tri.v) {
					island.uMin = std::min(island.uMin, mesh.uvs[v].x);
					island.uMax = std::max(island.uMax, mesh.uvs[v].x);
					island.vMin = std::min(island.vMin, mesh.uvs[v].y);
					island.vMax = std::max(island.vMax, mesh.uvs[v].y);
				}
				if (!painted.Empty())
					ForEachProbe(tri, [&](const Vec2& a_uv) { painted.Accumulate(a_uv, tri.area / kProbeCount, island.tensor); });
			}

			// The axis is the way the painted strands run in the island's part of the texture.
			// Without clear streaks (or a texture to read), atlases lay most strips along V, but
			// not all (vanilla Skyrim's long strip runs along U): the island follows whichever
			// axis it is clearly longer along on the surface.
			for (auto& island : islands) {
				float coherence = 0.0f;
				const Vec2 axis = painted.Empty() ? Vec2{} : painted.Axis(island.tensor, coherence);
				island.streaks = painted.Empty() ? 1.0f : coherence;
				if (coherence >= kPaintedAxisCoherence) {
					island.axis = axis;
				} else {
					const float lengthU = (island.uMax - island.uMin) * island.unitsPerU;
					const float lengthV = (island.vMax - island.vMin) * island.unitsPerV;
					island.axis = lengthU > kIslandAxisRatio * lengthV ? Vec2(1.0f, 0.0f) : Vec2(0.0f, 1.0f);
				}
			}

			// Which way along the axis is root to tip. Across a welded seam hair continues, or
			// runs beside its neighbour, the same way (or away from a parting), so islands joined
			// by seams form pieces that turn together. Per pair of islands: seam length x the
			// agreement of their axes across it, and length x its size.
			std::unordered_map<uint64_t, Vec2> seams;
			for (uint32_t t = 0; t < tris.size(); ++t) {
				const auto& tri = tris[t];
				if (!tri.valid)
					continue;
				for (int i = 0; i < 3; ++i) {
					const int32_t n = tri.neighbor[i];
					if (n <= static_cast<int32_t>(t) || !tris[n].valid || islandOf[n] == islandOf[t])
						continue;
					const float agreement = SurfaceDirection(tri, islands[islandOf[t]].axis).Dot(SurfaceDirection(tris[n], islands[islandOf[n]].axis));
					const float length = (Position(tri.v[i]) - Position(tri.v[(i + 1) % 3])).Length();
					const uint64_t key = (static_cast<uint64_t>(std::min(islandOf[t], islandOf[n])) << 32) | std::max(islandOf[t], islandOf[n]);
					seams[key] += Vec2(length * agreement, length * std::abs(agreement));
				}
			}
			// Strongest seams first, each island with its sign relative to its parent. Seams
			// where the flow mostly crosses (or the sign keeps changing) join nothing.
			std::vector<uint32_t> pieceParent(islands.size());
			std::iota(pieceParent.begin(), pieceParent.end(), 0u);
			std::vector<float> parity(islands.size(), 1.0f);
			const auto root = [&](uint32_t a_island) {
				float sign = 1.0f;
				while (pieceParent[a_island] != a_island) {
					sign *= parity[a_island];
					a_island = pieceParent[a_island];
				}
				return std::pair{ a_island, sign };
			};
			std::vector<std::pair<float, uint64_t>> joins;
			for (const auto& [key, seam] : seams) {
				if (std::abs(seam.x) >= kSeamMinLength && std::abs(seam.x) >= kSeamMinAgreement * seam.y)
					joins.emplace_back(std::abs(seam.x), key);
			}
			std::ranges::sort(joins, std::greater{});
			for (const auto& [strength, key] : joins) {
				const auto [a, aSign] = root(static_cast<uint32_t>(key >> 32));
				const auto [b, bSign] = root(static_cast<uint32_t>(key & 0xFFFFFFFFu));
				if (a == b)
					continue;
				pieceParent[b] = a;
				parity[b] = (seams[key].x < 0.0f ? -1.0f : 1.0f) * aSign * bSign;
			}
			std::vector<uint32_t> pieceOf(islands.size());
			std::vector<float> islandSign(islands.size());  // relative to its piece
			for (uint32_t i = 0; i < islands.size(); ++i)
				std::tie(pieceOf[i], islandSign[i]) = root(i);

			// Hair runs away from the head and, on balance, downwards. That is only sure where it
			// hangs free, clear of the scalp: on the scalp and the nape it runs either way (combed
			// down from the crown, or up into a tie). A piece on the flow map follows the map, and
			// one that clearly hangs free one way keeps that way.
			std::vector<std::pair<float, float>> distances;  // skull centre to each triangle, and its area
			for (const auto& tri : tris) {
				if (tri.valid)
					distances.emplace_back(((Position(tri.v[0]) + Position(tri.v[1]) + Position(tri.v[2])) / 3.0f - headCentre).Length(), tri.area);
			}
			std::ranges::sort(distances);
			float scalpRadius = 0.0f, innerArea = 0.0f;
			for (const auto& [distance, area] : distances) {
				scalpRadius = distance;
				if ((innerArea += area) >= kScalpShare * totalArea)
					break;
			}
			struct Piece
			{
				float mapVote = 0.0f;
				float ownVote = 0.0f;      // area weighted preference for the piece's sense of its axes
				float hangingVote = 0.0f;  // the same where it hangs free
				float area = 0.0f;
				float cellVote = 0.0f;  // the same from decided pieces sampling its texels, and its scale
				float cellWeight = 0.0f;
				float sign = 0.0f;  // +1 or -1 once decided
			};
			std::vector<Piece> pieces(islands.size());
			for (uint32_t t = 0; t < tris.size(); ++t) {
				const auto& tri = tris[t];
				if (!tri.valid)
					continue;
				auto& piece = pieces[pieceOf[islandOf[t]]];
				const Vec3 dir = SurfaceDirection(tri, islands[islandOf[t]].axis * islandSign[islandOf[t]]);
				piece.area += tri.area;
				if (tri.fromFlowMap) {
					piece.mapVote += tri.area * tri.flow.Dot(dir);
					continue;
				}
				const Vec3 centre = (Position(tri.v[0]) + Position(tri.v[1]) + Position(tri.v[2])) / 3.0f;
				Vec3 radial = centre - headCentre;
				const float distance = radial.Length();
				radial.Normalize();
				const float vote = tri.area * (dir.Dot(radial) - 0.5f * dir.z);
				const float hanging = scalpRadius > 0.0f ? std::clamp((distance / scalpRadius - kHangingFrom) / (kHangingFull - kHangingFrom), 0.0f, 1.0f) : 1.0f;
				piece.ownVote += vote;
				piece.hangingVote += vote * hanging;
			}
			for (auto& piece : pieces) {
				if (piece.mapVote != 0.0f)
					piece.sign = piece.mapVote < 0.0f ? -1.0f : 1.0f;
				else if (std::abs(piece.hangingVote) >= kConfidentVote * piece.area && piece.area > 0.0f)
					piece.sign = piece.hangingVote < 0.0f ? -1.0f : 1.0f;
			}

			// A piece whose shape says little (lying on the crown, rising from the hairline)
			// follows the decided pieces that sample the same texels, which show the same painted
			// strands: each votes in the texture cells it samples. Only decided pieces vote: a
			// dense scalp texture with no visible root or tip gets mapped either way up, and
			// weak votes from its cards are noise. With nobody to follow, the weak vote stands.
			std::vector<Vec2> cells(kFlowVoteCells * kFlowVoteCells);
			const auto cellOf = [&](const Triangle& a_tri) {
				const Vec2 uv = (mesh.uvs[a_tri.v[0]] + mesh.uvs[a_tri.v[1]] + mesh.uvs[a_tri.v[2]]) / 3.0f;
				if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
					return size_t{ 0 };
				const auto x = std::min(static_cast<uint32_t>((uv.x - std::floor(uv.x)) * kFlowVoteCells), kFlowVoteCells - 1);
				const auto y = std::min(static_cast<uint32_t>((uv.y - std::floor(uv.y)) * kFlowVoteCells), kFlowVoteCells - 1);
				return static_cast<size_t>(y) * kFlowVoteCells + x;
			};
			for (uint32_t t = 0; t < tris.size(); ++t) {
				const uint32_t i = islandOf[t];
				if (tris[t].valid && pieces[pieceOf[i]].sign != 0.0f)
					cells[cellOf(tris[t])] += islands[i].axis * (islandSign[i] * pieces[pieceOf[i]].sign * tris[t].area);
			}
			for (uint32_t t = 0; t < tris.size(); ++t) {
				const uint32_t i = islandOf[t];
				auto& piece = pieces[pieceOf[i]];
				if (!tris[t].valid || piece.sign != 0.0f)
					continue;
				const Vec2& cell = cells[cellOf(tris[t])];
				piece.cellVote += tris[t].area * islandSign[i] * islands[i].axis.Dot(cell);
				piece.cellWeight += tris[t].area * cell.Length();
			}
			for (auto& piece : pieces) {
				if (piece.sign != 0.0f || piece.area <= 0.0f)
					continue;
				// Own shape: the mean preference, under kConfidentVote here. Texels: -1 to 1.
				const float vote = piece.ownVote / piece.area + (piece.cellWeight > 0.0f ? piece.cellVote / piece.cellWeight : 0.0f);
				piece.sign = vote < 0.0f ? -1.0f : 1.0f;
			}
			for (uint32_t i = 0; i < islands.size(); ++i) {
				if (islandSign[i] * pieces[pieceOf[i]].sign < 0.0f)
					islands[i].axis = -islands[i].axis;
			}

			for (uint32_t t = 0; t < tris.size(); ++t) {
				auto& tri = tris[t];
				if (!tri.valid)
					continue;
				tri.streaks = islands[islandOf[t]].streaks;
				if (tri.fromFlowMap)
					continue;
				if (const Vec3 dir = SurfaceDirection(tri, islands[islandOf[t]].axis); dir != kZero)
					tri.flow = dir;
			}
			return totalArea > 0.0f ? mapArea / totalArea : 0.0f;
		}

		void Generator::BuildAdjacency()
		{
			std::unordered_map<uint64_t, std::pair<uint32_t, int>> edges;
			edges.reserve(tris.size() * 2);
			for (uint32_t t = 0; t < tris.size(); ++t) {
				auto& tri = tris[t];
				if (!tri.valid)
					continue;
				for (int i = 0; i < 3; ++i) {
					const uint32_t a = tri.w[i], b = tri.w[(i + 1) % 3];
					if (a == b)
						continue;
					const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
					auto [it, inserted] = edges.try_emplace(key, t, i);
					if (inserted)
						continue;
					auto& [other, otherEdge] = it->second;
					if (other == UINT32_MAX)
						continue;  // non-manifold: keep the first pair only
					if (tris[other].neighbor[otherEdge] < 0 && tri.neighbor[i] < 0) {
						tris[other].neighbor[otherEdge] = static_cast<int32_t>(t);
						tri.neighbor[i] = static_cast<int32_t>(other);
					}
					other = UINT32_MAX;
				}
			}
		}

		void Generator::BuildComponents()
		{
			int32_t next = 0;
			std::vector<uint32_t> stack;
			for (uint32_t t = 0; t < tris.size(); ++t) {
				if (!tris[t].valid || tris[t].component >= 0)
					continue;
				stack.push_back(t);
				tris[t].component = next;
				while (!stack.empty()) {
					const uint32_t cur = stack.back();
					stack.pop_back();
					for (int32_t n : tris[cur].neighbor) {
						if (n >= 0 && tris[n].valid && tris[n].component < 0) {
							tris[n].component = next;
							stack.push_back(static_cast<uint32_t>(n));
						}
					}
				}
				++next;
			}
		}

		void Generator::DropBackFaces()
		{
			// Double-sided cards list every triangle twice, facing both ways, and the weld keeps
			// the two sides apart as separate sheets. Decide per sheet: drop a sheet that mostly
			// repeats earlier kept ones, keep any other whole, overlap included. Dropping copy by
			// copy leaves holes wherever a mesh interleaves the sides or a sheet shares a band with
			// another's back, and every strand crossing a hole stops there.
			int32_t componentCount = 0;
			for (const auto& tri : tris)
				componentCount = std::max(componentCount, tri.component + 1);
			std::vector<std::vector<uint32_t>> members(componentCount);
			for (uint32_t t = 0; t < tris.size(); ++t) {
				if (tris[t].valid)
					members[tris[t].component].push_back(t);
			}

			std::unordered_set<uint64_t> kept;
			kept.reserve(tris.size());
			for (const auto& sheet : members) {
				float area = 0.0f, repeated = 0.0f;
				for (uint32_t t : sheet) {
					area += tris[t].area;
					repeated += kept.contains(tris[t].positionKey) ? tris[t].area : 0.0f;
				}
				const bool dropSheet = repeated > 0.5f * area;
				for (uint32_t t : sheet) {
					if (dropSheet)
						tris[t].valid = false;
					else
						kept.insert(tris[t].positionKey);
				}
			}
		}

		int32_t Generator::FindHeadBone() const
		{
			for (size_t b = 0; b < mesh.boneNames.size() && b < mesh.boneBindPositions.size(); ++b) {
				std::string name = mesh.boneNames[b];
				std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (name.find("npc head") != std::string::npos)
					return static_cast<int32_t>(b);
			}
			return -1;
		}

		Vec3 Generator::FindHeadCentre() const
		{
			if (const int32_t bone = FindHeadBone(); bone >= 0)
				return mesh.boneBindPositions[bone] + Vec3(0.0f, 0.0f, kSkullCentreOffset);
			// No head bone (wigs on odd skeletons): a point just under the top of the mesh.
			Vec3 lo(FLT_MAX), hi(-FLT_MAX);
			for (const auto& p : mesh.positions) {
				lo = Vec3::Min(lo, p);
				hi = Vec3::Max(hi, p);
			}
			return { 0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), hi.z - 8.0f };
		}

		void Generator::BuildVertexFields()
		{
			weldedFlow.assign(weldedCount, kZero);
			weldedNormal.assign(weldedCount, kZero);
			for (const auto& tri : tris) {
				if (!tri.valid)
					continue;
				for (uint32_t w : tri.w) {
					weldedFlow[w] += tri.flow * tri.area;
					weldedNormal[w] += tri.normal * tri.area;
				}
			}
			for (auto& n : weldedNormal)
				n.Normalize();

			shadingNormal.resize(mesh.positions.size());
			for (size_t v = 0; v < mesh.positions.size(); ++v) {
				shadingNormal[v] = mesh.normals.size() == mesh.positions.size() ? mesh.normals[v] : weldedNormal[weldId[v]];
				if (shadingNormal[v].LengthSquared() < 1e-8f)
					shadingNormal[v] = kUnitZ;
			}
		}

		Vec3 Generator::FlowAt(const Triangle& a_tri, const Vec3& a_bary) const
		{
			Vec3 flow = weldedFlow[a_tri.w[0]] * a_bary.x + weldedFlow[a_tri.w[1]] * a_bary.y + weldedFlow[a_tri.w[2]] * a_bary.z;
			flow -= a_tri.normal * flow.Dot(a_tri.normal);
			// Where neighbouring flows disagree the average cancels; fall back to the face.
			if (flow.LengthSquared() < 1e-8f || flow.Dot(a_tri.flow) < 0.0f)
				return a_tri.flow;
			flow.Normalize();
			return flow;
		}

		template <class F>
		float Generator::Trace(uint32_t a_tri, Vec3 a_pos, float a_sign, float a_maxLength, F&& a_onSample, uint32_t* o_endTri, Vec3* o_endPos) const
		{
			uint32_t tri = a_tri;
			Vec3 pos = a_pos;
			float length = 0.0f;
			Vec3 previousDir;
			uint32_t zeroCrossings = 0;
			const uint32_t id = ++traceId;
			uint32_t cameFrom = UINT32_MAX;
			traceStamp[tri] = id;
			a_onSample(pos, tri, 0.0f);

			for (uint32_t stepIndex = 0; stepIndex < kMaxStepsPerStrand && length < a_maxLength; ++stepIndex) {
				float remaining = std::min(step, a_maxLength - length);
				bool stop = false;
				for (uint32_t crossing = 0; remaining > 1e-5f; ++crossing) {
					if (crossing >= kMaxCrossingsPerStep) {
						stop = true;
						break;
					}
					const Triangle& t = tris[tri];
					const Vec3 a = Position(t.v[0]), b = Position(t.v[1]), c = Position(t.v[2]);
					const Vec3 bp = ClampBarycentric(Barycentric(pos, a, b, c));
					const Vec3 dir = FlowAt(t, bp) * a_sign;
					if (previousDir.LengthSquared() > 0.0f && dir.Dot(previousDir) < -0.2f) {
						stop = true;  // the flow turned back on itself
						break;
					}
					const Vec3 target = pos + dir * remaining;
					const Vec3 bq = Barycentric(target, a, b, c);
					if (bq.x >= 0.0f && bq.y >= 0.0f && bq.z >= 0.0f) {
						pos = a * bq.x + b * bq.y + c * bq.z;
						length += remaining;
						remaining = 0.0f;
						previousDir = dir;
						break;
					}

					// Leave through the edge whose barycentric coordinate reaches zero first.
					float s = 1.0f;
					int exitVertex = -1;
					for (int i = 0; i < 3; ++i) {
						const float from = bp[i], to = bq[i];
						if (to < 0.0f && from - to > 1e-9f) {
							const float si = from / (from - to);
							if (si < s) {
								s = si;
								exitVertex = i;
							}
						}
					}
					if (exitVertex < 0) {
						stop = true;
						break;
					}
					s = std::clamp(s, 0.0f, 1.0f);
					const float moved = remaining * s;
					pos += dir * moved;
					length += moved;
					remaining -= moved;
					previousDir = dir;

					zeroCrossings = moved < 1e-5f ? zeroCrossings + 1 : 0;
					const int32_t next = t.neighbor[(exitVertex + 1) % 3];
					if (next < 0 || !tris[next].valid || tris[next].normal.Dot(t.normal) < kFoldThreshold || zeroCrossings > 8) {
						stop = true;  // a boundary: the tip (or root, tracing backwards)
						break;
					}
					// A streamline re-entering a triangle circles (a bun, a closed lock): it would
					// wind round until the length cap.
					if (traceStamp[next] == id && static_cast<uint32_t>(next) != cameFrom) {
						stop = true;
						break;
					}
					traceStamp[next] = id;
					cameFrom = tri;
					tri = static_cast<uint32_t>(next);
				}
				a_onSample(pos, tri, length);
				if (stop)
					break;
			}
			if (o_endTri)
				*o_endTri = tri;
			if (o_endPos)
				*o_endPos = pos;
			return length;
		}

		float Generator::Coverage(uint32_t a_tri, const Vec3& a_pos) const
		{
			const auto& tri = tris[a_tri];
			const Vec3 bary = ClampBarycentric(Barycentric(a_pos, Position(tri.v[0]), Position(tri.v[1]), Position(tri.v[2])));
			const Vec2 uv = mesh.uvs[tri.v[0]] * bary.x + mesh.uvs[tri.v[1]] * bary.y + mesh.uvs[tri.v[2]] * bary.z;
			return mesh.coverage.Sample(uv.x, uv.y);
		}

		bool Generator::HasCoverage(uint32_t a_tri) const
		{
			if (!useCoverage)
				return true;
			const auto& tri = tris[a_tri];
			const Vec3 a = Position(tri.v[0]), b = Position(tri.v[1]), c = Position(tri.v[2]);
			for (uint32_t i = 0; i <= kCoverageProbeGrid; ++i) {
				for (uint32_t j = 0; i + j <= kCoverageProbeGrid; ++j) {
					const float u = static_cast<float>(i) / kCoverageProbeGrid, v = static_cast<float>(j) / kCoverageProbeGrid;
					if (Covered(a_tri, a * (1.0f - u - v) + b * u + c * v))
						return true;
				}
			}
			return false;
		}

		bool Generator::TryAddGuide(uint32_t a_tri, const Vec3& a_pos, float a_maxLength, bool a_countVisits, bool a_findCoverage)
		{
			if (guides.size() >= guideBudget)
				return false;
			// Each triangle the streamline crosses, once.
			++currentStamp;
			crossed.clear();
			samples.clear();
			Trace(a_tri, a_pos, 1.0f, a_maxLength, [&](const Vec3& a_p, uint32_t a_t, float a_length) {
				if (visitStamp[a_t] != currentStamp) {
					visitStamp[a_t] = currentStamp;
					crossed.push_back(a_t);
				}
				samples.push_back({ a_p, a_t, a_length });
			});

			// With a coverage mask: from the first painted sample to the last one before a
			// transparent gap longer than kCoverageGap. Where the card's texture ends, the hair ends.
			size_t first = 0, last = samples.empty() ? 0 : samples.size() - 1;
			bool keep = !samples.empty();
			if (keep && useCoverage) {
				if (a_findCoverage) {
					while (first < samples.size() && !Covered(samples[first].tri, samples[first].position))
						++first;
				} else if (!Covered(samples[0].tri, samples[0].position)) {
					keep = false;
				}
				if (first >= samples.size())
					keep = false;
				if (keep) {
					last = first;
					for (size_t i = first + 1; i < samples.size(); ++i) {
						if (Covered(samples[i].tri, samples[i].position))
							last = i;
						else if (samples[i].length - samples[last].length > kCoverageGap)
							break;
					}
				}
			}
			keep = keep && samples[last].length - samples[first].length >= Limits::kMinStrandLength;

			// With a mask the whole streamline counts as visited, painted or not: its transparent
			// stretches need no fill guides either.
			if (a_countVisits && (keep || useCoverage)) {
				for (uint32_t t : crossed)
					visits[t] = static_cast<uint16_t>(std::min<uint32_t>(visits[t] + 1u, UINT16_MAX));
			}
			if (!keep)
				return false;

			CardGuide guide;
			guide.path.reserve(last - first + 1);
			float alpha = 0.0f;
			for (size_t i = first; i <= last; ++i) {
				guide.path.push_back({ samples[i].position, samples[i].tri, samples[i].length - samples[first].length });
				alpha += useCoverage ? Coverage(samples[i].tri, samples[i].position) : 1.0f;
			}
			guide.coverage = alpha / static_cast<float>(last - first + 1);
			guide.component = tris[samples[first].tri].component;
			guides.push_back(std::move(guide));
			return true;
		}

		void Generator::SeedRoots()
		{
			// One card guide per `spacing` of width across the flow, along the boundary edges the
			// flow enters: the card's upstream edge.
			struct RootEdge
			{
				uint32_t tri;
				int edge;
				float expected;
			};
			std::vector<RootEdge> roots;
			float total = 0.0f;
			for (uint32_t t = 0; t < tris.size(); ++t) {
				const auto& tri = tris[t];
				if (!tri.valid)
					continue;
				for (int i = 0; i < 3; ++i) {
					const int32_t n = tri.neighbor[i];
					if (n >= 0 && tris[n].valid)
						continue;
					const Vec3 a = Position(tri.v[i]), b = Position(tri.v[(i + 1) % 3]);
					const Vec3 edge = b - a;
					const float edgeLength = edge.Length();
					if (edgeLength < 1e-5f)
						continue;
					Vec3 outward = edge.Cross(tri.normal);
					outward.Normalize();
					float mid[3]{};
					mid[i] = 0.5f;
					mid[(i + 1) % 3] = 0.5f;
					const float entry = -FlowAt(tri, Vec3(mid[0], mid[1], mid[2])).Dot(outward);
					if (entry < kRootEntryThreshold)
						continue;
					const float expected = edgeLength * entry / spacing;
					roots.push_back({ t, i, expected });
					total += expected;
				}
			}

			const float budget = Limits::kMaxCardGuides * kRootBudgetShare;
			const float acceptance = total > budget ? budget / total : 1.0f;
			for (const auto& root : roots) {
				const auto& tri = tris[root.tri];
				KeyedRandom random(1, root.tri * 3u + static_cast<uint32_t>(root.edge), settings.seed);
				const uint32_t count = static_cast<uint32_t>(root.expected * acceptance + random());
				const Vec3 a = Position(tri.v[root.edge]), b = Position(tri.v[(root.edge + 1) % 3]);
				const Vec3 centre = (Position(tri.v[0]) + Position(tri.v[1]) + Position(tri.v[2])) / 3.0f;
				for (uint32_t j = 0; j < count; ++j) {
					const float s = (j + random()) / count;
					// Just inside the edge, so the first step starts in this triangle.
					const Vec3 p = Vec3::Lerp(Vec3::Lerp(a, b, s), centre, 0.002f);
					TryAddGuide(root.tri, p, Limits::kMaxStrandLength, true, true);
				}
			}
		}

		void Generator::SeedFill()
		{
			// Streamlines through triangles the roots missed (cards whose upstream edge is
			// shared with the scalp cap, sheets with no clean top edge): walk back to where
			// the flow starts, then keep the whole streamline.
			// In a random order, the same whichever triangles are converted.
			std::vector<uint32_t> order(tris.size());
			std::iota(order.begin(), order.end(), 0u);
			std::vector<uint32_t> rank(tris.size());
			for (uint32_t t = 0; t < tris.size(); ++t)
				rank[t] = Mix(t ^ Mix(settings.seed + 0x2545F491u));
			std::ranges::stable_sort(order, {}, [&](uint32_t a_t) { return rank[a_t]; });
			for (uint32_t t : order) {
				const auto& tri = tris[t];
				if (!tri.valid || !HasCoverage(t))
					continue;
				KeyedRandom random(2, t, settings.seed);
				const auto target = static_cast<uint32_t>(std::max(1.0f, std::round(std::sqrt(tri.area) / spacing * 0.5f)));
				for (uint32_t attempt = 0; visits[t] < target && attempt < target * 2; ++attempt) {
					float r1 = std::sqrt(random()), r2 = random();
					const Vec3 p = Position(tri.v[0]) * (1.0f - r1) + Position(tri.v[1]) * (r1 * (1.0f - r2)) + Position(tri.v[2]) * (r1 * r2);
					uint32_t rootTri = t;
					Vec3 rootPos = p;
					Trace(t, p, -1.0f, Limits::kMaxStrandLength, [](const Vec3&, uint32_t, float) {}, &rootTri, &rootPos);
					if (!TryAddGuide(rootTri, rootPos, Limits::kMaxStrandLength, true, true) && guides.size() >= guideBudget)
						return;
				}
			}
		}

		void Generator::SeedArea(const Scalp& a_scalp)
		{
			// Short strands scattered over the cards on the scalp: each is its own one-strand
			// guide. Short hair grows on the scalp too, so none on cards standing off it.
			std::vector<bool> onScalp(tris.size(), false);
			float totalArea = 0.0f;
			for (uint32_t t = 0; t < tris.size(); ++t) {
				const auto& tri = tris[t];
				if (!tri.valid)
					continue;
				for (uint32_t v : tri.v)
					onScalp[t] = onScalp[t] || a_scalp.Height(Position(v)) <= kAttachHeight;
				totalArea += onScalp[t] ? tri.area : 0.0f;
			}
			const float perArea = settings.density * 4.0f;
			const float budget = static_cast<float>(Limits::kMaxStrands);
			const float acceptance = totalArea * perArea > budget ? budget / (totalArea * perArea) : 1.0f;
			guideBudget = Limits::kMaxStrands;
			for (uint32_t t = 0; t < tris.size(); ++t) {
				const auto& tri = tris[t];
				if (!onScalp[t])
					continue;
				KeyedRandom random(3, t, settings.seed);
				const auto count = static_cast<uint32_t>(tri.area * perArea * acceptance + random());
				for (uint32_t j = 0; j < count; ++j) {
					float r1 = std::sqrt(random()), r2 = random();
					const Vec3 p = Position(tri.v[0]) * (1.0f - r1) + Position(tri.v[1]) * (r1 * (1.0f - r2)) + Position(tri.v[2]) * (r1 * r2);
					if (a_scalp.Height(p) > kAttachHeight)
						continue;
					if (!TryAddGuide(t, p, settings.shortLength, false, false) && guides.size() >= guideBudget)
						return;
				}
			}
		}

		void Generator::FitScalp(Scalp& o_scalp) const
		{
			// Samples of the painted hair, weighted by area.
			struct Sample
			{
				Vec3 p;
				float w;
			};
			std::vector<Sample> hair;
			for (uint32_t t = 0; t < tris.size(); ++t) {
				const auto& tri = tris[t];
				if (!tri.valid)
					continue;
				const Vec3 a = Position(tri.v[0]), b = Position(tri.v[1]), c = Position(tri.v[2]);
				for (uint32_t i = 0; i <= kCoverageProbeGrid; ++i) {
					for (uint32_t j = 0; i + j <= kCoverageProbeGrid; ++j) {
						const float u = static_cast<float>(i) / kCoverageProbeGrid, v = static_cast<float>(j) / kCoverageProbeGrid;
						const Vec3 p = a * (1.0f - u - v) + b * u + c * v;
						if (Covered(t, p))
							hair.push_back({ p, tri.area / kProbeCount });
					}
				}
			}

			o_scalp = {};
			o_scalp.centre = headCentre;
			constexpr int kBins = Scalp::kAzimuthBins * Scalp::kElevationBins;
			const auto binOf = [](const Vec3& a_direction) {
				const float azimuth = std::atan2(a_direction.y, a_direction.x);
				const float elevation = std::asin(std::clamp(a_direction.z, -1.0f, 1.0f));
				const int a = std::clamp(static_cast<int>((azimuth + kPi) / (2.0f * kPi) * Scalp::kAzimuthBins), 0, Scalp::kAzimuthBins - 1);
				const int e = std::clamp(static_cast<int>((elevation + 0.5f * kPi) / kPi * Scalp::kElevationBins), 0, Scalp::kElevationBins - 1);
				return e * Scalp::kAzimuthBins + a;
			};

			// Per direction from the centre, the innermost hair (kScalpPercentile of it) lies on
			// the scalp: the scalp cap's cards, or the cards nearest the skin. Only the top of the
			// head counts; below it hair hangs free (a ponytail, a curtain down the back).
			std::array<float, kBins> inner{};
			std::array<float, kBins> innerWeight{};
			std::array<Vec3, kBins> innerPoint{};
			const auto measure = [&](const Vec3& a_centre) {
				std::vector<std::vector<std::pair<float, uint32_t>>> bins(kBins);
				float capWeight = 0.0f;
				for (uint32_t i = 0; i < hair.size(); ++i) {
					Vec3 d = hair[i].p - a_centre;
					const float length = d.Length();
					if (length < 1e-4f)
						continue;
					d = d / length;
					if (d.z < kCapMinZ)
						continue;
					bins[binOf(d)].emplace_back(length, i);
					capWeight += hair[i].w;
				}
				for (int b = 0; b < kBins; ++b) {
					auto& bin = bins[b];
					inner[b] = 0.0f;
					innerWeight[b] = 0.0f;
					float weight = 0.0f;
					for (const auto& [d, i] : bin)
						weight += hair[i].w;
					if (bin.empty() || weight < kScalpMinBinWeight * capWeight / kBins)
						continue;
					std::ranges::sort(bin);
					float below = 0.0f;
					Vec3 point;
					for (const auto& [d, i] : bin) {
						below += hair[i].w;
						point += hair[i].p * hair[i].w;
						inner[b] = d;
						if (below >= kScalpPercentile * weight)
							break;
					}
					innerWeight[b] = weight;
					innerPoint[b] = point / below;
				}
			};

			// A sphere through the inner points, its centre held near the skull centre, refitted
			// once without the points far off it.
			Vec3 centre = headCentre;
			float radius = 0.0f;
			std::array<bool, kBins> use{};
			for (int pass = 0; pass < 3; ++pass) {
				if (pass < 2)
					measure(centre);
				std::array<std::array<double, 5>, 4> m{};
				int used = 0;
				double total = 0.0;
				for (int b = 0; b < kBins; ++b) {
					if (pass < 2)
						use[b] = innerWeight[b] > 0.0f;
					else if (use[b])
						use[b] = std::abs((innerPoint[b] - centre).Length() - radius) < 0.15f * radius;
					if (!use[b])
						continue;
					const Vec3& p = innerPoint[b];
					const double w = std::sqrt(innerWeight[b]);
					const std::array<double, 5> row{ 2.0 * p.x, 2.0 * p.y, 2.0 * p.z, 1.0, p.LengthSquared() };
					for (int r = 0; r < 4; ++r)
						for (int k = 0; k < 5; ++k)
							m[r][k] += w * row[r] * row[k];
					total += w;
					++used;
				}
				if (used < 6)
					break;
				// Prior: centre = skull centre, weighted against the data.
				const double prior = kScalpCentrePrior * total;
				for (int r = 0; r < 3; ++r) {
					m[r][r] += prior;
					m[r][4] += prior * headCentre[r];
				}
				std::array<double, 4> x{};
				if (!Solve<4>(m, x))
					break;
				Vec3 fitted(static_cast<float>(x[0]), static_cast<float>(x[1]), static_cast<float>(x[2]));
				Vec3 shift = fitted - headCentre;
				if (shift.Length() > kScalpMaxCentreShift)
					shift = shift.Normalized() * kScalpMaxCentreShift;
				const Vec3 c = headCentre + shift;
				const double r2 = x[3] + c.LengthSquared();
				if (!(r2 > 0.0))
					break;
				centre = c;
				radius = std::clamp(static_cast<float>(std::sqrt(r2)), kMinScalpRadius, kMaxScalpRadius);
				o_scalp.fitted = true;
			}

			if (!o_scalp.fitted) {
				// Too little hair over the head to fit: a sphere through the innermost hair.
				std::vector<float> distances;
				for (const auto& s : hair)
					distances.push_back((s.p - headCentre).Length());
				float r = 8.0f;
				if (!distances.empty()) {
					const size_t k = distances.size() / 10;
					std::nth_element(distances.begin(), distances.begin() + k, distances.end());
					r = distances[k];
				}
				o_scalp.centre = headCentre;
				o_scalp.sphereRadius = std::clamp(r, kMinScalpRadius, kMaxScalpRadius);
				o_scalp.radii.fill(o_scalp.sphereRadius);
				return;
			}

			// Per direction: the inner hair, held near the sphere; the sphere where there is none.
			measure(centre);
			o_scalp.centre = centre;
			o_scalp.sphereRadius = radius;
			for (int b = 0; b < kBins; ++b)
				o_scalp.radii[b] = innerWeight[b] > 0.0f ? std::clamp(inner[b], kScalpBelowSphere * radius, kScalpAboveSphere * radius) : radius;
			for (int pass = 0; pass < 2; ++pass) {
				auto smoothed = o_scalp.radii;
				for (int e = 0; e < Scalp::kElevationBins; ++e) {
					for (int a = 0; a < Scalp::kAzimuthBins; ++a) {
						float sum = 0.0f;
						int n = 0;
						for (int de = -1; de <= 1; ++de) {
							const int ee = e + de;
							if (ee < 0 || ee >= Scalp::kElevationBins)
								continue;
							for (int da = -1; da <= 1; ++da) {
								sum += o_scalp.radii[ee * Scalp::kAzimuthBins + (a + da + Scalp::kAzimuthBins) % Scalp::kAzimuthBins];
								++n;
							}
						}
						smoothed[e * Scalp::kAzimuthBins + a] = sum / static_cast<float>(n);
					}
				}
				o_scalp.radii = smoothed;
			}
		}

		void Generator::PruneRedundant(Stats& o_stats)
		{
			const float close = kRedundantRadius * spacing;
			const float fragmentRadius = kFragmentRadius * spacing;
			std::vector<uint32_t> order(guides.size());
			std::iota(order.begin(), order.end(), 0u);
			std::ranges::stable_sort(order, [&](uint32_t a, uint32_t b) { return guides[a].path.back().s > guides[b].path.back().s; });

			// A sheet's long hair: its 90th percentile guide length.
			std::unordered_map<int32_t, std::vector<float>> sheetLengths;
			for (const auto& guide : guides)
				sheetLengths[guide.component].push_back(guide.path.back().s);
			std::unordered_map<int32_t, float> sheetLong;
			for (auto& [component, lengths] : sheetLengths) {
				const size_t k = lengths.size() * 9 / 10;
				std::nth_element(lengths.begin(), lengths.begin() + k, lengths.end());
				sheetLong[component] = lengths[k];
			}

			// Per sheet: overlapping cards are layers of hair, each carrying its own.
			struct Kept
			{
				Vec3 position;
				int32_t component;
			};
			PointGrid<Kept> kept(fragmentRadius);
			std::vector<CardGuide> survivors;
			survivors.reserve(guides.size());
			for (uint32_t g : order) {
				auto& guide = guides[g];
				uint32_t closeCount = 0, nearCount = 0;
				for (const auto& sample : guide.path) {
					float best = FLT_MAX;
					kept.Query(sample.position, fragmentRadius, [&](const Kept& a_q) {
						if (a_q.component == guide.component)
							best = std::min(best, (a_q.position - sample.position).LengthSquared());
					});
					closeCount += best <= close * close;
					nearCount += best <= fragmentRadius * fragmentRadius;
				}
				const auto sampleCount = static_cast<float>(guide.path.size());
				// Running alongside a longer guide, or a short fragment beside the sheet's long hair
				// (a streamline from a side edge, or the end of a lock past a transparent gap).
				const bool repeats = closeCount > kRedundantShare * sampleCount;
				const bool fragment = guide.path.back().s < kFragmentLength * sheetLong[guide.component] && nearCount > kFragmentShare * sampleCount;
				if (repeats || fragment) {
					++o_stats.redundantGuides;
					continue;
				}
				for (const auto& sample : guide.path)
					kept.Insert(sample.position, { sample.position, guide.component });
				survivors.push_back(std::move(guide));
			}
			guides = std::move(survivors);
		}

		Vec3 Generator::InitialTangent(const CardGuide& a_guide) const
		{
			const auto& path = a_guide.path;
			for (size_t i = 1; i < path.size(); ++i) {
				if (path[i].s >= std::min(1.0f, path.back().s)) {
					const Vec3 d = path[i].position - path[0].position;
					if (d.LengthSquared() > 1e-10f)
						return d.Normalized();
				}
			}
			return path.size() > 1 ? (path.back().position - path[0].position).Normalized() : kUnitZ;
		}

		void Generator::Recompute(std::vector<PathSample>& io_path) const
		{
			float s = 0.0f;
			for (size_t i = 0; i < io_path.size(); ++i) {
				if (i > 0)
					s += (io_path[i].position - io_path[i - 1].position).Length();
				io_path[i].s = s;
			}
		}

		void Generator::AttachGuides(std::vector<int32_t>& o_pointOf)
		{
			o_pointOf.assign(guides.size(), -1);
			const auto& points = settings.attachPoints;
			if (points.empty())
				return;
			for (uint32_t g = 0; g < guides.size(); ++g) {
				auto& guide = guides[g];
				float best = FLT_MAX;
				int32_t point = -1;
				bool byTip = false;
				for (uint32_t i = 0; i < points.size(); ++i) {
					const float start = (guide.path.front().position - points[i].position).Length();
					const float tip = (guide.path.back().position - points[i].position).Length();
					if (start <= points[i].radius && start < best) {
						best = start;
						point = static_cast<int32_t>(i);
						byTip = false;
					}
					if (tip <= points[i].radius && tip < best) {
						best = tip;
						point = static_cast<int32_t>(i);
						byTip = true;
					}
				}
				if (point < 0)
					continue;
				// By its tip: the guide was traced the wrong way round.
				if (byTip)
					std::ranges::reverse(guide.path);
				const Vec3 at = points[point].position;
				if ((guide.path[0].position - at).LengthSquared() > 1e-6f)
					guide.path.insert(guide.path.begin(), { at, guide.path[0].tri, 0.0f });
				Recompute(guide.path);
				guide.kind = GuideKind::Tied;
				guide.ancestor = static_cast<int32_t>(g);
				o_pointOf[g] = point;
			}
		}

		void Generator::TieAttached(const std::vector<int32_t>& a_pointOf)
		{
			// A tie per attach point, on the chain of a braid it lies on (its hair swings with it).
			std::vector<int32_t> tieOf(settings.attachPoints.size(), -1);
			for (uint32_t g = 0; g < guides.size() && g < a_pointOf.size(); ++g) {
				const int32_t point = a_pointOf[g];
				if (point < 0)
					continue;
				const Vec3 at = settings.attachPoints[point].position;
				if (tieOf[point] < 0) {
					tieOf[point] = static_cast<int32_t>(ties.size());
					Tie tie;
					tie.centre = at;
					float best = kGatherReach * kGatherReach;
					for (const auto& piece : woven) {
						if (piece.chain < 0)
							continue;
						for (uint32_t t : piece.tris) {
							if (const float d = (triCentre[t] - at).LengthSquared(); d <= best) {
								best = d;
								tie.chain = piece.chain;
							}
						}
					}
					ties.push_back(tie);
				}
				auto& guide = guides[g];
				auto& tie = ties[tieOf[point]];
				guide.tie = tieOf[point];
				++tie.tails;
				tie.radius = std::max(tie.radius, (guide.path[std::min<size_t>(1, guide.path.size() - 1)].position - at).Length());
			}
		}

		void Generator::BindToScalp(const Scalp& a_scalp, Stats& o_stats)
		{
			// HairCS (2026) binds each card's guide to a root on the scalp. Here a card guide that
			// starts on the scalp grows from it. Hair gathered into woven hair (a braid, a tie) or
			// into a ponytail's tie is pulled tight to the head and stays cards; hair starting at
			// either grows from there. Any other guide that starts away from the scalp (a lower
			// layer) continues the rooted hair it starts on: the nearest hair running the same way,
			// already bound, within kMergeRadius. Chains bind over several passes (a layer under a
			// layer). What is left joins the scalp directly if close, and is dropped if not: hair
			// has to come from the head (or a tie). Hair attached by hand grows from where it was
			// attached, and takes no part in the rest.
			std::vector<int32_t> attachedTo;
			AttachGuides(attachedTo);
			std::vector<uint32_t> floating;
			for (uint32_t g = 0; g < guides.size(); ++g) {
				auto& guide = guides[g];
				if (attachedTo[g] >= 0)
					continue;
				const Vec3 root = guide.path[0].position;
				const float height = a_scalp.Height(root);
				if (height > kAttachHeight) {
					floating.push_back(g);
					continue;
				}
				guide.kind = GuideKind::Rooted;
				guide.ancestor = static_cast<int32_t>(g);
				if (height > 0.02f) {
					// Rise from the scalp a little upstream, so the hair leaves it at a slant.
					const Vec3 scalpRoot = a_scalp.Project(root - InitialTangent(guide) * height);
					guide.path.insert(guide.path.begin(), { scalpRoot, guide.path[0].tri, 0.0f });
					Recompute(guide.path);
				}
			}
			BindToWoven(a_scalp, floating);
			if (settings.keepWoven)
				FindTies(a_scalp, floating);
			TieAttached(attachedTo);  // after FindTies: their ties pull no other hair in

			struct Anchor
			{
				uint32_t guide;
				uint32_t sample;
			};
			PointGrid<Anchor> grid(kMergeRadius);
			const auto tangentAt = [&](const CardGuide& a_guide, uint32_t a_i) {
				const auto& p = a_guide.path;
				const uint32_t lo = a_i > 0 ? a_i - 1 : 0, hi = std::min<uint32_t>(a_i + 1, static_cast<uint32_t>(p.size()) - 1);
				const Vec3 d = p[hi].position - p[lo].position;
				return d.LengthSquared() > 1e-12f ? d.Normalized() : kZero;
			};
			const auto insert = [&](uint32_t a_g, uint32_t a_from) {
				const auto& path = guides[a_g].path;
				for (uint32_t i = a_from; i < path.size(); ++i)
					grid.Insert(path[i].position, { a_g, i });
			};
			const auto bound = [](const CardGuide& a_guide) { return a_guide.kind == GuideKind::Rooted || a_guide.kind == GuideKind::Merged || a_guide.kind == GuideKind::Bridged; };
			// Eases the end of a path (over a few units) so that it arrives at a_target.
			const auto easeEnd = [](std::vector<PathSample>& io_path, const Vec3& a_target) {
				const float endS = io_path.back().s;
				const Vec3 offset = a_target - io_path.back().position;
				const float blend = std::min(std::max(2.0f * offset.Length(), 1.0f), endS);
				for (auto& sample : io_path)
					sample.position += blend > 1e-4f ? offset * Smoothstep((sample.s - (endS - blend)) / blend) : offset;
			};
			// Gathered hair is bound too: a layer under it is gathered with it.
			const auto anchors = [&](const CardGuide& a_guide) { return bound(a_guide) || a_guide.kind == GuideKind::Gathered; };
			for (uint32_t g = 0; g < guides.size(); ++g)
				if (anchors(guides[g]))
					insert(g, 0);

			for (uint32_t pass = 0; pass < kMergePasses && !floating.empty(); ++pass) {
				// Where bound hair ends, this pass.
				PointGrid<uint32_t> tips(kContinueRadius);
				for (uint32_t g = 0; g < guides.size(); ++g)
					if (bound(guides[g]))
						tips.Insert(guides[g].path.back().position, g);
				std::vector<bool> extended(guides.size(), false);

				std::vector<uint32_t> boundNow, left;
				std::vector<std::pair<uint32_t, uint32_t>> grown;  // guide, first new sample
				for (uint32_t g : floating) {
					auto& guide = guides[g];
					const Vec3 p0 = guide.path[0].position;
					const Vec3 t0 = InitialTangent(guide);

					// Bound hair ending where this guide starts carries on into it: every lock that
					// gathers into a tie continues down the tail, not just the nearest one.
					std::vector<uint32_t> feeding;
					tips.Query(p0, kContinueRadius, [&](const uint32_t& a_h) {
						const auto& other = guides[a_h];
						if (extended[a_h] || !bound(other))
							return;
						const uint32_t last = static_cast<uint32_t>(other.path.size()) - 1;
						const Vec3 tip = other.path[last].position;
						const Vec3 tq = tangentAt(other, last);
						if ((p0 - tip).Length() <= kContinueRadius && tq.Dot(t0) >= kMergeAlignment && (p0 - tip).Dot(tq) >= -1.0f)
							feeding.push_back(a_h);
					});
					if (!feeding.empty()) {
						for (uint32_t h : feeding) {
							auto& other = guides[h];
							const auto first = static_cast<uint32_t>(other.path.size()) - 1;
							easeEnd(other.path, p0);
							other.path.pop_back();  // now at p0, which this guide's path starts with
							other.path.insert(other.path.end(), guide.path.begin(), guide.path.end());
							Recompute(other.path);
							extended[h] = true;
							grown.emplace_back(h, first);
						}
						guide.kind = GuideKind::Continued;
						continue;
					}

					float bestCost = FLT_MAX;
					Anchor best{};
					grid.Query(p0, kMergeRadius, [&](const Anchor& a_anchor) {
						const auto& other = guides[a_anchor.guide];
						if (!anchors(other) || a_anchor.sample >= other.path.size())
							return;
						const Vec3 q = other.path[a_anchor.sample].position;
						const Vec3 tq = tangentAt(other, a_anchor.sample);
						const float distance = (p0 - q).Length();
						const float agreement = tq.Dot(t0);
						if (distance > kMergeRadius || agreement < kMergeAlignment || (p0 - q).Dot(tq) < -0.5f)
							return;
						const float cost = distance + kMergeTurnCost * (1.0f - agreement);
						if (cost < bestCost) {
							bestCost = cost;
							best = a_anchor;
						}
					});
					if (bestCost == FLT_MAX) {
						left.push_back(g);
						continue;
					}

					// The bound hair up to the anchor, eased over to this guide's start, then this guide.
					const auto& upstream = guides[best.guide];
					std::vector<PathSample> path(upstream.path.begin(), upstream.path.begin() + best.sample + 1);
					easeEnd(path, p0);
					path.pop_back();  // now at p0, which the guide's own path starts with
					path.insert(path.end(), guide.path.begin(), guide.path.end());
					Recompute(path);
					guide.kind = upstream.kind == GuideKind::Gathered && !ChosenAsStrands(g) ? GuideKind::Gathered : GuideKind::Merged;
					guide.ancestor = upstream.ancestor;
					guide.path = std::move(path);
					boundNow.push_back(g);
				}
				for (uint32_t g : boundNow)
					insert(g, 0);
				for (const auto& [g, first] : grown)
					insert(g, first);
				const bool progress = left.size() < floating.size();
				floating = std::move(left);
				if (!progress)
					break;
			}

			for (uint32_t g : floating) {
				auto& guide = guides[g];
				const Vec3 root = guide.path[0].position;
				const float height = a_scalp.Height(root);
				if (height > kMaxBridgeHeight || guide.path.back().s < kMinMergedTail) {
					guide.kind = GuideKind::Dropped;
					continue;
				}
				// A Hermite curve up from the scalp, arriving along the guide.
				const Vec3 t0 = InitialTangent(guide);
				const Vec3 scalpRoot = a_scalp.Project(root - t0 * height);
				const float span = (root - scalpRoot).Length();
				const Vec3 normal = (scalpRoot - a_scalp.centre).Normalized();
				const Vec3 startTangent = (t0 - normal * t0.Dot(normal) + normal * 0.5f).Normalized() * span;
				const Vec3 endTangent = t0 * span;
				const auto count = static_cast<uint32_t>(std::max(2.0f, std::ceil(span / step)));
				std::vector<PathSample> bridge;
				for (uint32_t i = 0; i < count; ++i) {
					const float u = static_cast<float>(i) / count;
					const float u2 = u * u, u3 = u2 * u;
					const Vec3 p = scalpRoot * (2 * u3 - 3 * u2 + 1) + startTangent * (u3 - 2 * u2 + u) + root * (-2 * u3 + 3 * u2) + endTangent * (u3 - u2);
					bridge.push_back({ p, guide.path[0].tri, 0.0f });
				}
				guide.path.insert(guide.path.begin(), bridge.begin(), bridge.end());
				Recompute(guide.path);
				guide.kind = GuideKind::Bridged;
				guide.ancestor = static_cast<int32_t>(g);
			}

			for (const auto& guide : guides) {
				o_stats.rootedGuides += guide.kind == GuideKind::Rooted;
				o_stats.mergedGuides += guide.kind == GuideKind::Merged;
				o_stats.bridgedGuides += guide.kind == GuideKind::Bridged;
				o_stats.continuedGuides += guide.kind == GuideKind::Continued;
				o_stats.droppedGuides += guide.kind == GuideKind::Dropped;
				o_stats.gatheredGuides += guide.kind == GuideKind::Gathered;
				o_stats.tiedGuides += guide.kind == GuideKind::Tied;
			}
		}

		void Generator::ResolveChoices()
		{
			choice.assign(tris.size(), RegionChoice::Auto);
			for (uint32_t t = 0; t < tris.size(); ++t) {
				if (t < settings.triangleRegions.size() && settings.triangleRegions[t] != RegionChoice::Auto) {
					choice[t] = settings.triangleRegions[t];
					continue;
				}
				if (settings.excludeUV.empty() && settings.chainUV.empty())
					continue;
				const auto& v = tris[t].v;
				if (v[0] >= mesh.uvs.size() || v[1] >= mesh.uvs.size() || v[2] >= mesh.uvs.size())
					continue;
				const Vec2 uv = (mesh.uvs[v[0]] + mesh.uvs[v[1]] + mesh.uvs[v[2]]) / 3.0f;
				const auto inside = [&](const std::vector<UVRect>& a_rects) { return std::ranges::any_of(a_rects, [&](const UVRect& r) { return r.Contains(uv.x, uv.y); }); };
				if (inside(settings.chainUV))
					choice[t] = RegionChoice::Chain;
				else if (inside(settings.excludeUV))
					choice[t] = RegionChoice::Cards;
			}
		}

		uint16_t Generator::DominantBone(uint32_t a_tri) const
		{
			const bool skinned = mesh.boneIndices.size() == mesh.positions.size() && mesh.boneWeights.size() == mesh.positions.size();
			const int32_t head = FindHeadBone();
			if (!skinned)
				return static_cast<uint16_t>(std::max(head, 0));
			std::array<std::pair<uint16_t, float>, 12> influences{};
			size_t count = 0;
			for (uint32_t v : tris[a_tri].v) {
				for (int j = 0; j < 4; ++j) {
					const float w = mesh.boneWeights[v][j];
					if (!(w > 0.0f))
						continue;
					const uint16_t bone = mesh.boneIndices[v][j];
					auto it = std::find_if(influences.begin(), influences.begin() + count, [&](const auto& e) { return e.first == bone; });
					if (it != influences.begin() + count)
						it->second += w;
					else
						influences[count++] = { bone, w };
				}
			}
			if (count == 0)
				return static_cast<uint16_t>(std::max(head, 0));
			return std::max_element(influences.begin(), influences.begin() + count, [](const auto& l, const auto& r) { return l.second < r.second; })->first;
		}

		void Generator::FindWoven(const Scalp& a_scalp)
		{
			const auto triCount = static_cast<uint32_t>(tris.size());
			wovenOf.assign(triCount, -1);
			woven.clear();
			chains.clear();
			lines.clear();
			triCentre.assign(triCount, kZero);
			triHeight.assign(triCount, 0.0f);
			triHangs.assign(triCount, false);
			std::vector<bool> inMesh(triCount, false);
			for (uint32_t t = 0; t < triCount; ++t) {
				const auto& v = tris[t].v;
				if (v[0] >= mesh.positions.size() || v[1] >= mesh.positions.size() || v[2] >= mesh.positions.size())
					continue;
				inMesh[t] = true;
				triCentre[t] = (Position(v[0]) + Position(v[1]) + Position(v[2])) / 3.0f;
				triHeight[t] = a_scalp.Height(triCentre[t]);
				triHangs[t] = Hangs(a_scalp, triCentre[t]);
			}
			const bool anyForced = std::ranges::any_of(choice, [](RegionChoice c) { return c == RegionChoice::Cards || c == RegionChoice::Chain; });
			if (!settings.keepWoven && !anyForced)
				return;

			// Points for contact tests: a triangle's centre and corners.
			struct Sample
			{
				Vec3 p;
				uint32_t id;
			};
			const auto forSamples = [&](uint32_t a_tri, auto&& a_f) {
				a_f(triCentre[a_tri]);
				for (uint32_t v : tris[a_tri].v)
					a_f(Position(v));
			};

			int32_t componentCount = 0;
			for (const auto& tri : tris)
				componentCount = std::max(componentCount, tri.component + 1);
			// Woven hair is found as if nothing were chosen by hand, so a choice changes only its own
			// triangles: a braid partly chosen as cards is still a braid, and the hair gathered into it
			// is still gathered. The chosen triangles leave their pieces at the end.
			std::vector<std::vector<uint32_t>> members(componentCount);
			for (uint32_t t = 0; t < triCount; ++t) {
				if (tris[t].valid)
					members[tris[t].component].push_back(t);
			}

			// Each sheet's shape, from its painted part: its principal axes (spreads along them), which
			// ways its surface faces round them, and how its hair runs.
			enum class Shape : uint8_t
			{
				Loose,
				Tube,   // a braid or twist built as a tube
				Twist,  // a strip rolled or twisted round its length: a plait's strand, or a curl
				Ring,   // a tie or band round a tail
				Lobe    // a short curved piece: one of a plait's lobes, or of a bun
			};
			std::vector<Shape> shape(componentCount, Shape::Loose);
			std::vector<float> paintedArea(componentCount, 0.0f);
			std::vector<float> spreadAlong(componentCount, 0.0f);
			if (settings.keepWoven) {
				std::vector<uint32_t> painted;
				for (int32_t k = 0; k < componentCount; ++k) {
					painted.clear();
					double weight = 0.0;
					std::array<double, 3> mean{};
					for (uint32_t t : members[k]) {
						if (!HasCoverage(t))
							continue;
						painted.push_back(t);
						weight += tris[t].area;
						for (int i = 0; i < 3; ++i)
							mean[i] += tris[t].area * static_cast<double>(triCentre[t][i]);
					}
					paintedArea[k] = static_cast<float>(weight);
					if (painted.size() < 3 || !(weight > 0.0))
						continue;
					for (auto& m : mean)
						m /= weight;
					std::array<std::array<double, 3>, 3> covariance{};
					for (uint32_t t : painted) {
						const double d[3] = { triCentre[t].x - mean[0], triCentre[t].y - mean[1], triCentre[t].z - mean[2] };
						for (int r = 0; r < 3; ++r)
							for (int c = 0; c < 3; ++c)
								covariance[r][c] += tris[t].area * d[r] * d[c] / weight;
					}
					std::array<double, 3> values{};
					std::array<Vec3, 3> axes{};
					SymmetricEigen(covariance, values, axes);
					std::array<float, 3> spread{};
					for (int i = 0; i < 3; ++i)
						spread[i] = 2.0f * std::sqrt(static_cast<float>(std::max(values[i], 0.0)));
					spreadAlong[k] = spread[0];

					// Of kSideBins directions round an axis, how many the surface faces (n and -n alike).
					const auto sides = [&](const Vec3& a_axis) {
						const Vec3 a = a_axis.Cross(std::abs(a_axis.z) < 0.9f ? kUnitZ : Vec3(1.0f, 0.0f, 0.0f)).Normalized();
						const Vec3 b = a_axis.Cross(a);
						std::array<float, kSideBins> bins{};
						float total = 0.0f;
						for (uint32_t t : painted) {
							const float x = tris[t].normal.Dot(a), y = tris[t].normal.Dot(b);
							float angle = std::atan2(y, x);
							if (angle < 0.0f)
								angle += kPi;
							const float w = tris[t].area * std::sqrt(x * x + y * y);
							bins[std::min(static_cast<uint32_t>(angle / kPi * kSideBins), kSideBins - 1)] += w;
							total += w;
						}
						return static_cast<uint32_t>(std::ranges::count_if(bins, [&](float w) { return w > kSideShare * total; }));
					};
					// A ring faces out round its axis; a flat card faces along it.
					float ringFacing = 0.0f;
					for (uint32_t t : painted)
						ringFacing += tris[t].area * std::abs(tris[t].normal.Dot(axes[2]));
					ringFacing /= static_cast<float>(weight);
					float alignment = 0.0f;
					for (uint32_t t : painted)
						alignment += tris[t].area * std::abs(tris[t].flow.Dot(axes[0]));
					alignment /= static_cast<float>(weight);

					// How much of each cross-section's circle round the axis holds hair: a tube's all of it, a
					// rolled strip's or a curl's ribbon only an arc.
					const auto closed = [&](int a_axis) {
						const Vec3 axis = axes[a_axis], a = axes[(a_axis + 1) % 3], b = axes[(a_axis + 2) % 3];
						std::unordered_map<int32_t, std::vector<uint32_t>> slices;
						const Vec3 origin(static_cast<float>(mean[0]), static_cast<float>(mean[1]), static_cast<float>(mean[2]));
						for (uint32_t t : painted)
							slices[static_cast<int32_t>(std::floor((triCentre[t] - origin).Dot(axis) / kSliceLength))].push_back(t);
						float covered = 0.0f, total = 0.0f;
						for (const auto& [index, slice] : slices) {
							Vec3 centre;
							float area = 0.0f;
							for (uint32_t t : slice) {
								centre += triCentre[t] * tris[t].area;
								area += tris[t].area;
							}
							centre = centre / area;
							std::array<float, kSliceBins> bins{};
							for (uint32_t t : slice) {
								const Vec3 d = triCentre[t] - centre;
								const float angle = std::atan2(d.Dot(b), d.Dot(a)) + kPi;
								bins[std::min(static_cast<uint32_t>(angle / (2.0f * kPi) * kSliceBins), kSliceBins - 1)] += tris[t].area;
							}
							const auto filled = std::ranges::count_if(bins, [&](float w) { return w > 0.03f * area; });
							covered += area * static_cast<float>(filled) / kSliceBins;
							total += area;
						}
						return total > 0.0f ? covered / total : 0.0f;
					};

					// A thicker tube is a braid only if its texture shows a plait (crossing streaks, not
					// strands running one way): a ponytail rolled into one sheet looks the same otherwise.
					float streaks = 0.0f;
					for (uint32_t t : painted)
						streaks += tris[t].streaks * tris[t].area;
					streaks /= static_cast<float>(weight);
					const bool thin = spread[1] < kTubeMaxWidth;
					const bool plaitWidth = streaks < kPlaitStreaks && spread[1] < kPlaitTubeMaxWidth;
					if ((thin || plaitWidth) && spread[0] >= kTubeElongation * spread[1] && alignment >= kTubeAlignment && sides(axes[0]) >= kTubeSides) {
						if (closed(0) >= kTubeClosed)
							shape[k] = Shape::Tube;
						else if (thin)
							shape[k] = Shape::Twist;
					} else if (spread[0] < kRingMaxWidth && spread[2] < kRingFlatness * spread[1] && ringFacing < kRingFacing && closed(2) >= kTubeClosed)
						shape[k] = Shape::Ring;  // closed round its axis: an arc of a layered cut is not
					else if (spread[0] < kLobeMaxLength && sides(axes[0]) >= kLobeSides)
						shape[k] = Shape::Lobe;
				}
			}

			// Woven sheets in contact form groups. A group with a tube is a braid; enough lobes in a slim
			// bundle are a plait or a bun; twisted strips (a plait's strands) go with either, but alone
			// are curls, which stay loose hair. A ring near the scalp is a tie in any case.
			PointGrid<Sample> candidates(kContactRadius);
			for (int32_t k = 0; k < componentCount; ++k) {
				if (shape[k] != Shape::Loose) {
					for (uint32_t t : members[k])
						forSamples(t, [&](const Vec3& a_p) { candidates.Insert(a_p, { a_p, static_cast<uint32_t>(k) }); });
				}
			}
			DisjointSets groups(componentCount);
			for (int32_t k = 0; k < componentCount; ++k) {
				if (shape[k] == Shape::Loose)
					continue;
				for (uint32_t t : members[k]) {
					forSamples(t, [&](const Vec3& a_p) {
						candidates.Query(a_p, kContactRadius, [&](const Sample& a_q) {
							if (a_q.id != static_cast<uint32_t>(k) && (a_q.p - a_p).LengthSquared() <= kContactRadius * kContactRadius)
								groups.Unite(static_cast<uint32_t>(k), a_q.id);
						});
					});
				}
			}
			struct Group
			{
				uint32_t tubes = 0, lobes = 0;
				std::vector<int32_t> components;
				double weight = 0.0;
			};
			std::unordered_map<uint32_t, Group> found;
			for (int32_t k = 0; k < componentCount; ++k) {
				if (shape[k] == Shape::Loose)
					continue;
				auto& group = found[groups.Find(static_cast<uint32_t>(k))];
				group.components.push_back(k);
				group.tubes += shape[k] == Shape::Tube;
				group.lobes += shape[k] == Shape::Lobe;
				for (uint32_t t : members[k])
					group.weight += tris[t].area;
			}
			std::vector<int32_t> groupOf(componentCount, -1);  // accepted groups only
			std::vector<float> groupArea;
			// A braid, a plait or a bun is slim wherever you look at it, even curving round the head: the
			// hair within kLocalRadius of a point spreads along it, not across. A sheaf of short cards
			// across the head, or a curtain of dreadlocks, is wide.
			const auto slim = [&](const std::vector<uint32_t>& groupTris) {
				PointGrid<uint32_t> grid(kLocalRadius);
				for (uint32_t t : groupTris)
					grid.Insert(triCentre[t], t);
				std::vector<float> widths;
				const size_t stride = std::max<size_t>(1, groupTris.size() / 256);
				for (size_t i = 0; i < groupTris.size(); i += stride) {
					const Vec3& p = triCentre[groupTris[i]];
					double weight = 0.0;
					std::array<double, 3> mean{};
					std::array<std::array<double, 3>, 3> moment{};
					uint32_t count = 0;
					grid.Query(p, kLocalRadius, [&](const uint32_t& a_t) {
						if ((triCentre[a_t] - p).LengthSquared() > kLocalRadius * kLocalRadius)
							return;
						const double w = tris[a_t].area;
						++count;
						weight += w;
						for (int r = 0; r < 3; ++r) {
							mean[r] += w * triCentre[a_t][r];
							for (int col = 0; col < 3; ++col)
								moment[r][col] += w * triCentre[a_t][r] * triCentre[a_t][col];
						}
					});
					if (count < 4 || !(weight > 0.0))
						continue;
					std::array<std::array<double, 3>, 3> covariance{};
					for (int r = 0; r < 3; ++r)
						for (int col = 0; col < 3; ++col)
							covariance[r][col] = moment[r][col] / weight - (mean[r] / weight) * (mean[col] / weight);
					std::array<double, 3> values{};
					std::array<Vec3, 3> axes{};
					SymmetricEigen(covariance, values, axes);
					widths.push_back(2.0f * std::sqrt(static_cast<float>(std::max(values[1], 0.0))));
				}
				if (widths.empty())
					return false;
				std::nth_element(widths.begin(), widths.begin() + widths.size() / 2, widths.end());
				return widths[widths.size() / 2] < kBraidWidth;
			};
			for (auto& [root, group] : found) {
				// Short curved cards lying on the head are as often a layered cut as a braid, and
				// keeping a layered cut as cards would lose all its strands: lobes make a plait only
				// where it hangs. Tubes are braids anywhere; twisted strips go with either.
				float hangingArea = 0.0f;
				for (int32_t k : group.components)
					for (uint32_t t : members[k])
						hangingArea += triHangs[t] ? tris[t].area : 0.0f;
				const bool plait = group.lobes >= kMinLobes && hangingArea >= kPlaitHanging * static_cast<float>(group.weight);
				std::vector<uint32_t> groupTris;
				for (int32_t k : group.components)
					groupTris.insert(groupTris.end(), members[k].begin(), members[k].end());
				if ((group.tubes > 0 || plait) && slim(groupTris)) {
					for (int32_t k : group.components)
						groupOf[k] = static_cast<int32_t>(groupArea.size());
					groupArea.push_back(static_cast<float>(group.weight));
					continue;
				}
				for (int32_t k : group.components) {
					// In a wide group, a tube on its own is a braid or a dreadlock only if it is long and
					// hangs: short rolled clumps fill a mass of curls.
					float hanging = 0.0f, area = 0.0f;
					for (uint32_t t : members[k]) {
						area += tris[t].area;
						hanging += triHangs[t] ? tris[t].area : 0.0f;
					}
					if (shape[k] == Shape::Tube && spreadAlong[k] >= kMinChainLength && hanging >= kPlaitHanging * area) {
						groupOf[k] = static_cast<int32_t>(groupArea.size());
						groupArea.push_back(paintedArea[k]);
						continue;
					}
					if (shape[k] != Shape::Ring || std::ranges::none_of(members[k], [&](uint32_t t) { return triHeight[t] < kMaxTieHeight; }))
						continue;
					groupOf[k] = static_cast<int32_t>(groupArea.size());
					groupArea.push_back(paintedArea[k]);
				}
			}

			// Small sheets held in a group (a plait's lobes built as flat strips, the knot of a tie) are part of it.
			PointGrid<Sample> held(kContactRadius);
			for (int32_t k = 0; k < componentCount; ++k) {
				if (groupOf[k] >= 0) {
					for (uint32_t t : members[k])
						forSamples(t, [&](const Vec3& a_p) { held.Insert(a_p, { a_p, static_cast<uint32_t>(groupOf[k]) }); });
				}
			}
			std::vector<int32_t> absorbed(componentCount, -1);
			for (int32_t k = 0; k < componentCount; ++k) {
				if (groupOf[k] >= 0 || members[k].empty())
					continue;
				std::unordered_map<uint32_t, uint32_t> touching;
				uint32_t total = 0;
				for (uint32_t t : members[k]) {
					forSamples(t, [&](const Vec3& a_p) {
						++total;
						int32_t nearest = -1;
						held.Query(a_p, kContactRadius, [&](const Sample& a_q) {
							if ((a_q.p - a_p).LengthSquared() <= kContactRadius * kContactRadius)
								nearest = static_cast<int32_t>(a_q.id);
						});
						if (nearest >= 0)
							++touching[static_cast<uint32_t>(nearest)];
					});
				}
				for (const auto& [group, count] : touching) {
					if (count >= kAbsorbShare * total && paintedArea[k] < 0.5f * groupArea[group])
						absorbed[k] = static_cast<int32_t>(group);
				}
			}
			for (int32_t k = 0; k < componentCount; ++k) {
				if (absorbed[k] >= 0)
					groupOf[k] = absorbed[k];
			}
			woven.resize(groupArea.size());
			for (int32_t k = 0; k < componentCount; ++k) {
				if (groupOf[k] >= 0)
					woven[groupOf[k]].tris.insert(woven[groupOf[k]].tris.end(), members[k].begin(), members[k].end());
			}

			// Triangles chosen by hand as cards or chains: each connected set is a piece of its own.
			for (const RegionChoice forced : { RegionChoice::Cards, RegionChoice::Chain }) {
				std::vector<uint32_t> chosen;
				for (uint32_t t = 0; t < triCount; ++t) {
					if (inMesh[t] && choice[t] == forced)
						chosen.push_back(t);
				}
				if (chosen.empty())
					continue;
				PointGrid<Sample> grid(kContactRadius);
				for (uint32_t i = 0; i < chosen.size(); ++i)
					forSamples(chosen[i], [&](const Vec3& a_p) { grid.Insert(a_p, { a_p, i }); });
				DisjointSets sets(chosen.size());
				for (uint32_t i = 0; i < chosen.size(); ++i) {
					forSamples(chosen[i], [&](const Vec3& a_p) {
						grid.Query(a_p, kContactRadius, [&](const Sample& a_q) {
							if ((a_q.p - a_p).LengthSquared() <= kContactRadius * kContactRadius)
								sets.Unite(i, a_q.id);
						});
					});
				}
				std::unordered_map<uint32_t, uint32_t> pieceOfSet;
				for (uint32_t i = 0; i < chosen.size(); ++i) {
					const auto [it, inserted] = pieceOfSet.try_emplace(sets.Find(i), static_cast<uint32_t>(woven.size()));
					if (inserted) {
						woven.emplace_back();
						woven.back().forced = true;
						woven.back().region = forced == RegionChoice::Chain ? Region::Chain : Region::Cards;
					}
					woven[it->second].tris.push_back(chosen[i]);
				}
			}

			// Woven hair lying on the head stays with it. Where it hangs free (off the scalp, or
			// below the skull), each connected hanging part swings on a chain: a braid below a crown of braids,
			// or below a French braid, each on its own. A piece chosen by hand as a chain is one whole.
			std::vector<WovenPiece> parts;
			for (auto& piece : woven) {
				if (piece.forced) {
					parts.push_back(std::move(piece));
					continue;
				}
				WovenPiece onHead;
				std::vector<uint32_t> hanging;
				for (uint32_t t : piece.tris)
					(triHangs[t] ? hanging : onHead.tris).push_back(t);
				if (!onHead.tris.empty())
					parts.push_back(std::move(onHead));
				PointGrid<Sample> grid(kContactRadius);
				for (uint32_t i = 0; i < hanging.size(); ++i)
					forSamples(hanging[i], [&](const Vec3& a_p) { grid.Insert(a_p, { a_p, i }); });
				DisjointSets sets(hanging.size());
				for (uint32_t i = 0; i < hanging.size(); ++i) {
					forSamples(hanging[i], [&](const Vec3& a_p) {
						grid.Query(a_p, kContactRadius, [&](const Sample& a_q) {
							if ((a_q.p - a_p).LengthSquared() <= kContactRadius * kContactRadius)
								sets.Unite(i, a_q.id);
						});
					});
				}
				std::unordered_map<uint32_t, std::vector<uint32_t>> hangingSets;
				for (uint32_t i = 0; i < hanging.size(); ++i)
					hangingSets[sets.Find(i)].push_back(hanging[i]);
				for (auto& [root, set] : hangingSets) {
					if (slim(set)) {
						parts.emplace_back();
						parts.back().region = Region::Chain;
						parts.back().tris = std::move(set);
						continue;
					}
					// Braids or dreadlocks hanging side by side, touching: each sheet swings on its own.
					std::unordered_map<int32_t, size_t> byComponent;
					for (uint32_t t : set) {
						const auto [it, inserted] = byComponent.try_emplace(tris[t].component, parts.size());
						if (inserted) {
							parts.emplace_back();
							parts.back().region = Region::Chain;
						}
						parts[it->second].tris.push_back(t);
					}
				}
			}
			woven = std::move(parts);

			// The biggest hanging parts first, while chains are left; any other stays with the head.
			// Pieces found get theirs before pieces chosen by hand, which take none from them.
			std::vector<uint32_t> order(woven.size());
			std::iota(order.begin(), order.end(), 0u);
			std::ranges::stable_sort(order, [&](uint32_t a, uint32_t b) {
				if (woven[a].forced != woven[b].forced)
					return !woven[a].forced;
				return woven[a].tris.size() > woven[b].tris.size();
			});
			uint32_t bones = 0;
			for (uint32_t i : order) {
				auto& piece = woven[i];
				const bool wanted = piece.region == Region::Chain;
				piece.region = Region::Cards;
				// A piece found keeps its centre line through the triangles chosen away from it, a
				// chain while any of it is left: hair is gathered into it, or grows from its end, as
				// if nothing were chosen.
				std::vector<uint32_t> own;
				if (!piece.forced)
					for (uint32_t t : piece.tris)
						(choice[t] == RegionChoice::Auto ? own : piece.chosenAway).push_back(t);
				ChainCurve chain;
				if (wanted && lines.size() < Limits::kMaxChains && BuildChain(piece, a_scalp, chain)) {
					piece.line = static_cast<int32_t>(lines.size());
					lines.push_back(chain);
					if (piece.forced || !own.empty()) {
						chain.firstBone = chainBoneBase + bones;
						bones += static_cast<uint32_t>(chain.joints.size());
						piece.region = Region::Chain;
						piece.chain = static_cast<int32_t>(chains.size());
						chains.push_back(std::move(chain));
					}
				}
				if (!piece.forced)
					piece.tris = std::move(own);
				for (uint32_t t : piece.tris) {
					wovenOf[t] = static_cast<int32_t>(i);
					tris[t].valid = false;  // no flow tracing, no strands
				}
			}
		}

		bool Generator::BuildChain(const WovenPiece& a_piece, const Scalp& a_scalp, ChainCurve& o_chain) const
		{
			// The centre line runs from the piece's end nearest the scalp: the centre of its hair at
			// each distance from there, measured through the piece.
			constexpr float kLink = 1.5f;  // triangle centres this close are neighbours along the piece
			const auto& pieceTris = a_piece.tris;
			const auto count = static_cast<uint32_t>(pieceTris.size());
			if (count < 4)
				return false;
			float lowest = FLT_MAX;
			for (uint32_t t : pieceTris)
				lowest = std::min(lowest, triHeight[t]);
			PointGrid<uint32_t> grid(kLink);
			for (uint32_t i = 0; i < count; ++i)
				grid.Insert(triCentre[pieceTris[i]], i);
			std::vector<float> distance(count, FLT_MAX);
			using Entry = std::pair<float, uint32_t>;
			std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
			uint32_t rootTri = pieceTris[0];
			for (uint32_t i = 0; i < count; ++i) {
				if (triHeight[pieceTris[i]] <= lowest + 0.5f) {
					distance[i] = 0.0f;
					queue.push({ 0.0f, i });
					if (triHeight[pieceTris[i]] == lowest)
						rootTri = pieceTris[i];
				}
			}
			while (!queue.empty()) {
				const auto [d, i] = queue.top();
				queue.pop();
				if (d > distance[i])
					continue;
				const Vec3& p = triCentre[pieceTris[i]];
				grid.Query(p, kLink, [&](const uint32_t& a_j) {
					const float step = (triCentre[pieceTris[a_j]] - p).Length();
					if (step <= kLink && d + step < distance[a_j]) {
						distance[a_j] = d + step;
						queue.push({ distance[a_j], a_j });
					}
				});
			}
			float furthest = 0.0f;
			for (float d : distance)
				furthest = d < FLT_MAX ? std::max(furthest, d) : furthest;
			const auto bins = static_cast<uint32_t>(furthest / kChainBin) + 1;
			std::vector<Vec3> sums(bins);
			std::vector<float> weights(bins, 0.0f);
			for (uint32_t i = 0; i < count; ++i) {
				if (distance[i] == FLT_MAX)
					continue;
				const auto bin = std::min(static_cast<uint32_t>(distance[i] / kChainBin), bins - 1);
				sums[bin] += triCentre[pieceTris[i]] * tris[pieceTris[i]].area;
				weights[bin] += tris[pieceTris[i]].area;
			}
			std::vector<Vec3> line;
			for (uint32_t b = 0; b < bins; ++b) {
				if (weights[b] > 0.0f)
					line.push_back(sums[b] / weights[b]);
			}
			if (line.size() < 2)
				return false;
			for (int pass = 0; pass < 2; ++pass) {
				auto smoothed = line;
				for (size_t i = 1; i + 1 < line.size(); ++i)
					smoothed[i] = line[i] * 0.5f + (line[i - 1] + line[i + 1]) * 0.25f;
				line = std::move(smoothed);
			}
			std::vector<float> along(line.size(), 0.0f);
			for (size_t i = 1; i < line.size(); ++i)
				along[i] = along[i - 1] + (line[i] - line[i - 1]).Length();
			const float length = along.back();
			// A chain hangs: its end lies below its root (a bun or a braid lying over the head does not swing).
			if (length < kMinChainLength || line.back().z > line.front().z - 0.5f * kMinChainLength)
				return false;

			const auto joints = std::clamp(static_cast<uint32_t>(std::ceil(length / kChainSegment)) + 1, 2u, Limits::kMaxChainJoints);
			o_chain = {};
			o_chain.joints.resize(joints);
			size_t at = 0;
			for (uint32_t j = 0; j < joints; ++j) {
				const float s = length * static_cast<float>(j) / static_cast<float>(joints - 1);
				while (at + 2 < line.size() && along[at + 1] < s)
					++at;
				const float span = along[at + 1] - along[at];
				o_chain.joints[j] = Vec3::Lerp(line[at], line[at + 1], span > 1e-6f ? std::clamp((s - along[at]) / span, 0.0f, 1.0f) : 0.0f);
			}
			// Joints lying on the head move with it; the chain needs some length past them to swing.
			uint32_t pinned = 1;
			while (pinned < joints && !Hangs(a_scalp, o_chain.joints[pinned]) && a_scalp.Height(o_chain.joints[pinned]) < 0.5f * kHangingHeight)
				++pinned;
			if (static_cast<float>(joints - pinned) * length / static_cast<float>(joints - 1) < kMinChainLength)
				return false;
			o_chain.pinnedJoints = pinned;

			std::vector<float> offsets;
			offsets.reserve(count);
			for (uint32_t t : pieceTris) {
				float best = FLT_MAX;
				const Vec3& p = triCentre[t];
				for (uint32_t j = 0; j + 1 < joints; ++j) {
					const Vec3 a = o_chain.joints[j], d = o_chain.joints[j + 1] - a;
					const float s = std::clamp((p - a).Dot(d) / std::max(d.LengthSquared(), 1e-12f), 0.0f, 1.0f);
					best = std::min(best, (a + d * s - p).Length());
				}
				offsets.push_back(best);
			}
			std::nth_element(offsets.begin(), offsets.begin() + offsets.size() / 2, offsets.end());
			o_chain.radius = std::max(offsets[offsets.size() / 2], 0.25f);
			o_chain.parentBone = DominantBone(rootTri);
			o_chain.triangles = count;
			return true;
		}

		float Generator::ChainPosition(const ChainCurve& a_chain, const Vec3& a_p, float& o_length, float& o_pinned) const
		{
			const auto& chain = a_chain;
			float best = FLT_MAX, at = 0.0f;
			o_length = 0.0f;
			o_pinned = 0.0f;
			for (uint32_t j = 0; j + 1 < chain.joints.size(); ++j) {
				const Vec3 d = chain.joints[j + 1] - chain.joints[j];
				const float length = d.Length();
				const float s = std::clamp((a_p - chain.joints[j]).Dot(d) / std::max(length * length, 1e-12f), 0.0f, 1.0f);
				if (const float distance = (chain.joints[j] + d * s - a_p).LengthSquared(); distance < best) {
					best = distance;
					at = o_length + s * length;
				}
				o_length += length;
				if (j + 1 < chain.pinnedJoints)
					o_pinned = o_length;
			}
			return at;
		}

		void Generator::ChainSkin(uint32_t a_chain, const Vec3& a_p, std::array<uint16_t, 4>& o_bones, std::array<float, 4>& o_weights) const
		{
			// The nearest segment's two joints, blended along it, so the cards bend smoothly at each joint.
			const auto& chain = chains[a_chain];
			float best = FLT_MAX, f = 0.0f;
			uint32_t k = 0;
			for (uint32_t j = 0; j + 1 < chain.joints.size(); ++j) {
				const Vec3 a = chain.joints[j], d = chain.joints[j + 1] - a;
				const float s = std::clamp((a_p - a).Dot(d) / std::max(d.LengthSquared(), 1e-12f), 0.0f, 1.0f);
				const float distance = (a + d * s - a_p).LengthSquared();
				if (distance < best) {
					best = distance;
					k = j;
					f = s;
				}
			}
			const auto first = static_cast<uint16_t>(chain.firstBone + k);
			o_bones = { first, static_cast<uint16_t>(first + 1), 0, 0 };
			o_weights = { 1.0f - f, f, 0.0f, 0.0f };
			if (f > 0.5f) {
				std::swap(o_bones[0], o_bones[1]);
				std::swap(o_weights[0], o_weights[1]);
			}
		}

		void Generator::BindToWoven(const Scalp& a_scalp, std::vector<uint32_t>& io_floating)
		{
			if (woven.empty())
				return;
			struct Sample
			{
				Vec3 p;
				uint32_t piece;
				bool onHead;
			};
			// Woven hair on the head: lying on it, or where a chain leaves it (up to a segment past its pinned joints).
			const auto atHead = [&](uint32_t a_piece, const Vec3& a_p, bool a_hangs) {
				const int32_t line = woven[a_piece].line;
				if (!a_hangs || line < 0)
					return !a_hangs;
				float length = 0.0f, pinned = 0.0f;
				return ChainPosition(lines[line], a_p, length, pinned) <= pinned + kChainSegment;
			};
			PointGrid<Sample> grid(kGatherReach);
			const auto insert = [&](uint32_t a_piece, uint32_t a_tri) {
				const bool onHead = atHead(a_piece, triCentre[a_tri], triHangs[a_tri]);
				grid.Insert(triCentre[a_tri], { triCentre[a_tri], a_piece, onHead });
				for (uint32_t v : tris[a_tri].v)
					grid.Insert(Position(v), { Position(v), a_piece, onHead });
			};
			for (uint32_t i = 0; i < woven.size(); ++i) {
				for (uint32_t t : woven[i].tris)
					insert(i, t);
				for (uint32_t t : woven[i].chosenAway)
					insert(i, t);
			}
			const auto nearest = [&](const Vec3& a_p, bool a_onHead) {
				int32_t piece = -1;
				float best = kGatherReach * kGatherReach;
				grid.Query(a_p, kGatherReach, [&](const Sample& a_s) {
					const float d = (a_s.p - a_p).LengthSquared();
					if ((!a_onHead || a_s.onHead) && d <= best) {
						best = d;
						piece = static_cast<int32_t>(a_s.piece);
					}
				});
				return piece;
			};

			// Inside a hanging braid: within its thickness (and a little) of its centre line.
			const auto insideChain = [&](const ChainCurve& a_line, const Vec3& a_p) {
				const auto& joints = a_line.joints;
				float best = FLT_MAX;
				for (uint32_t j = 0; j + 1 < joints.size(); ++j) {
					const Vec3 d = joints[j + 1] - joints[j];
					const float s = std::clamp((a_p - joints[j]).Dot(d) / std::max(d.LengthSquared(), 1e-12f), 0.0f, 1.0f);
					best = std::min(best, (joints[j] + d * s - a_p).LengthSquared());
				}
				return std::sqrt(best) <= 1.5f * a_line.radius + 0.5f;
			};
			const auto staysOnHead = [&](const CardGuide& a_guide) {
				return std::ranges::none_of(a_guide.path, [&](const PathSample& a_s) { return Hangs(a_scalp, a_s.position); });
			};

			// Hair ending at woven hair on the head (a braid along the scalp, a tie, where a braid
			// leaves the head), or inside a hanging braid, is gathered into it: pulled tight, it
			// stays cards. So is hair starting there that stays on the head: on a tight cap the
			// cards run either way, into the tie or out from it.
			for (uint32_t g = 0; g < guides.size(); ++g) {
				auto& guide = guides[g];
				if (guide.kind != GuideKind::Rooted || ChosenAsStrands(g))
					continue;
				const Vec3 tip = guide.path.back().position;
				const int32_t piece = nearest(tip, false);
				bool gathered = piece >= 0 && (nearest(tip, true) >= 0 || (woven[piece].line >= 0 && insideChain(lines[woven[piece].line], tip)));
				// The first sample may be a root added on the scalp below where the card starts.
				const Vec3 start = guide.path[std::min<size_t>(1, guide.path.size() - 1)].position;
				gathered = gathered || ((nearest(start, true) >= 0 || nearest(guide.path[0].position, true) >= 0) && staysOnHead(guide));
				if (gathered)
					guide.kind = GuideKind::Gathered;
			}

			// Hair starting at woven hair grows from it (a braid's tuft below its end, a tail below a
			// tie), unless it runs back onto the head: then it is the hair gathered into it.
			std::vector<uint32_t> left;
			std::unordered_map<uint64_t, int32_t> tieOf;  // by piece and joint
			for (uint32_t g : io_floating) {
				auto& guide = guides[g];
				const Vec3 start = guide.path[0].position;
				const int32_t piece = nearest(start, false);
				if (piece < 0) {
					left.push_back(g);
					continue;
				}
				const float tipHeight = a_scalp.Height(guide.path.back().position);
				if (tipHeight < kAttachHeight || tipHeight < a_scalp.Height(start) - 0.5f) {
					if (ChosenAsStrands(g))
						left.push_back(g);  // binds as any other floating guide
					else
						guide.kind = GuideKind::Gathered;
					continue;
				}
				const int32_t chain = woven[piece].chain;
				if (const int32_t line = woven[piece].line; line >= 0) {
					// Only the hair below a braid's end grows from it; a stray piece beside it binds as any other.
					float length = 0.0f, pinned = 0.0f;
					if (ChainPosition(lines[line], start, length, pinned) < kTuftStart * length) {
						left.push_back(g);
						continue;
					}
				}
				uint32_t joint = 0;
				if (chain >= 0) {
					std::array<uint16_t, 4> bones{};
					std::array<float, 4> weights{};
					ChainSkin(static_cast<uint32_t>(chain), start, bones, weights);
					joint = bones[0];
				}
				const uint64_t key = (static_cast<uint64_t>(piece) << 32) | joint;
				const auto [it, inserted] = tieOf.try_emplace(key, static_cast<int32_t>(ties.size()));
				if (inserted) {
					ties.emplace_back();
					ties.back().chain = chain;
				}
				auto& tie = ties[it->second];
				tie.centre += start;  // a sum until every tail is in
				++tie.tails;
				guide.kind = GuideKind::Tied;
				guide.tie = it->second;
				guide.ancestor = static_cast<int32_t>(g);
			}
			for (const auto& [key, index] : tieOf) {
				auto& tie = ties[index];
				tie.centre = tie.centre / static_cast<float>(tie.tails);
			}
			for (const auto& guide : guides) {
				if (guide.kind == GuideKind::Tied && guide.tie >= 0)
					ties[guide.tie].radius = std::max(ties[guide.tie].radius, (guide.path[0].position - ties[guide.tie].centre).Length());
			}
			io_floating = std::move(left);
		}

		void Generator::FindTies(const Scalp& a_scalp, std::vector<uint32_t>& io_floating)
		{
			// Rooted hair ending where a floating card guide starts, running into it (as BindToScalp's
			// continuation, but further: a cap's painted hair may stop short of the tie).
			PointGrid<uint32_t> tips(kTieReach);
			for (uint32_t g = 0; g < guides.size(); ++g) {
				if (guides[g].kind == GuideKind::Rooted)
					tips.Insert(guides[g].path.back().position, g);
			}
			struct Tail
			{
				uint32_t guide;
				std::vector<uint32_t> feeders;
			};
			std::vector<Tail> tails;
			for (uint32_t g : io_floating) {
				const auto& guide = guides[g];
				const Vec3 p0 = guide.path[0].position;
				const Vec3 t0 = InitialTangent(guide);
				Tail tail{ g, {} };
				tips.Query(p0, kTieReach, [&](const uint32_t& a_h) {
					const auto& path = guides[a_h].path;
					const Vec3 tip = path.back().position;
					const Vec3 d = path.size() > 1 ? tip - path[path.size() - 2].position : kZero;
					const Vec3 tq = d.LengthSquared() > 1e-12f ? d.Normalized() : kZero;
					if ((p0 - tip).Length() <= kTieReach && tq.Dot(t0) >= kMergeAlignment && (p0 - tip).Dot(tq) >= -1.0f)
						tail.feeders.push_back(a_h);
				});
				if (!tail.feeders.empty())
					tails.push_back(std::move(tail));
			}
			if (tails.empty())
				return;

			// Tails starting close together share a tie. It is one if hair from across the scalp
			// gathers into it, close to the head: a layer carrying on from the one above it, lower
			// down, is not.
			DisjointSets sets(tails.size());
			for (uint32_t i = 0; i < tails.size(); ++i)
				for (uint32_t j = i + 1; j < tails.size(); ++j)
					if ((guides[tails[i].guide].path[0].position - guides[tails[j].guide].path[0].position).Length() <= kTieRadius)
						sets.Unite(i, j);
			std::unordered_map<uint32_t, std::vector<uint32_t>> clusters;
			for (uint32_t i = 0; i < tails.size(); ++i)
				clusters[sets.Find(i)].push_back(i);
			std::vector<bool> tied(guides.size(), false);
			for (const auto& [root, members] : clusters) {
				std::vector<uint32_t> feeders;
				Vec3 centre;
				for (uint32_t i : members) {
					feeders.insert(feeders.end(), tails[i].feeders.begin(), tails[i].feeders.end());
					centre += guides[tails[i].guide].path[0].position;
				}
				std::ranges::sort(feeders);
				feeders.erase(std::unique(feeders.begin(), feeders.end()), feeders.end());
				centre = centre / static_cast<float>(members.size());
				if (feeders.size() < kMinTieFeeders || a_scalp.Height(centre) > kMaxTieHeight)
					continue;
				// A tie is compact, and the hair converges on it: from roots spread across the
				// scalp to tips close together. Layers carrying on from the ones above them form a
				// row along the head instead, and their hair runs side by side.
				bool compact = true;
				for (uint32_t i : members)
					compact = compact && (guides[tails[i].guide].path[0].position - centre).Length() <= kTieRadius;
				Vec3 rootLo(FLT_MAX), rootHi(-FLT_MAX), tipLo(FLT_MAX), tipHi(-FLT_MAX);
				for (uint32_t h : feeders) {
					rootLo = Vec3::Min(rootLo, guides[h].path[0].position);
					rootHi = Vec3::Max(rootHi, guides[h].path[0].position);
					tipLo = Vec3::Min(tipLo, guides[h].path.back().position);
					tipHi = Vec3::Max(tipHi, guides[h].path.back().position);
				}
				const float rootSpread = (rootHi - rootLo).Length(), tipSpread = (tipHi - tipLo).Length();
				if (!compact || rootSpread < kMinTieSpread || rootSpread < kTieConvergence * tipSpread)
					continue;

				// The tail: the guides the gathered hair runs into, and any other starting at the tie.
				// A ponytail's tail is several cards; one card carrying on below others is a layer.
				std::vector<uint32_t> tail;
				for (uint32_t g : io_floating) {
					const Vec3 start = guides[g].path[0].position;
					const bool member = std::ranges::any_of(members, [&](uint32_t i) { return tails[i].guide == g; });
					if (!tied[g] && (member || (start - centre).Length() <= kTieRadius))
						tail.push_back(g);
				}
				if (tail.size() < kMinTieTails)
					continue;

				const auto index = static_cast<int32_t>(ties.size());
				Tie tie;
				tie.centre = centre;
				tie.gathered = static_cast<uint32_t>(feeders.size());
				for (uint32_t h : feeders) {
					if (!ChosenAsStrands(h))
						guides[h].kind = GuideKind::Gathered;
				}
				for (uint32_t g : tail) {
					const Vec3 start = guides[g].path[0].position;
					guides[g].kind = GuideKind::Tied;
					guides[g].tie = index;
					guides[g].ancestor = static_cast<int32_t>(g);
					tied[g] = true;
					++tie.tails;
					tie.radius = std::max(tie.radius, (start - centre).Length());
				}
				ties.push_back(tie);
			}
			std::erase_if(io_floating, [&](uint32_t g) { return tied[g]; });

			// With a ponytail, all the hair lying on the head that runs to its tie is pulled into it,
			// not only the cards reaching it: a cap's front row stops short of the tie, and on a
			// tight cap the cards run either way. Hair leaving the head (a fringe, side locks) or
			// running elsewhere stays loose.
			for (const auto& tie : ties) {
				if (tie.chain >= 0)
					continue;
				const float reach = tie.radius + kTieRadius;
				for (uint32_t g = 0; g < guides.size(); ++g) {
					auto& guide = guides[g];
					if (guide.kind != GuideKind::Rooted || ChosenAsStrands(g))
						continue;
					const Vec3 start = guide.path[0].position, tip = guide.path.back().position;
					const auto n = static_cast<uint32_t>(guide.path.size());
					const Vec3 end = n > 1 ? tip - guide.path[n - 2].position : kZero;
					const Vec3 toTie = tie.centre - tip;
					const bool runsToTie = (tip - tie.centre).Length() <= reach || (start - tie.centre).Length() <= reach ||
					                       (end.LengthSquared() > 1e-12f && end.Normalized().Dot(toTie.Normalized()) >= 0.7f);
					if (runsToTie && std::ranges::none_of(guide.path, [&](const PathSample& a_s) { return Hangs(a_scalp, a_s.position); }))
						guide.kind = GuideKind::Gathered;
				}
			}
		}

		bool Generator::ChosenAsStrands(uint32_t a_guide) const
		{
			if (a_guide >= ownPaths.size() || ownPaths[a_guide].empty())
				return false;
			size_t chosen = 0;
			for (const auto& sample : ownPaths[a_guide])
				chosen += sample.tri < choice.size() && choice[sample.tri] == RegionChoice::Strands;
			return 2 * chosen > ownPaths[a_guide].size();
		}

		void Generator::LabelTriangles(Result& o_result)
		{
			const auto triCount = static_cast<uint32_t>(tris.size());
			regionOf.assign(triCount, Region::Strands);
			chainOf.assign(triCount, -1);
			std::vector<bool> labelled(triCount, false);
			for (const auto& piece : woven) {
				for (uint32_t t : piece.tris) {
					regionOf[t] = piece.region;
					chainOf[t] = piece.chain;
					labelled[t] = true;
				}
			}

			// Converted triangles go with the card guide nearest them on their sheet: hair gathered
			// into a tie or a braid stays cards, and so does hair too far from the head to grow from it.
			struct GuideSample
			{
				Vec3 p;
				uint32_t guide;
				int32_t component;
			};
			PointGrid<GuideSample> grid(spacing);
			for (uint32_t g = 0; g < ownPaths.size(); ++g)
				for (const auto& sample : ownPaths[g])
					grid.Insert(sample.position, { sample.position, g, tris[sample.tri].component });
			const float reach = 2.0f * spacing;
			for (uint32_t t = 0; t < triCount; ++t) {
				if (labelled[t] || !tris[t].valid)
					continue;
				float best = reach * reach;
				int32_t guide = -1;
				grid.Query(triCentre[t], reach, [&](const GuideSample& a_s) {
					const float d = (a_s.p - triCentre[t]).LengthSquared();
					if (a_s.component == tris[t].component && d <= best) {
						best = d;
						guide = static_cast<int32_t>(a_s.guide);
					}
				});
				const GuideKind kind = guide >= 0 ? guides[guide].kind : GuideKind::Free;
				regionOf[t] = kind == GuideKind::Gathered || kind == GuideKind::Dropped ? Region::Cards : Region::Strands;
				labelled[t] = true;
			}

			// Triangles the conversion skipped: a copy on the same vertices, or the back face of a
			// kept sheet, goes with that; any other with the nearest labelled triangle.
			std::unordered_map<uint64_t, uint32_t> byPosition;
			for (uint32_t t = 0; t < triCount; ++t)
				if (labelled[t] && tris[t].positionKey != 0)
					byPosition.try_emplace(tris[t].positionKey, t);
			PointGrid<uint32_t> labelledGrid(kContactRadius);
			for (uint32_t t = 0; t < triCount; ++t)
				if (labelled[t])
					labelledGrid.Insert(triCentre[t], t);
			for (uint32_t t = 0; t < triCount; ++t) {
				if (labelled[t])
					continue;
				int32_t from = -1;
				if (tris[t].sameAs >= 0 && labelled[tris[t].sameAs])
					from = tris[t].sameAs;
				else if (const auto it = byPosition.find(tris[t].positionKey); tris[t].positionKey != 0 && it != byPosition.end())
					from = static_cast<int32_t>(it->second);
				else {
					float best = 4.0f * kContactRadius * kContactRadius;
					labelledGrid.Query(triCentre[t], 2.0f * kContactRadius, [&](const uint32_t& a_q) {
						const float d = (triCentre[a_q] - triCentre[t]).LengthSquared();
						if (d <= best) {
							best = d;
							from = static_cast<int32_t>(a_q);
						}
					});
				}
				if (from >= 0) {
					regionOf[t] = regionOf[from];
					chainOf[t] = chainOf[from];
				}
			}

			// A region chosen by hand is the region reported. Cards and Chain choices are woven pieces
			// already (a Chain choice that does not hang stays plain cards); a Strands choice wins over
			// the card guide nearest it, even one dropped for starting far from the scalp, whose
			// triangles then show neither cards nor strands.
			for (uint32_t t = 0; t < triCount; ++t) {
				if (t < choice.size() && choice[t] == RegionChoice::Strands) {
					regionOf[t] = Region::Strands;
					chainOf[t] = -1;
				}
			}

			o_result.triangleRegions = regionOf;
			for (uint32_t t = 0; t < triCount; ++t) {
				o_result.stats.cardTriangles += regionOf[t] == Region::Cards;
				o_result.stats.chainTriangles += regionOf[t] == Region::Chain;
			}
			for (auto& chain : chains)
				chain.triangles = 0;
			for (uint32_t t = 0; t < triCount; ++t)
				if (chainOf[t] >= 0)
					++chains[chainOf[t]].triangles;
		}

		void Generator::BuildCards(Result& o_result) const
		{
			const bool skinned = mesh.boneIndices.size() == mesh.positions.size() && mesh.boneWeights.size() == mesh.positions.size();
			const bool haveFrame = mesh.tangents.size() == mesh.positions.size() && mesh.bitangents.size() == mesh.positions.size();
			const bool haveNormals = mesh.normals.size() == mesh.positions.size();

			// Without the mesh's own tangent frame, the texture's U and V directions on the surface.
			std::vector<Vec3> alongU, alongV, normals;
			if (!haveFrame || !haveNormals) {
				alongU.assign(mesh.positions.size(), kZero);
				alongV.assign(mesh.positions.size(), kZero);
				normals.assign(mesh.positions.size(), kZero);
				for (uint32_t t = 0; t < tris.size(); ++t) {
					if (regionOf[t] == Region::Strands)
						continue;
					const auto& v = tris[t].v;
					const Vec3 a = Position(v[0]), b = Position(v[1]), c = Position(v[2]);
					const Vec3 e1 = b - a, e2 = c - a;
					const Vec3 n = e1.Cross(e2);
					const float du1 = mesh.uvs[v[1]].x - mesh.uvs[v[0]].x, dv1 = mesh.uvs[v[1]].y - mesh.uvs[v[0]].y;
					const float du2 = mesh.uvs[v[2]].x - mesh.uvs[v[0]].x, dv2 = mesh.uvs[v[2]].y - mesh.uvs[v[0]].y;
					const float det = du1 * dv2 - du2 * dv1;
					for (uint32_t i : v) {
						normals[i] += n;
						if (std::abs(det) > 1e-10f) {
							alongU[i] += (e1 * dv2 - e2 * dv1) / det * (0.5f * n.Length());
							alongV[i] += (e2 * du1 - e1 * du2) / det * (0.5f * n.Length());
						}
					}
				}
			}

			std::unordered_map<uint64_t, uint32_t> remap;
			for (uint32_t t = 0; t < tris.size(); ++t) {
				if (regionOf[t] == Region::Strands)
					continue;
				const auto& v = tris[t].v;
				if (v[0] >= mesh.positions.size() || v[1] >= mesh.positions.size() || v[2] >= mesh.positions.size())
					continue;
				const int32_t chain = regionOf[t] == Region::Chain ? chainOf[t] : -1;
				for (uint32_t i : v) {
					const uint64_t key = (static_cast<uint64_t>(i) << 8) | static_cast<uint64_t>(chain + 1);
					const auto [it, inserted] = remap.try_emplace(key, static_cast<uint32_t>(o_result.cardVertices.size()));
					if (inserted) {
						CardVertex vertex;
						vertex.position = Position(i);
						vertex.uv = mesh.uvs[i];
						vertex.source = i;
						vertex.normal = haveNormals ? mesh.normals[i] : normals[i].Normalized();
						if (haveFrame) {
							vertex.tangent = mesh.tangents[i];
							vertex.bitangent = mesh.bitangents[i];
						} else {
							const Vec3 n = vertex.normal;
							vertex.tangent = (alongU[i] - n * alongU[i].Dot(n)).Normalized();
							vertex.bitangent = (alongV[i] - n * alongV[i].Dot(n)).Normalized();
						}
						if (chain >= 0) {
							ChainSkin(static_cast<uint32_t>(chain), vertex.position, vertex.bones, vertex.weights);
						} else if (skinned) {
							vertex.bones = mesh.boneIndices[i];
							vertex.weights = mesh.boneWeights[i];
						} else {
							vertex.weights[0] = 1.0f;
						}
						o_result.cardVertices.push_back(vertex);
					}
					o_result.cardIndices.push_back(it->second);
				}
			}
		}

		Vec3 Generator::LiftNormal(uint32_t a_tri, const Vec3& a_pos) const
		{
			// Always away from the head, whichever way the card's winding faces.
			Vec3 n = tris[a_tri].normal;
			if (n.Dot(a_pos - headCentre) < 0.0f)
				n = -n;
			return n;
		}

		PointAttributes Generator::Attributes(uint32_t a_tri, const Vec3& a_pos) const
		{
			const auto& tri = tris[a_tri];
			const Vec3 bary = ClampBarycentric(Barycentric(a_pos, Position(tri.v[0]), Position(tri.v[1]), Position(tri.v[2])));
			const float weights[3] = { bary.x, bary.y, bary.z };

			PointAttributes out;
			Vec3 normal;
			std::array<std::pair<uint16_t, float>, 12> influences{};
			size_t influenceCount = 0;
			const bool skinned = mesh.boneIndices.size() == mesh.positions.size() && mesh.boneWeights.size() == mesh.positions.size();
			for (int i = 0; i < 3; ++i) {
				const uint32_t v = tri.v[i];
				normal += shadingNormal[v] * weights[i];
				out.uv += mesh.uvs[v] * weights[i];
				if (!skinned)
					continue;
				for (int j = 0; j < 4; ++j) {
					const float w = mesh.boneWeights[v][j] * weights[i];
					if (!(w > 0.0f))
						continue;
					const uint16_t bone = mesh.boneIndices[v][j];
					auto it = std::find_if(influences.begin(), influences.begin() + influenceCount, [&](const auto& e) { return e.first == bone; });
					if (it != influences.begin() + influenceCount)
						it->second += w;
					else
						influences[influenceCount++] = { bone, w };
				}
			}
			if (normal.Dot(a_pos - headCentre) < 0.0f && tri.normal.Dot(normal) < 0.0f)
				normal = -normal;  // back side of a double-sided card: shade the outside
			normal.Normalize();
			out.normal = normal;

			// The four strongest bones, renormalised.
			std::sort(influences.begin(), influences.begin() + influenceCount, [](const auto& l, const auto& r) { return l.second > r.second; });
			influenceCount = std::min<size_t>(influenceCount, 4);
			float total = 0.0f;
			for (size_t i = 0; i < influenceCount; ++i)
				total += influences[i].second;
			for (size_t i = 0; i < influenceCount; ++i) {
				out.bones[i] = influences[i].first;
				out.weights[i] = influences[i].second / total;
			}
			if (influenceCount == 0)
				out.weights[0] = 1.0f;  // unweighted vertex: follow bone 0 rather than vanish
			return out;
		}

		void Generator::BuildStrands(const Scalp& a_scalp, Result& o_result)
		{
			// Each kept card guide is one clump, as in HairCS's wrappers and in TressFX's or
			// Unreal's guide-and-follower hair: `density` strands per unit of card width,
			// rooted round the guide's scalp root and spread across its share of the card, then
			// drawn towards the guide by `clumpStrength` towards the tip.
			std::vector<uint32_t> kept;
			for (uint32_t g = 0; g < guides.size(); ++g)
				if (guides[g].kind != GuideKind::Dropped && guides[g].kind != GuideKind::Continued && guides[g].kind != GuideKind::Gathered && guides[g].path.size() >= 2 && guides[g].path.back().s >= Limits::kMinStrandLength)
					kept.push_back(g);
			if (kept.empty())
				return;

			const bool area = o_result.stats.seedingUsed == Seeding::Area;
			std::unordered_map<int32_t, uint32_t> sharing;  // card guides per scalp root
			float total = 0.0f;
			for (uint32_t g : kept) {
				++sharing[guides[g].ancestor];
				const float painted = useCoverage ? std::clamp(guides[g].coverage / kFullCoverage, kMinCoverageShare, 1.0f) : 1.0f;
				total += area ? 1.0f : settings.density * spacing * painted;
			}
			const float acceptance = total > Limits::kMaxStrands ? Limits::kMaxStrands / total : 1.0f;
			// Each card guide's random numbers are its own, keyed by where it starts on the cards.
			std::vector<uint32_t> keys(guides.size(), 0);
			for (uint32_t g = 0; g < guides.size() && g < ownPaths.size(); ++g) {
				const auto& start = ownPaths[g].front();
				uint32_t key = Mix(start.tri);
				for (int i = 0; i < 3; ++i)
					key = Mix(key ^ static_cast<uint32_t>(static_cast<int32_t>(std::lround(start.position[i] * 100.0f))));
				keys[g] = key;
			}
			std::vector<uint32_t> counts(guides.size(), 0);
			uint32_t strandTotal = 0;
			for (uint32_t g : kept) {
				KeyedRandom random(4, keys[g], settings.seed);
				const float painted = useCoverage ? std::clamp(guides[g].coverage / kFullCoverage, kMinCoverageShare, 1.0f) : 1.0f;
				counts[g] = area ? 1u : static_cast<uint32_t>(settings.density * spacing * painted * acceptance + random());
				counts[g] = std::min(counts[g], Limits::kMaxStrands - strandTotal);
				strandTotal += counts[g];
			}
			for (uint32_t g = 0; g < guides.size(); ++g)
				o_result.guides[g].strands = counts[g];

			// Roots may spread only over the scalp under rooted hair, so none crosses a hairline.
			PointGrid<Vec3> scalpRoots(spacing);
			for (uint32_t g : kept)
				if (guides[g].kind == GuideKind::Rooted || guides[g].kind == GuideKind::Bridged)
					scalpRoots.Insert(guides[g].path[0].position, guides[g].path[0].position);
			const auto onScalpUnderHair = [&](const Vec3& a_p) {
				bool found = false;
				scalpRoots.Query(a_p, spacing, [&](const Vec3& a_q) { found = found || (a_q - a_p).LengthSquared() <= spacing * spacing; });
				return found;
			};

			// One point count for the whole asset, enough for all but the longest strands.
			std::vector<float> lengths;
			for (uint32_t g : kept)
				if (counts[g] > 0)
					lengths.push_back(guides[g].path.back().s * settings.lengthScale);
			if (lengths.empty())
				return;
			const size_t p95 = std::min(lengths.size() * 95 / 100, lengths.size() - 1);
			std::nth_element(lengths.begin(), lengths.begin() + p95, lengths.end());
			const uint32_t points = std::clamp(static_cast<uint32_t>(std::ceil(lengths[p95] / settings.segmentLength)) + 1, Limits::kMinPointsPerStrand, Limits::kMaxPointsPerStrand);
			o_result.pointsPerStrand = points;
			o_result.points.reserve(static_cast<size_t>(strandTotal) * points);
			o_result.strands.reserve(strandTotal);

			const float exponent = 0.35f + 0.65f * (1.0f - settings.clumpStrength);
			std::vector<Vec3> tangent, lift, across;
			std::vector<PointAttributes> attributes;
			double lengthSum = 0.0;
			for (uint32_t g : kept) {
				if (counts[g] == 0)
					continue;
				const auto& path = guides[g].path;
				const auto n = static_cast<uint32_t>(path.size());
				const float guideLength = path.back().s;

				// A frame along the guide: tangent, lift (the card normal facing away from the
				// head, square to the tangent) and across the card.
				tangent.resize(n);
				lift.resize(n);
				across.resize(n);
				attributes.resize(n);
				for (uint32_t i = 0; i < n; ++i) {
					const Vec3 d = path[std::min(i + 1, n - 1)].position - path[i > 0 ? i - 1 : 0].position;
					tangent[i] = d.LengthSquared() > 1e-12f ? d.Normalized() : (i > 0 ? tangent[i - 1] : kUnitZ);
					Vec3 up = LiftNormal(path[i].tri, path[i].position);
					up -= tangent[i] * up.Dot(tangent[i]);
					lift[i] = up.LengthSquared() > 1e-6f ? up.Normalized() : (i > 0 ? lift[i - 1] : (path[i].position - headCentre).Normalized());
					across[i] = tangent[i].Cross(lift[i]);
					attributes[i] = Attributes(path[i].tri, path[i].position);
				}
				const auto at = [&](float a_s, uint32_t& io_i) {
					while (io_i + 2 < n && path[io_i + 1].s < a_s)
						++io_i;
					const float span = path[io_i + 1].s - path[io_i].s;
					return span > 1e-6f ? std::clamp((a_s - path[io_i].s) / span, 0.0f, 1.0f) : 0.0f;
				};

				// How far along the guide it lies on the scalp: strands may root anywhere there,
				// under the hair that started before them, as on a real head. A card combed back
				// from the hairline otherwise grows all its hair from the hairline.
				float scalpRun = 0.0f;
				for (uint32_t k = 0; k < n && a_scalp.Height(path[k].position) <= kAttachHeight; ++k)
					scalpRun = path[k].s;
				const bool tied = guides[g].kind == GuideKind::Tied;
				const float maxAlong = area || tied || guides[g].kind == GuideKind::Free ? 0.0f : std::min(scalpRun, kMaxRootAlong * guideLength);

				// Roots spread over a guide's share of the card width, wider where several card
				// guides share one scalp root.
				const float shared = static_cast<float>(sharing[guides[g].ancestor]);
				const float rootSpread = area ? 0.0f : 0.5f * spacing * std::min(std::sqrt(shared), 4.0f);
				const float clumpRandom = Hash01(keys[g] + settings.seed);
				KeyedRandom random(5, keys[g], settings.seed);
				// Hair growing from a chain's end rides on it: every point skinned to the joints at its root.
				const int32_t tieChain = tied && guides[g].tie >= 0 ? ties[guides[g].tie].chain : -1;
				std::array<uint16_t, 4> chainBones{};
				std::array<float, 4> chainWeights{};
				if (tieChain >= 0)
					ChainSkin(static_cast<uint32_t>(tieChain), path[0].position, chainBones, chainWeights);

				for (uint32_t j = 0; j < counts[g]; ++j) {
					// Stratified across the card, so strands keep their order from root to tip.
					const float b = area ? 0.0f : (j + random()) / static_cast<float>(counts[g]) - 0.5f;
					const float a = area ? 0.0f : random() - 0.5f;
					const float start = maxAlong > 0.0f && random() >= kScalpRootShare ? random() * maxAlong : 0.0f;

					// The scalp frame under the guide where the strand starts.
					uint32_t i = 0;
					float f = at(start, i);
					const Vec3 base = Vec3::Lerp(path[i].position, path[i + 1].position, f);
					Vec3 radial = base - a_scalp.centre;
					const float baseRadius = radial.Length();
					radial = baseRadius > 1e-6f ? radial / baseRadius : kUnitZ;
					const float rootRadius = std::min(baseRadius, a_scalp.Radius(radial));
					const Vec3 t0 = Vec3::Lerp(tangent[i], tangent[i + 1], f);
					Vec3 scalpAlong = t0 - radial * t0.Dot(radial);
					scalpAlong = scalpAlong.LengthSquared() > 1e-6f ? scalpAlong.Normalized() : across[i].Cross(radial).Normalized();
					const Vec3 scalpAcross = radial.Cross(scalpAlong);
					Vec3 strandRoot = a_scalp.centre + radial * rootRadius;
					if (tied) {
						// Round where it starts at its tie, across the card and off it.
						strandRoot = path[0].position + (across[0] * (2.0f * b) + lift[0] * a) * rootSpread;
					} else if (!area) {
						// Along the guide the start already spreads the roots; at the guide's own
						// root they spread both ways, and only over scalp under rooted hair, so
						// none crosses a hairline.
						const float spread = start > 0.0f ? 0.5f * spacing : rootSpread;
						const float alongShare = start > 0.0f ? 0.0f : a;
						for (float shrink = 1.0f; shrink > 0.1f; shrink *= 0.5f) {
							Vec3 p = strandRoot + (scalpAcross * (2.0f * b) + scalpAlong * alongShare) * (spread * shrink);
							p = a_scalp.centre + (p - a_scalp.centre).Normalized() * rootRadius;
							if (shrink < 0.2f || start > 0.0f || onScalpUnderHair(p)) {
								strandRoot = p;
								break;
							}
						}
					} else {
						strandRoot = path[0].position;
					}

					const float length = std::max(Limits::kMinStrandLength, (guideLength - start) * settings.lengthScale * (1.0f - settings.tipVariation * random()));
					const float width = b * spacing;
					const float layer = random() * settings.layerJitter;
					const auto offsetAt = [&](uint32_t a_i, float a_f, float a_s, float a_t) {
						const Vec3 t = Vec3::Lerp(tangent[a_i], tangent[a_i + 1], a_f).Normalized();
						const Vec3 up = Vec3::Lerp(lift[a_i], lift[a_i + 1], a_f).Normalized();
						const Vec3 side = Vec3::Lerp(across[a_i], across[a_i + 1], a_f).Normalized();
						const float depth = area ? layer : layer + settings.volume * Smoothstep(a_t);
						Vec3 offset = side * width + up * depth;
						if (settings.clumpTwist != 0.0f) {
							const float angle = 2.0f * kPi * settings.clumpTwist * a_s;
							offset = offset * std::cos(angle) + t.Cross(offset) * std::sin(angle);
						}
						const float pull = settings.clumpStrength * std::pow(std::clamp(a_s / guideLength, 0.0f, 1.0f), exponent);
						return offset * (1.0f - pull);
					};

					const Vec3 rootCorrection = strandRoot - (base + offsetAt(i, f, start, 0.0f));
					const float blend = std::min(kRootBlend, 0.3f * length);
					for (uint32_t k = 0; k < points; ++k) {
						const float t = static_cast<float>(k) / (points - 1);
						const float s = start + length * t;
						f = at(s, i);
						const Vec3 centre = Vec3::Lerp(path[i].position, path[i + 1].position, f);
						const Vec3 offset = offsetAt(i, f, s, t);
						StrandPoint point;
						point.position = centre + offset + rootCorrection * (1.0f - Smoothstep(blend > 1e-4f ? (s - start) / blend : 1.0f));
						point.t = t;
						const auto& nearest = attributes[f < 0.5f ? i : i + 1];
						point.bones = tieChain >= 0 ? chainBones : nearest.bones;
						point.weights = tieChain >= 0 ? chainWeights : nearest.weights;
						point.normal = Vec3::Lerp(attributes[i].normal, attributes[i + 1].normal, f).Normalized();
						if (point.normal == kZero)
							point.normal = nearest.normal;
						point.uv = attributes[i].uv + (attributes[i + 1].uv - attributes[i].uv) * f;
						// Across the card in texture space too, so a clump shows the card's painted strands.
						const Triangle& tri = tris[path[f < 0.5f ? i : i + 1].tri];
						const float uu = tri.dPdu.Dot(tri.dPdu), uvDot = tri.dPdu.Dot(tri.dPdv), vv = tri.dPdv.Dot(tri.dPdv);
						const float det = uu * vv - uvDot * uvDot;
						if (std::abs(det) > 1e-12f) {
							const float du = (vv * tri.dPdu.Dot(offset) - uvDot * tri.dPdv.Dot(offset)) / det;
							const float dv = (uu * tri.dPdv.Dot(offset) - uvDot * tri.dPdu.Dot(offset)) / det;
							point.uv += Vec2(std::clamp(du, -kMaxUVOffset, kMaxUVOffset), std::clamp(dv, -kMaxUVOffset, kMaxUVOffset));
						}
						o_result.points.push_back(point);
					}
					Strand strand;
					strand.length = length;
					strand.random = random();
					strand.clumpRandom = clumpRandom;
					strand.cardGuide = g;
					strand.scalpRooted = guides[g].kind != GuideKind::Free && !tied;
					o_result.strands.push_back(strand);
					lengthSum += length;
				}
			}
			o_result.averageLength = o_result.strands.empty() ? 0.0f : static_cast<float>(lengthSum / static_cast<double>(o_result.strands.size()));
		}

		void Generator::Shuffle(Result& o_result)
		{
			const uint32_t points = o_result.pointsPerStrand;
			std::vector<uint32_t> order(o_result.StrandCount());
			std::iota(order.begin(), order.end(), 0u);
			std::shuffle(order.begin(), order.end(), rng);
			std::vector<StrandPoint> shuffledPoints(o_result.points.size());
			std::vector<Strand> shuffledStrands(o_result.strands.size());
			for (uint32_t i = 0; i < order.size(); ++i) {
				shuffledStrands[i] = o_result.strands[order[i]];
				std::copy_n(&o_result.points[static_cast<size_t>(order[i]) * points], points, &shuffledPoints[static_cast<size_t>(i) * points]);
			}
			o_result.points = std::move(shuffledPoints);
			o_result.strands = std::move(shuffledStrands);
		}

		void Generator::AssignGuides(Result& o_result) const
		{
			const uint32_t strands = o_result.StrandCount();
			const uint32_t points = o_result.pointsPerStrand;
			if (strands == 0 || points < 2)
				return;
			// The list is shuffled, so its first strands are an even thinning of the hair.
			const uint32_t guideCount = std::min(strands, std::clamp((strands + Limits::kStrandsPerGuide - 1) / Limits::kStrandsPerGuide, Limits::kMinGuides, Limits::kMaxGuides));
			o_result.guideCount = guideCount;

			// The point a_distance from a strand's root (clamped to its ends).
			const auto at = [&](uint32_t a_strand, float a_distance) {
				const float length = std::max(o_result.strands[a_strand].length, 1e-4f);
				const float x = std::clamp(a_distance / length, 0.0f, 1.0f) * (points - 1);
				const uint32_t i = std::min(static_cast<uint32_t>(x), points - 2);
				const StrandPoint* p = &o_result.points[static_cast<size_t>(a_strand) * points];
				return Vec3::Lerp(p[i].position, p[i + 1].position, x - i);
			};
			const auto cellOf = [](float a_value) { return static_cast<int32_t>(std::floor(a_value / kGuideSearchCell)); };
			std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
			for (uint32_t g = 0; g < guideCount; ++g) {
				const Vec3& root = o_result.points[static_cast<size_t>(g) * points].position;
				grid[CellKey(cellOf(root.x), cellOf(root.y), cellOf(root.z))].push_back(g);
			}

			// A strand follows the nearby guide that runs most like it: closest at its root,
			// middle and tip, each compared with the guide's point at the same distance from
			// the root. Guides shorter than the strand lose on the tip, and guides of another
			// lock that only share the root area lose on the middle and tip.
			for (uint32_t s = 0; s < strands; ++s) {
				if (s < guideCount) {
					o_result.strands[s].guide = s;
					continue;
				}
				const float length = o_result.strands[s].length;
				const Vec3 probes[3] = { at(s, 0.0f), at(s, 0.5f * length), at(s, length) };
				const auto cost = [&](uint32_t a_guide) {
					float sum = 0.0f;
					for (int k = 0; k < 3; ++k)
						sum += (probes[k] - at(a_guide, 0.5f * k * length)).LengthSquared();
					return sum;
				};
				uint32_t best = 0;
				float bestCost = FLT_MAX;
				const int32_t cx = cellOf(probes[0].x), cy = cellOf(probes[0].y), cz = cellOf(probes[0].z);
				for (int32_t ring = 1; ring <= kGuideSearchRings && bestCost == FLT_MAX; ring *= 2) {
					for (int32_t dz = -ring; dz <= ring; ++dz) {
						for (int32_t dy = -ring; dy <= ring; ++dy) {
							for (int32_t dx = -ring; dx <= ring; ++dx) {
								const auto it = grid.find(CellKey(cx + dx, cy + dy, cz + dz));
								if (it == grid.end())
									continue;
								for (uint32_t g : it->second) {
									if (const float c = cost(g); c < bestCost) {
										bestCost = c;
										best = g;
									}
								}
							}
						}
					}
				}
				if (bestCost == FLT_MAX) {
					for (uint32_t g = 0; g < guideCount; ++g) {
						if (const float c = cost(g); c < bestCost) {
							bestCost = c;
							best = g;
						}
					}
				}
				o_result.strands[s].guide = best;
			}
		}

		void Generator::FitHeadCollider(Result& o_result) const
		{
			o_result.headBone = FindHeadBone();
			o_result.headCentre = headCentre;
			if (o_result.points.empty())
				return;
			// Just inside all but the innermost few strand points: those lie on the scalp or
			// are tucked under it, and the styled shape must never collide.
			std::vector<float> distances;
			distances.reserve(o_result.points.size());
			for (const auto& point : o_result.points)
				distances.push_back((point.position - headCentre).Length());
			const size_t k = static_cast<size_t>(static_cast<float>(distances.size()) * kHeadRadiusPercentile);
			std::nth_element(distances.begin(), distances.begin() + k, distances.end());
			o_result.headRadius = std::clamp(distances[k] * 0.95f, kMinHeadRadius, kMaxHeadRadius);
		}

		bool Generator::Run(Result& o_result, std::string& o_error)
		{
			o_result = {};
			o_result.stats.totalTriangles = mesh.TriangleCount();
			if (mesh.positions.empty() || mesh.indices.empty()) {
				o_error = "empty mesh";
				return false;
			}
			if (mesh.uvs.size() != mesh.positions.size()) {
				o_error = "the mesh has no texture coordinates";
				return false;
			}
			step = std::clamp(settings.segmentLength * 0.5f, 0.2f, 1.0f);

			Weld();
			BuildTriangles();
			BuildAdjacency();
			BuildComponents();
			DropBackFaces();
			headCentre = FindHeadCentre();
			o_result.stats.flowMapShare = ChooseFlow();
			// Hair chosen by hand to run the other way: its root is where the conversion found its tip.
			for (uint32_t t = 0; t < tris.size() && t < settings.triangleFlow.size(); ++t)
				if (settings.triangleFlow[t] == FlowChoice::Reverse && tris[t].valid)
					tris[t].flow = -tris[t].flow;
			o_result.triangleDirections.assign(tris.size(), kZero);
			for (uint32_t t = 0; t < tris.size(); ++t)
				if (tris[t].valid)
					o_result.triangleDirections[t] = tris[t].flow;
			BuildVertexFields();

			if (std::ranges::none_of(tris, [](const Triangle& t) { return t.valid; })) {
				o_error = "no triangle has a usable texture flow";
				return false;
			}

			visits.assign(tris.size(), 0);
			visitStamp.assign(tris.size(), 0);
			traceStamp.assign(tris.size(), 0);

			FitScalp(o_result.scalp);

			// Chain joints are bones numbered after the mesh's own.
			chainBoneBase = static_cast<uint32_t>(mesh.boneNames.size());
			for (const auto& bones : mesh.boneIndices)
				for (uint16_t bone : bones)
					chainBoneBase = std::max<uint32_t>(chainBoneBase, bone + 1u);
			// Braids, ties and buns stay cards: what is left of the cards becomes strands.
			ResolveChoices();
			FindWoven(o_result.scalp);
			o_result.stats.convertedTriangles = static_cast<uint32_t>(std::ranges::count_if(tris, [](const Triangle& t) { return t.valid; }));

			Seeding mode = settings.seeding;
			if (mode != Seeding::Area && o_result.stats.convertedTriangles > 0) {
				SeedRoots();
				SeedFill();
				if (mode == Seeding::Auto) {
					// Short hair if most of the traced hair (by length, not by count) lies in short
					// card guides. Dense long hair yields many short streamlines too (from card side
					// edges, between transparent gaps, through small fill triangles), and those can
					// outnumber the long ones.
					std::vector<float> lengths;
					double total = 0.0;
					for (const auto& g : guides) {
						lengths.push_back(g.path.back().s);
						total += g.path.back().s;
					}
					std::ranges::sort(lengths);
					float weightedMedian = 0.0f;
					double below = 0.0;
					for (float length : lengths) {
						below += length;
						weightedMedian = length;
						if (below >= 0.5 * total)
							break;
					}
					mode = weightedMedian < kShortHairLength ? Seeding::Area : Seeding::Scalp;
				}
			}
			o_result.stats.cardGuides = static_cast<uint32_t>(guides.size());
			if (mode == Seeding::Area) {
				guides.clear();
				SeedArea(o_result.scalp);
				for (const auto& guide : guides)
					ownPaths.push_back(guide.path);
			} else {
				PruneRedundant(o_result.stats);
				for (const auto& guide : guides)
					ownPaths.push_back(guide.path);
				BindToScalp(o_result.scalp, o_result.stats);
			}
			o_result.stats.seedingUsed = mode;
			LabelTriangles(o_result);

			// The card guides as bound, for tools that show or edit them (Strand::cardGuide indexes these).
			o_result.guides.resize(guides.size());
			for (uint32_t g = 0; g < guides.size(); ++g) {
				auto& out = o_result.guides[g];
				out.kind = guides[g].kind;
				out.tie = guides[g].tie;
				out.path.reserve(guides[g].path.size());
				for (const auto& sample : guides[g].path)
					out.path.push_back(sample.position);
			}

			BuildStrands(o_result.scalp, o_result);
			BuildCards(o_result);
			o_result.chains = chains;
			o_result.ties = ties;
			o_result.chainBoneBase = chainBoneBase;
			for (const auto& chain : chains)
				o_result.chainBoneCount += static_cast<uint32_t>(chain.joints.size());
			o_result.stats.wovenPieces = static_cast<uint32_t>(std::ranges::count_if(woven, [](const WovenPiece& a_piece) { return !a_piece.tris.empty(); }));
			if (o_result.strands.empty() && o_result.cardIndices.empty()) {
				o_error = "no strand could be traced";
				return false;
			}
			Shuffle(o_result);
			AssignGuides(o_result);
			FitHeadCollider(o_result);
			return true;
		}
	}

	bool Convert(const CardMesh& a_mesh, const Settings& a_settings, Result& o_result, std::string& o_error)
	{
		Generator generator(a_mesh, a_settings);
		return generator.Run(o_result, o_error);
	}

	namespace
	{
		/** A unit quaternion. */
		struct Quat
		{
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			float w = 1.0f;

			Vec3 Rotate(const Vec3& a_v) const
			{
				const Vec3 q(x, y, z);
				const Vec3 t = q.Cross(a_v) * 2.0f;
				return a_v + t * w + q.Cross(t);
			}
			/** @brief This rotation after a_o. */
			Quat operator*(const Quat& a_o) const
			{
				return { w * a_o.x + x * a_o.w + y * a_o.z - z * a_o.y, w * a_o.y - x * a_o.z + y * a_o.w + z * a_o.x,
					w * a_o.z + x * a_o.y - y * a_o.x + z * a_o.w, w * a_o.w - x * a_o.x - y * a_o.y - z * a_o.z };
			}
			/** @brief The shortest rotation taking unit vector a_from to unit vector a_to. */
			static Quat Arc(const Vec3& a_from, const Vec3& a_to)
			{
				const float d = a_from.Dot(a_to);
				if (d < -0.9999f) {
					const Vec3 axis = a_from.Cross(std::abs(a_from.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f)).Normalized();
					return { axis.x, axis.y, axis.z, 0.0f };
				}
				const Vec3 c = a_from.Cross(a_to);
				const float w = 1.0f + d;
				const float n = std::sqrt(c.LengthSquared() + w * w);
				return { c.x / n, c.y / n, c.z / n, w / n };
			}
		};

		/** @brief Carries a running turn on from one segment to the next: the shortest arc from where it points a_rest to a_now, after it. */
		Quat Transport(const Quat& a_turn, const Vec3& a_rest, const Vec3& a_now)
		{
			if (a_rest.LengthSquared() < 1e-12f || a_now.LengthSquared() < 1e-12f)
				return a_turn;
			return Quat::Arc(a_turn.Rotate(a_rest).Normalized(), a_now.Normalized()) * a_turn;
		}

		Vec3 ClosestOnSegment(const Vec3& a_p, const Vec3& a_a, const Vec3& a_b)
		{
			const Vec3 d = a_b - a_a;
			const float length = d.LengthSquared();
			return length > 1e-12f ? a_a + d * std::clamp((a_p - a_a).Dot(d) / length, 0.0f, 1.0f) : a_a;
		}
	}

	void ChainSimulator::Reset(const ChainCurve& a_chain, const Affine& a_parent)
	{
		const size_t n = a_chain.joints.size();
		target.resize(n);
		for (size_t j = 0; j < n; ++j)
			target[j] = a_parent.Apply(a_chain.joints[j]);
		position = target;
		velocity.assign(n, Vec3{});
		targetMove.assign(n, Vec3{});
		offset.assign(n, Vec3{});
		previousOffset = offset;
		moved = false;
	}

	void ChainSimulator::Translate(const Vec3& a_delta)
	{
		for (auto* points : { &position, &target })
			for (auto& p : *points)
				p += a_delta;
	}

	void ChainSimulator::Step(const ChainCurve& a_chain, const Affine& a_parent, float a_dt, const ChainSettings& a_settings, const ChainCollider* a_colliders, size_t a_colliderCount)
	{
		const auto n = static_cast<uint32_t>(a_chain.joints.size());
		if (n < 2 || !(a_dt > 0.0f))
			return;
		if (position.size() != n) {
			Reset(a_chain, a_parent);
			return;
		}
		const uint32_t pinned = std::clamp<uint32_t>(a_chain.pinnedJoints, 1, n);
		const uint32_t iterations = std::max(a_settings.iterations, 1u);
		// The settings are per 1/60 s step; the pull is spread over the passes, and TressFX halves
		// its local stiffness (one segment's pull moves both its ends) and keeps it below 0.5.
		const float steps = a_dt * 60.0f;
		const float keep = std::pow(std::clamp(1.0f - a_settings.damping, 0.0f, 1.0f), steps);
		const float pull = 0.5f * std::min(1.0f - std::pow(1.0f - std::clamp(a_settings.stiffness, 0.0f, 1.0f), steps / static_cast<float>(iterations)), 0.95f);
		const float inertia = std::clamp(a_settings.inertia, 0.0f, 1.0f);
		const float maxThrow = std::max(a_settings.maxInertia, 0.0f) * a_dt * a_dt;
		const float dftl = std::clamp(a_settings.dftlDamping, 0.0f, 1.0f);

		// Where the parent bone alone carries each joint, and each styled segment.
		std::vector<Vec3> now(n), rest(n - 1);
		for (uint32_t j = 0; j < n; ++j)
			now[j] = a_parent.Apply(a_chain.joints[j]);
		for (uint32_t j = 0; j + 1 < n; ++j)
			rest[j] = a_parent.ApplyLinear(a_chain.joints[j + 1] - a_chain.joints[j]);

		// Each joint moves with its target, plus its own velocity: gravity, and the head's
		// acceleration it does not follow (bounded, against the game's snap turns), damped. The
		// pinned joints go where the parent bone puts them.
		const Vec3 fall(0.0f, 0.0f, -a_settings.gravity * a_dt * a_dt);
		for (uint32_t j = 0; j < n; ++j) {
			const Vec3 move = now[j] - target[j];
			if (j < pinned) {
				position[j] = now[j];
				velocity[j] = {};
			} else {
				Vec3 thrown = moved ? (targetMove[j] - move) * inertia : Vec3{};
				if (const float length = thrown.Length(); length > maxThrow)
					thrown = thrown * (maxThrow / length);
				velocity[j] = velocity[j] * keep + thrown + fall;
				// Never more than a segment's length in a step: a hitch cannot fling the braid.
				const float limit = rest[j - 1].Length();
				if (const float speed = velocity[j].Length(); speed > limit)
					velocity[j] = velocity[j] * (limit / speed);
				position[j] += move + velocity[j];
			}
			targetMove[j] = move;
			target[j] = now[j];
		}
		moved = true;
		const std::vector<Vec3> integrated = position;
		const std::vector<Vec3> integratedVelocity = velocity;

		// Towards the styled shape: each segment as the one before it carries its styled
		// direction (the first, as the parent bone does), pulling both its ends.
		for (uint32_t pass = 0; pass < iterations; ++pass) {
			for (uint32_t j = 0; j + 1 < n; ++j) {
				const uint32_t c = j + 1;
				if (c < pinned)
					continue;
				Vec3 styled = rest[j];
				if (j > 0) {
					const Vec3 before = position[j] - position[j - 1];
					if (rest[j - 1].LengthSquared() > 1e-12f && before.LengthSquared() > 1e-12f)
						styled = Quat::Arc(rest[j - 1].Normalized(), before.Normalized()).Rotate(rest[j]);
				}
				const Vec3 delta = (position[j] + styled - position[c]) * pull;
				if (j >= pinned)
					position[j] -= delta;
				position[c] += delta;
			}
		}

		// Segments at their length, from the root down: only the joint further from it moves, and
		// what it moved is taken back out of the joint before (DFTL), or the chain gains energy.
		std::vector<Vec3> correction(n), contact(n);
		for (uint32_t pass = 0; pass < iterations; ++pass) {
			for (uint32_t j = 0; j + 1 < n; ++j) {
				const uint32_t c = j + 1;
				if (c < pinned)
					continue;
				const Vec3 d = position[c] - position[j];
				const float length = d.Length();
				if (length > 1e-6f) {
					const Vec3 before = position[c];
					position[c] = position[j] + d * (rest[j].Length() / length);
					correction[c] += position[c] - before;
				}
				// Out of the colliders, the braid's thickness off them, but never further out than
				// its styled place lies (a braid styled against the neck stays there), and never
				// less than half of it (the strands' simulation keeps the same margin).
				for (size_t k = 0; k < a_colliderCount; ++k) {
					const auto& collider = a_colliders[k];
					const Vec3 nearest = ClosestOnSegment(position[c], collider.a, collider.b);
					const float full = collider.radius + a_chain.radius;
					const float styled = (now[c] - ClosestOnSegment(now[c], collider.a, collider.b)).Length();
					const float clearance = std::max(std::min(full, styled), 0.5f * full);
					Vec3 away = position[c] - nearest;
					const float distance = away.Length();
					if (distance >= clearance)
						continue;
					away = distance > 1e-6f ? away / distance : Vec3(0.0f, 0.0f, 1.0f);
					position[c] = nearest + away * clearance;
					contact[c] = away;
				}
			}
		}

		// What the constraints moved becomes velocity, as Verlet's would, less what the length
		// constraints of the joint below took (DFTL). A contact only stops a joint: being pushed out
		// of a collider (an arm swinging into a braid, a body capsule rocking against it on a run)
		// adds no speed away from it, or every stride kicks the braid off the body.
		for (uint32_t j = pinned; j < n; ++j) {
			velocity[j] += position[j] - integrated[j];
			if (j + 1 < n)
				velocity[j] -= correction[j + 1] * dftl;
			if (contact[j].LengthSquared() > 0.0f) {
				const float out = velocity[j].Dot(contact[j]);
				const float allowed = std::max(integratedVelocity[j].Dot(contact[j]), 0.0f);
				if (out > allowed)
					velocity[j] -= contact[j] * (out - allowed);
			}
		}

		previousOffset = offset;
		for (uint32_t j = 0; j < n; ++j)
			offset[j] = position[j] - now[j];
	}

	void ChainSimulator::Bones(const ChainCurve& a_chain, const Affine& a_parent, float a_alpha, std::vector<Affine>& o_bones) const
	{
		const size_t n = a_chain.joints.size();
		o_bones.assign(n, a_parent);
		if (position.size() != n || n < 2)
			return;
		// The joints as drawn: carried by the parent bone as it is now, offset as simulated.
		std::vector<Vec3> drawn(n);
		for (size_t j = 0; j < n; ++j)
			drawn[j] = a_parent.Apply(a_chain.joints[j]) + Vec3::Lerp(previousOffset[j], offset[j], std::clamp(a_alpha, 0.0f, 1.0f));
		// Each joint turns with the segment leaving it (the last with the one reaching it), twisting no more than the bends need.
		Quat turn;
		for (size_t j = 0; j < n; ++j) {
			if (j + 1 < n)
				turn = Transport(turn, a_parent.ApplyLinear(a_chain.joints[j + 1] - a_chain.joints[j]), drawn[j + 1] - drawn[j]);
			auto& bone = o_bones[j];
			for (int column = 0; column < 3; ++column) {
				const Vec3 axis(column == 0 ? 1.0f : 0.0f, column == 1 ? 1.0f : 0.0f, column == 2 ? 1.0f : 0.0f);
				const Vec3 turned = turn.Rotate(a_parent.ApplyLinear(axis));
				for (int row = 0; row < 3; ++row)
					bone.rows[row][column] = turned[row];
			}
			// The bind pose's joint lands on the joint as drawn.
			const Vec3 translation = drawn[j] - bone.ApplyLinear(a_chain.joints[j]);
			for (int row = 0; row < 3; ++row)
				bone.rows[row][3] = translation[row];
		}
	}
}
