#include "StrandGenerator.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace Strands
{
	namespace
	{
		constexpr float kWeldScale = 1000.0f;        // weld positions closer than 1/1000 unit
		constexpr float kSkullCentreOffset = 5.0f;   // head bone (skull base) to skull centre, up
		constexpr float kRootEntryThreshold = 0.3f;  // how squarely flow must enter a root edge
		constexpr float kFoldThreshold = -0.2f;      // neighbour normals this opposed end a strand
		constexpr float kShortHairLength = 1.0f;     // Auto seeding: median below this is short hair
		constexpr uint32_t kMaxStepsPerStrand = 4096;
		constexpr uint32_t kMaxCrossingsPerStep = 64;  // triangles one step may cross (slivers)
		constexpr float kRootBudgetShare = 0.85f;      // of kMaxStrands, the rest left for fill strands
		constexpr float kCoverageGap = 0.5f;           // a transparent stretch longer than this ends a strand
		constexpr uint32_t kCoverageProbeGrid = 4;     // barycentric grid probing a triangle for any hair
		constexpr float kClumpAlignment = 0.5f;        // a strand joins its clump if its root runs within 60 degrees of the guide's
		constexpr float kIslandAxisRatio = 1.5f;       // Auto: a UV island flows along U only when this much longer that way
		constexpr float kIslandUVScale = 4096.0f;      // UVs closer than 1/4096 join one island

		struct Triangle
		{
			std::array<uint32_t, 3> v{};                    // source vertex indices
			std::array<uint32_t, 3> w{};                    // welded vertex ids
			std::array<int32_t, 3> neighbor{ -1, -1, -1 };  // across edge i = (v[i], v[i+1])
			float3 normal;
			float3 flow;
			float3 flowU;                // Auto: the U direction, taken where the hair runs along U
			float unitsPerU = 0.0f;      // surface length of one unit of U, and of V
			float unitsPerV = 0.0f;
			float area = 0.0f;
			uint64_t positionKey = 0;  // the same for a triangle and its back face
			int32_t component = -1;
			bool alongU = false;
			bool valid = false;
		};

		struct Seed
		{
			uint32_t tri;
			float3 position;
			float length;
			int32_t component;
		};

		struct TraceSample
		{
			float3 position;
			uint32_t tri;
			float length;
		};

		float3 Barycentric(const float3& a_p, const float3& a_a, const float3& a_b, const float3& a_c)
		{
			const float3 v0 = a_b - a_a, v1 = a_c - a_a, v2 = a_p - a_a;
			const float d00 = v0.Dot(v0), d01 = v0.Dot(v1), d11 = v1.Dot(v1);
			const float d20 = v2.Dot(v0), d21 = v2.Dot(v1);
			const float denom = d00 * d11 - d01 * d01;
			if (std::abs(denom) < 1e-12f)
				return { 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f };
			const float v = (d11 * d20 - d01 * d21) / denom;
			const float w = (d00 * d21 - d01 * d20) / denom;
			return { 1.0f - v - w, v, w };
		}

		float3 ClampBarycentric(float3 a_b)
		{
			a_b.x = std::max(a_b.x, 0.0f);
			a_b.y = std::max(a_b.y, 0.0f);
			a_b.z = std::max(a_b.z, 0.0f);
			const float sum = a_b.x + a_b.y + a_b.z;
			return sum > 1e-6f ? a_b / sum : float3{ 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f };
		}

		float Component(const float3& a_v, int a_i)
		{
			return a_i == 0 ? a_v.x : (a_i == 1 ? a_v.y : a_v.z);
		}

		float Hash01(uint32_t a_x)
		{
			a_x ^= a_x >> 16;
			a_x *= 0x7FEB352Du;
			a_x ^= a_x >> 15;
			a_x *= 0x846CA68Bu;
			a_x ^= a_x >> 16;
			return (a_x >> 8) * (1.0f / 16777216.0f);
		}

		class Generator
		{
		public:
			Generator(const HairMeshData& a_mesh, const StrandStyle& a_style) :
				mesh(a_mesh), style(a_style), rng(a_style.seed * 0x9E3779B9u + static_cast<uint32_t>(a_mesh.positions.size())),
				useCoverage(!a_mesh.coverage.Empty() && a_style.coverageThreshold > 0.0f)
			{}

			bool Run(StrandAssetData& o_asset, std::string& o_error);

		private:
			float Random() { return uniform(rng); }

			void Weld();
			void BuildTriangles();
			void BuildAdjacency();
			void BuildComponents();
			void DropBackFaces();
			void ChooseFlowAxis();
			float3 FindHeadCentre() const;
			void OrientFlow();
			void BuildVertexFields();

			float3 FlowAt(const Triangle& a_tri, const float3& a_bary) const;
			float3 Position(uint32_t a_vertex) const { return mesh.positions[a_vertex]; }

			/**
			 * Follows the flow (sign +1) or against it (-1) from a point on a triangle. Calls
			 * a_onSample(position, triangle, length) at the start, after every step and at the
			 * end. Returns the length travelled; o_end* receive where it stopped.
			 */
			template <class F>
			float Trace(uint32_t a_tri, float3 a_pos, float a_sign, float a_maxLength, F&& a_onSample, uint32_t* o_endTri = nullptr, float3* o_endPos = nullptr) const;

			void SeedRoots(std::vector<Seed>& o_seeds);
			void SeedFill(std::vector<Seed>& o_seeds);
			void SeedArea(std::vector<Seed>& o_seeds);
			/**
			 * Traces a strand from a seed and keeps it if long enough. With a coverage mask the
			 * strand is cut to the painted part of its streamline; a_findCoverage lets that part
			 * start downstream of the seed (roots on a transparent card edge), otherwise the seed
			 * itself must be painted.
			 */
			bool TryAddSeed(uint32_t a_tri, const float3& a_pos, float a_maxLength, std::vector<Seed>& o_seeds, bool a_countVisits, bool a_findCoverage);
			bool TrimToCoverage(bool a_findCoverage, Seed& io_seed) const;
			float Coverage(uint32_t a_tri, const float3& a_pos) const;
			bool Covered(uint32_t a_tri, const float3& a_pos) const { return !useCoverage || Coverage(a_tri, a_pos) >= style.coverageThreshold; }
			bool HasCoverage(uint32_t a_tri) const;

			RestPoint MakePoint(uint32_t a_tri, const float3& a_pos, float a_t) const;
			float3 LiftNormal(uint32_t a_tri, const float3& a_pos) const;
			void BuildStrands(const std::vector<Seed>& a_seeds, StrandAssetData& o_asset);
			void ApplyClumping(StrandAssetData& o_asset, const std::vector<Seed>& a_seeds) const;
			void Shuffle(StrandAssetData& o_asset);

			const HairMeshData& mesh;
			const StrandStyle& style;
			std::mt19937 rng;
			std::uniform_real_distribution<float> uniform{ 0.0f, 1.0f };
			const bool useCoverage;

			std::vector<uint32_t> weldId;      // per source vertex
			std::vector<uint32_t> positionId;  // per source vertex, ignoring normals
			uint32_t weldedCount = 0;
			std::vector<Triangle> tris;
			std::vector<float3> weldedFlow;
			std::vector<float3> weldedNormal;   // geometric, area weighted
			std::vector<float3> shadingNormal;  // per source vertex: authored, else geometric
			float3 headCentre;
			float step = 0.5f;

			std::vector<uint16_t> visits;      // strands through each triangle
			std::vector<uint32_t> visitStamp;  // last strand counted per triangle
			uint32_t currentStamp = 0;
			std::vector<uint32_t> crossed;
			std::vector<TraceSample> samples;  // the last traced streamline
			mutable std::vector<uint32_t> traceStamp;  // last trace that entered each triangle
			mutable uint32_t traceId = 0;
			uint32_t strandBudget = GeneratorLimits::kMaxStrands;
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
			std::unordered_set<uint64_t> seen;
			seen.reserve(triCount);

			for (uint32_t t = 0; t < triCount; ++t) {
				auto& tri = tris[t];
				for (int i = 0; i < 3; ++i) {
					tri.v[i] = mesh.indices[t * 3 + i];
					tri.w[i] = weldId[tri.v[i]];
				}
				const float3 a = Position(tri.v[0]), b = Position(tri.v[1]), c = Position(tri.v[2]);
				float3 n = (b - a).Cross(c - a);
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
				if (!seen.insert(tripleKey(tri.w)).second)
					continue;
				tri.positionKey = tripleKey({ positionId[tri.v[0]], positionId[tri.v[1]], positionId[tri.v[2]] });

				const auto& ua = mesh.uvs[tri.v[0]];
				const auto& ub = mesh.uvs[tri.v[1]];
				const auto& uc = mesh.uvs[tri.v[2]];
				const float2 uvCentre = (ua + ub + uc) / 3.0f;
				if (std::ranges::any_of(style.excludeUV, [&](const UVRect& r) { return r.Contains(uvCentre.x, uvCentre.y); }))
					continue;

				// Solve dP = dPdu du + dPdv dv over the triangle's two edges.
				const float3 e1 = b - a, e2 = c - a;
				const float du1 = ub.x - ua.x, dv1 = ub.y - ua.y, du2 = uc.x - ua.x, dv2 = uc.y - ua.y;
				const float det = du1 * dv2 - du2 * dv1;
				if (std::abs(det) < 1e-10f)
					continue;
				const float3 dPdu = (e1 * dv2 - e2 * dv1) / det;
				const float3 dPdv = (e2 * du1 - e1 * du2) / det;
				float3 flow;
				switch (style.flowAxis) {
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
				tri.unitsPerU = dPdu.Length();
				tri.unitsPerV = dPdv.Length();
				tri.flowU = tri.unitsPerU > 1e-6f ? dPdu / tri.unitsPerU : tri.flow;
				tri.valid = true;
			}
		}

		void Generator::ChooseFlowAxis()
		{
			if (style.flowAxis != FlowAxis::Auto)
				return;

			// Atlases lay most strips of hair along V, but not all: vanilla's long strip runs along
			// U. Each UV island is one strip, and follows whichever axis it is clearly longer along
			// on the surface. Vertices sharing a position and a UV are one island vertex.
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
				float area = 0.0f;
				float unitsPerU = 0.0f;  // area weighted sums
				float unitsPerV = 0.0f;
				float uMin = FLT_MAX, uMax = -FLT_MAX, vMin = FLT_MAX, vMax = -FLT_MAX;
			};
			std::unordered_map<uint32_t, Island> islands;
			for (const auto& tri : tris) {
				if (!tri.valid)
					continue;
				auto& island = islands[find(tri.v[0])];
				island.area += tri.area;
				island.unitsPerU += tri.unitsPerU * tri.area;
				island.unitsPerV += tri.unitsPerV * tri.area;
				for (uint32_t v : tri.v) {
					island.uMin = std::min(island.uMin, mesh.uvs[v].x);
					island.uMax = std::max(island.uMax, mesh.uvs[v].x);
					island.vMin = std::min(island.vMin, mesh.uvs[v].y);
					island.vMax = std::max(island.vMax, mesh.uvs[v].y);
				}
			}
			for (auto& tri : tris) {
				if (!tri.valid)
					continue;
				const auto& island = islands[find(tri.v[0])];
				const float lengthU = (island.uMax - island.uMin) * island.unitsPerU;
				const float lengthV = (island.vMax - island.vMin) * island.unitsPerV;
				tri.alongU = lengthU > kIslandAxisRatio * lengthV;
				if (tri.alongU)
					tri.flow = tri.flowU;
			}
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

		float3 Generator::FindHeadCentre() const
		{
			for (size_t b = 0; b < mesh.boneNames.size(); ++b) {
				std::string name = mesh.boneNames[b];
				std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (name.find("npc head") != std::string::npos)
					return mesh.boneBindPositions[b] + float3(0.0f, 0.0f, kSkullCentreOffset);
			}
			// No head bone (wigs on odd skeletons): a point just under the top of the mesh.
			float3 lo(FLT_MAX), hi(-FLT_MAX);
			for (const auto& p : mesh.positions) {
				lo = float3::Min(lo, p);
				hi = float3::Max(hi, p);
			}
			return { 0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), hi.z - 8.0f };
		}

		void Generator::OrientFlow()
		{
			headCentre = FindHeadCentre();
			if (style.flowAxis != FlowAxis::Auto)
				return;

			// Hair flows away from the head and, on balance, downwards: orient each connected
			// piece so its flow agrees with both, weighted by area. Strips along U and along V
			// are laid out independently, so each axis within a piece is oriented on its own.
			const auto group = [](const Triangle& a_tri) { return static_cast<int64_t>(a_tri.component) * 2 + (a_tri.alongU ? 1 : 0); };
			std::unordered_map<int64_t, float> score;
			for (const auto& tri : tris) {
				if (!tri.valid)
					continue;
				const float3 centre = (Position(tri.v[0]) + Position(tri.v[1]) + Position(tri.v[2])) / 3.0f;
				float3 radial = centre - headCentre;
				radial.Normalize();
				score[group(tri)] += tri.area * (tri.flow.Dot(radial) - 0.5f * tri.flow.z);
			}
			for (auto& tri : tris) {
				if (tri.valid && score[group(tri)] < 0.0f)
					tri.flow = -tri.flow;
			}
		}

		void Generator::BuildVertexFields()
		{
			weldedFlow.assign(weldedCount, float3::Zero);
			weldedNormal.assign(weldedCount, float3::Zero);
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
				shadingNormal[v] = mesh.normals.empty() ? weldedNormal[weldId[v]] : mesh.normals[v];
				if (shadingNormal[v].LengthSquared() < 1e-8f)
					shadingNormal[v] = float3::UnitZ;
			}
		}

		float3 Generator::FlowAt(const Triangle& a_tri, const float3& a_bary) const
		{
			float3 flow = weldedFlow[a_tri.w[0]] * a_bary.x + weldedFlow[a_tri.w[1]] * a_bary.y + weldedFlow[a_tri.w[2]] * a_bary.z;
			flow -= a_tri.normal * flow.Dot(a_tri.normal);
			// Where neighbouring flows disagree the average cancels; fall back to the face.
			if (flow.LengthSquared() < 1e-8f || flow.Dot(a_tri.flow) < 0.0f)
				return a_tri.flow;
			flow.Normalize();
			return flow;
		}

		template <class F>
		float Generator::Trace(uint32_t a_tri, float3 a_pos, float a_sign, float a_maxLength, F&& a_onSample, uint32_t* o_endTri, float3* o_endPos) const
		{
			uint32_t tri = a_tri;
			float3 pos = a_pos;
			float length = 0.0f;
			float3 previousDir = float3::Zero;
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
					const float3 a = Position(t.v[0]), b = Position(t.v[1]), c = Position(t.v[2]);
					const float3 bp = ClampBarycentric(Barycentric(pos, a, b, c));
					const float3 dir = FlowAt(t, bp) * a_sign;
					if (previousDir.LengthSquared() > 0.0f && dir.Dot(previousDir) < -0.2f) {
						stop = true;  // the flow turned back on itself
						break;
					}
					const float3 target = pos + dir * remaining;
					const float3 bq = Barycentric(target, a, b, c);
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
						const float from = Component(bp, i), to = Component(bq, i);
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

		float Generator::Coverage(uint32_t a_tri, const float3& a_pos) const
		{
			const auto& tri = tris[a_tri];
			const float3 bary = ClampBarycentric(Barycentric(a_pos, Position(tri.v[0]), Position(tri.v[1]), Position(tri.v[2])));
			const float2 uv = mesh.uvs[tri.v[0]] * bary.x + mesh.uvs[tri.v[1]] * bary.y + mesh.uvs[tri.v[2]] * bary.z;
			return mesh.coverage.Sample(uv.x, uv.y);
		}

		bool Generator::HasCoverage(uint32_t a_tri) const
		{
			if (!useCoverage)
				return true;
			const auto& tri = tris[a_tri];
			const float3 a = Position(tri.v[0]), b = Position(tri.v[1]), c = Position(tri.v[2]);
			for (uint32_t i = 0; i <= kCoverageProbeGrid; ++i) {
				for (uint32_t j = 0; i + j <= kCoverageProbeGrid; ++j) {
					const float u = static_cast<float>(i) / kCoverageProbeGrid, v = static_cast<float>(j) / kCoverageProbeGrid;
					if (Covered(a_tri, a * (1.0f - u - v) + b * u + c * v))
						return true;
				}
			}
			return false;
		}

		bool Generator::TrimToCoverage(bool a_findCoverage, Seed& io_seed) const
		{
			// From the first painted sample to the last one before a transparent gap longer
			// than kCoverageGap: where the card's texture ends, the strand ends.
			size_t first = 0;
			if (a_findCoverage) {
				while (first < samples.size() && !Covered(samples[first].tri, samples[first].position))
					++first;
			} else if (samples.empty() || !Covered(samples[0].tri, samples[0].position)) {
				return false;
			}
			if (first >= samples.size())
				return false;
			size_t last = first;
			for (size_t i = first + 1; i < samples.size(); ++i) {
				if (Covered(samples[i].tri, samples[i].position))
					last = i;
				else if (samples[i].length - samples[last].length > kCoverageGap)
					break;
			}
			io_seed.tri = samples[first].tri;
			io_seed.position = samples[first].position;
			io_seed.length = samples[last].length - samples[first].length;
			io_seed.component = tris[io_seed.tri].component;
			return true;
		}

		bool Generator::TryAddSeed(uint32_t a_tri, const float3& a_pos, float a_maxLength, std::vector<Seed>& o_seeds, bool a_countVisits, bool a_findCoverage)
		{
			if (o_seeds.size() >= strandBudget)
				return false;
			// Each triangle the strand crosses, once.
			++currentStamp;
			crossed.clear();
			samples.clear();
			const float length = Trace(a_tri, a_pos, 1.0f, a_maxLength, [&](const float3& a_p, uint32_t a_t, float a_length) {
				if (visitStamp[a_t] != currentStamp) {
					visitStamp[a_t] = currentStamp;
					crossed.push_back(a_t);
				}
				samples.push_back({ a_p, a_t, a_length });
			});
			Seed seed{ a_tri, a_pos, length, tris[a_tri].component };
			const bool keep = (!useCoverage || TrimToCoverage(a_findCoverage, seed)) && seed.length >= GeneratorLimits::kMinStrandLength;
			// With a mask the whole streamline counts as visited, painted or not: its
			// transparent stretches need no fill strands either.
			if (a_countVisits && (keep || useCoverage)) {
				for (uint32_t t : crossed)
					visits[t] = static_cast<uint16_t>(std::min<uint32_t>(visits[t] + 1u, UINT16_MAX));
			}
			if (!keep)
				return false;
			o_seeds.push_back(seed);
			return true;
		}

		void Generator::SeedRoots(std::vector<Seed>& o_seeds)
		{
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
					const float3 a = Position(tri.v[i]), b = Position(tri.v[(i + 1) % 3]);
					const float3 edge = b - a;
					const float edgeLength = edge.Length();
					if (edgeLength < 1e-5f)
						continue;
					float3 outward = edge.Cross(tri.normal);
					outward.Normalize();
					float mid[3]{};
					mid[i] = 0.5f;
					mid[(i + 1) % 3] = 0.5f;
					const float entry = -FlowAt(tri, float3(mid[0], mid[1], mid[2])).Dot(outward);
					if (entry < kRootEntryThreshold)
						continue;
					// Strands per unit of width across the flow.
					const float expected = style.density * edgeLength * entry;
					roots.push_back({ t, i, expected });
					total += expected;
				}
			}

			const float budget = GeneratorLimits::kMaxStrands * kRootBudgetShare;
			const float acceptance = total > budget ? budget / total : 1.0f;
			for (const auto& root : roots) {
				const auto& tri = tris[root.tri];
				const uint32_t count = static_cast<uint32_t>(root.expected * acceptance + Random());
				const float3 a = Position(tri.v[root.edge]), b = Position(tri.v[(root.edge + 1) % 3]);
				const float3 centre = (Position(tri.v[0]) + Position(tri.v[1]) + Position(tri.v[2])) / 3.0f;
				for (uint32_t j = 0; j < count; ++j) {
					const float s = (j + Random()) / count;
					// Just inside the edge, so the first step starts in this triangle.
					const float3 p = float3::Lerp(float3::Lerp(a, b, s), centre, 0.002f);
					TryAddSeed(root.tri, p, GeneratorLimits::kMaxStrandLength, o_seeds, true, true);
				}
			}
		}

		void Generator::SeedFill(std::vector<Seed>& o_seeds)
		{
			// Streamlines through triangles the roots missed (cards whose upstream edge is
			// shared with the scalp cap, sheets with no clean top edge): walk back to where
			// the flow starts, then keep the whole streamline.
			std::vector<uint32_t> order(tris.size());
			std::iota(order.begin(), order.end(), 0u);
			std::shuffle(order.begin(), order.end(), rng);
			for (uint32_t t : order) {
				const auto& tri = tris[t];
				if (!tri.valid || !HasCoverage(t))
					continue;
				const auto target = static_cast<uint32_t>(std::max(1.0f, std::round(style.density * std::sqrt(tri.area) * 0.5f)));
				for (uint32_t attempt = 0; visits[t] < target && attempt < target * 2; ++attempt) {
					float r1 = std::sqrt(Random()), r2 = Random();
					const float3 p = Position(tri.v[0]) * (1.0f - r1) + Position(tri.v[1]) * (r1 * (1.0f - r2)) + Position(tri.v[2]) * (r1 * r2);
					uint32_t rootTri = t;
					float3 rootPos = p;
					Trace(t, p, -1.0f, GeneratorLimits::kMaxStrandLength, [](const float3&, uint32_t, float) {}, &rootTri, &rootPos);
					if (!TryAddSeed(rootTri, rootPos, GeneratorLimits::kMaxStrandLength, o_seeds, true, true) && o_seeds.size() >= strandBudget)
						return;
				}
			}
		}

		void Generator::SeedArea(std::vector<Seed>& o_seeds)
		{
			float totalArea = 0.0f;
			for (const auto& tri : tris)
				totalArea += tri.valid ? tri.area : 0.0f;
			const float perArea = style.density * 4.0f;
			const float acceptance = totalArea * perArea > strandBudget ? strandBudget / (totalArea * perArea) : 1.0f;
			for (uint32_t t = 0; t < tris.size(); ++t) {
				const auto& tri = tris[t];
				if (!tri.valid)
					continue;
				const auto count = static_cast<uint32_t>(tri.area * perArea * acceptance + Random());
				for (uint32_t j = 0; j < count; ++j) {
					float r1 = std::sqrt(Random()), r2 = Random();
					const float3 p = Position(tri.v[0]) * (1.0f - r1) + Position(tri.v[1]) * (r1 * (1.0f - r2)) + Position(tri.v[2]) * (r1 * r2);
					if (!TryAddSeed(t, p, style.shortLength, o_seeds, false, false) && o_seeds.size() >= strandBudget)
						return;
				}
			}
		}

		float3 Generator::LiftNormal(uint32_t a_tri, const float3& a_pos) const
		{
			// Always away from the head, whichever way the card's winding faces.
			float3 n = tris[a_tri].normal;
			if (n.Dot(a_pos - headCentre) < 0.0f)
				n = -n;
			return n;
		}

		RestPoint Generator::MakePoint(uint32_t a_tri, const float3& a_pos, float a_t) const
		{
			const auto& tri = tris[a_tri];
			const float3 bary = ClampBarycentric(Barycentric(a_pos, Position(tri.v[0]), Position(tri.v[1]), Position(tri.v[2])));
			const float weights[3] = { bary.x, bary.y, bary.z };

			RestPoint point{};
			point.position = a_pos;
			point.t = a_t;

			float3 normal = float3::Zero;
			float2 uv = float2::Zero;
			std::array<std::pair<uint16_t, float>, 12> influences{};
			size_t influenceCount = 0;
			for (int i = 0; i < 3; ++i) {
				const uint32_t v = tri.v[i];
				normal += shadingNormal[v] * weights[i];
				uv += mesh.uvs[v] * weights[i];
				for (int j = 0; j < 4; ++j) {
					const float w = mesh.boneWeights[v][j] * weights[i];
					if (w <= 0.0f)
						continue;
					const uint16_t bone = mesh.boneIndices[v][j];
					auto it = std::find_if(influences.begin(), influences.begin() + influenceCount, [&](const auto& e) { return e.first == bone; });
					if (it != influences.begin() + influenceCount)
						it->second += w;
					else
						influences[influenceCount++] = { bone, w };
				}
			}
			if (normal.Dot(a_pos - headCentre) < 0.0f && tris[a_tri].normal.Dot(normal) < 0.0f)
				normal = -normal;  // back side of a double-sided card: shade the outside
			normal.Normalize();
			point.normal = normal;
			point.u = uv.x;
			point.v = uv.y;

			// Keep the four strongest bones, renormalised into unorm8 weights summing to 255.
			std::sort(influences.begin(), influences.begin() + influenceCount, [](const auto& l, const auto& r) { return l.second > r.second; });
			influenceCount = std::min<size_t>(influenceCount, 4);
			float total = 0.0f;
			for (size_t i = 0; i < influenceCount; ++i)
				total += influences[i].second;
			std::array<uint16_t, 4> bones{};
			std::array<uint32_t, 4> packed{};
			uint32_t packedTotal = 0;
			for (size_t i = 0; i < influenceCount; ++i) {
				bones[i] = influences[i].first;
				packed[i] = static_cast<uint32_t>(std::lround(influences[i].second / total * 255.0f));
				packedTotal += packed[i];
			}
			if (influenceCount == 0)
				packed[0] = 255;  // unweighted vertex: follow bone 0 rather than vanish
			else
				packed[0] = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(packed[0]) + 255 - static_cast<int>(packedTotal), 0, 255));
			point.bones01 = bones[0] | (static_cast<uint32_t>(bones[1]) << 16);
			point.bones23 = bones[2] | (static_cast<uint32_t>(bones[3]) << 16);
			point.weights = packed[0] | (packed[1] << 8) | (packed[2] << 16) | (packed[3] << 24);
			return point;
		}

		void Generator::BuildStrands(const std::vector<Seed>& a_seeds, StrandAssetData& o_asset)
		{
			// One point count for the whole asset, enough for all but the longest strands.
			std::vector<float> lengths;
			lengths.reserve(a_seeds.size());
			for (const auto& seed : a_seeds)
				lengths.push_back(seed.length * style.lengthScale);
			std::vector<float> sorted = lengths;
			const size_t p95 = sorted.size() * 95 / 100;
			std::nth_element(sorted.begin(), sorted.begin() + std::min(p95, sorted.size() - 1), sorted.end());
			const float longLength = sorted[std::min(p95, sorted.size() - 1)];
			const uint32_t points = std::clamp(static_cast<uint32_t>(std::ceil(longLength / style.segmentLength)) + 1, GeneratorLimits::kMinPointsPerStrand, GeneratorLimits::kMaxPointsPerStrand);

			o_asset.pointsPerStrand = points;
			o_asset.points.resize(a_seeds.size() * points);
			o_asset.strands.resize(a_seeds.size());

			double lengthSum = 0.0;
			std::vector<uint32_t> pointTri;
			for (size_t s = 0; s < a_seeds.size(); ++s) {
				const auto& seed = a_seeds[s];
				const float length = lengths[s];
				RestPoint* out = &o_asset.points[s * points];

				// Resample the traced polyline at even arclength.
				uint32_t k = 0;
				pointTri.assign(points, seed.tri);
				float3 lastPos = seed.position;
				uint32_t lastTri = seed.tri;
				float lastLength = 0.0f;
				Trace(seed.tri, seed.position, 1.0f, length, [&](const float3& a_pos, uint32_t a_tri, float a_length) {
					while (k < points) {
						const float target = length * k / (points - 1);
						if (target > a_length + 1e-5f)
							break;
						const float span = a_length - lastLength;
						const float alpha = span > 1e-6f ? std::clamp((target - lastLength) / span, 0.0f, 1.0f) : 1.0f;
						const float3 p = float3::Lerp(lastPos, a_pos, alpha);
						pointTri[k] = alpha < 0.5f ? lastTri : a_tri;
						out[k] = MakePoint(pointTri[k], p, static_cast<float>(k) / (points - 1));
						++k;
					}
					lastPos = a_pos;
					lastTri = a_tri;
					lastLength = a_length;
				});
				for (; k < points; ++k) {
					pointTri[k] = lastTri;
					out[k] = MakePoint(lastTri, lastPos, static_cast<float>(k) / (points - 1));
				}

				// Depth and volume: lift off the card, a random layer at the root growing towards the tip.
				const float layer = Random() * style.layerJitter;
				for (uint32_t i = 0; i < points; ++i) {
					const float t = out[i].t;
					const float lift = layer + style.volume * t * t * (3.0f - 2.0f * t);
					out[i].position += LiftNormal(pointTri[i], out[i].position) * lift;
				}

				o_asset.strands[s] = { length, Random(), 0u, 0.0f };
				lengthSum += length;
			}
			o_asset.averageLength = a_seeds.empty() ? 0.0f : static_cast<float>(lengthSum / a_seeds.size());
		}

		void Generator::ApplyClumping(StrandAssetData& o_asset, const std::vector<Seed>& a_seeds) const
		{
			const uint32_t points = o_asset.pointsPerStrand;
			const float cell = style.clumpSize * 2.0f;

			// Clumps: strands of one connected piece whose roots share a grid cell.
			std::unordered_map<uint64_t, uint32_t> clumpIds;
			std::vector<std::vector<uint32_t>> clumps;
			for (uint32_t s = 0; s < o_asset.StrandCount(); ++s) {
				const float3& root = o_asset.points[s * points].position;
				const auto cx = static_cast<int64_t>(std::floor(root.x / cell)) & 0xFFFF;
				const auto cy = static_cast<int64_t>(std::floor(root.y / cell)) & 0xFFFF;
				const auto cz = static_cast<int64_t>(std::floor(root.z / cell)) & 0xFFFF;
				const uint64_t key = static_cast<uint64_t>(cx) | (static_cast<uint64_t>(cy) << 16) | (static_cast<uint64_t>(cz) << 32) | (static_cast<uint64_t>(a_seeds[s].component & 0xFFFF) << 48);
				auto [it, inserted] = clumpIds.try_emplace(key, static_cast<uint32_t>(clumps.size()));
				if (inserted)
					clumps.emplace_back();
				clumps[it->second].push_back(s);
				o_asset.strands[s].clump = it->second;
				o_asset.strands[s].clumpRandom = Hash01(it->second * 2654435761u + style.seed);
			}

			if (style.clumpStrength <= 0.0f && style.clumpTwist == 0.0f)
				return;

			// Each clump gathers round its longest strand, compared at equal distance from the
			// root. An average of point k across members would not do: point k sits at a
			// different distance on strands of different length, and that average (like any
			// average of strands on a curved scalp) lies inside the head.
			const float exponent = 0.35f + 0.65f * (1.0f - style.clumpStrength);
			std::vector<float3> guide(points);
			const auto rootDirection = [&](uint32_t a_strand) {
				float3 d = o_asset.points[a_strand * points + 1].position - o_asset.points[a_strand * points].position;
				d.Normalize();
				return d;
			};
			for (const auto& members : clumps) {
				if (members.size() < 2)
					continue;
				const uint32_t guideStrand = *std::ranges::max_element(members, [&](uint32_t a, uint32_t b) { return o_asset.strands[a].length < o_asset.strands[b].length; });
				for (uint32_t k = 0; k < points; ++k)
					guide[k] = o_asset.points[guideStrand * points + k].position;
				const float guideLength = std::max(o_asset.strands[guideStrand].length, 1e-4f);
				const float3 guideDirection = rootDirection(guideStrand);
				const auto guideAt = [&](float a_distance, float3& o_axis) {
					const float x = std::clamp(a_distance / guideLength, 0.0f, 1.0f) * (points - 1);
					const uint32_t i = std::min(static_cast<uint32_t>(x), points - 2);
					o_axis = guide[i + 1] - guide[i];
					return float3::Lerp(guide[i], guide[i + 1], x - i);
				};

				for (uint32_t s : members) {
					// Strands leaving the cell another way (a parting, a crown whorl) stay apart.
					if (s == guideStrand || rootDirection(s).Dot(guideDirection) < kClumpAlignment)
						continue;
					const float length = o_asset.strands[s].length;
					for (uint32_t k = 0; k < points; ++k) {
						auto& point = o_asset.points[s * points + k];
						float3 axis;
						const float3 centre = guideAt(point.t * length, axis);
						float3 rel = point.position - centre;
						if (style.clumpTwist != 0.0f && axis.LengthSquared() > 1e-8f) {
							axis.Normalize();
							const float angle = DirectX::XM_2PI * style.clumpTwist * point.t * length;
							const float c = std::cos(angle), sn = std::sin(angle);
							rel = rel * c + axis.Cross(rel) * sn + axis * axis.Dot(rel) * (1.0f - c);
						}
						const float pull = style.clumpStrength * std::pow(point.t, exponent);
						point.position = centre + rel * (1.0f - pull);
					}
				}
			}
		}

		void Generator::Shuffle(StrandAssetData& o_asset)
		{
			const uint32_t points = o_asset.pointsPerStrand;
			std::vector<uint32_t> order(o_asset.StrandCount());
			std::iota(order.begin(), order.end(), 0u);
			std::shuffle(order.begin(), order.end(), rng);
			std::vector<RestPoint> shuffledPoints(o_asset.points.size());
			std::vector<StrandInfo> shuffledStrands(o_asset.strands.size());
			for (uint32_t i = 0; i < order.size(); ++i) {
				shuffledStrands[i] = o_asset.strands[order[i]];
				std::copy_n(&o_asset.points[order[i] * points], points, &shuffledPoints[i * points]);
			}
			o_asset.points = std::move(shuffledPoints);
			o_asset.strands = std::move(shuffledStrands);
		}

		bool Generator::Run(StrandAssetData& o_asset, std::string& o_error)
		{
			o_asset = {};
			o_asset.totalTriangles = mesh.TriangleCount();
			if (mesh.positions.empty() || mesh.indices.empty()) {
				o_error = "empty mesh";
				return false;
			}
			step = std::clamp(style.segmentLength * 0.5f, 0.2f, 1.0f);

			Weld();
			BuildTriangles();
			BuildAdjacency();
			BuildComponents();
			DropBackFaces();
			ChooseFlowAxis();
			OrientFlow();
			BuildVertexFields();

			o_asset.convertedTriangles = static_cast<uint32_t>(std::ranges::count_if(tris, [](const Triangle& t) { return t.valid; }));
			if (o_asset.convertedTriangles == 0) {
				o_error = "no triangle has a usable texture flow";
				return false;
			}

			visits.assign(tris.size(), 0);
			visitStamp.assign(tris.size(), 0);
			traceStamp.assign(tris.size(), 0);

			std::vector<Seed> seeds;
			SeedMode mode = style.seeding;
			if (mode != SeedMode::Area) {
				SeedRoots(seeds);
				SeedFill(seeds);
				if (mode == SeedMode::Auto) {
					std::vector<float> lengths;
					for (const auto& s : seeds)
						lengths.push_back(s.length);
					bool shortHair = lengths.empty();
					if (!shortHair) {
						std::nth_element(lengths.begin(), lengths.begin() + lengths.size() / 2, lengths.end());
						shortHair = lengths[lengths.size() / 2] < kShortHairLength;
					}
					mode = shortHair ? SeedMode::Area : SeedMode::Roots;
					if (shortHair)
						seeds.clear();
				}
			}
			if (mode == SeedMode::Area)
				SeedArea(seeds);
			o_asset.seedingUsed = mode;

			if (seeds.empty()) {
				o_error = "no strand could be traced";
				return false;
			}

			BuildStrands(seeds, o_asset);
			ApplyClumping(o_asset, seeds);
			Shuffle(o_asset);
			return true;
		}
	}

	bool GenerateStrands(const HairMeshData& a_mesh, const StrandStyle& a_style, StrandAssetData& o_asset, std::string& o_error)
	{
		Generator generator(a_mesh, a_style);
		return generator.Run(o_asset, o_error);
	}
}
