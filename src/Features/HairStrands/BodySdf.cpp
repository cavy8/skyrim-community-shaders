#include "BodySdf.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <numeric>
#include <unordered_set>

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

namespace Strands
{
	namespace
	{
		// The field (BodySdf.cs.hlsl).
		constexpr float kCellSize = 1.25f;           // units between cells, at actor scale 1
		constexpr float kOutsideBand = 2.0f;         // cells the field reaches in front of a surface
		constexpr float kMaxInsideBand = 6.0f;       // cells it reaches behind one, where the mesh is that thick
		constexpr float kInsideCos = -0.25f;         // a cell nearer level with the surface than this is outside
		constexpr float kInsideBandCos = -0.7f;      // the inside band takes cells within about 45 degrees of straight behind
		constexpr float kMaxMotion = 64.0f;          // units: a surface moving further in a frame jumped
		constexpr float kMaxTriangleExtent = 32.0f;  // cells: a triangle's box this large is a broken skin
		constexpr uint32_t kInsideBandSteps = 32;    // steps of a triangle's inside band byte per cell
		constexpr uint32_t kMaxGridAxis = 128;
		constexpr uint32_t kMaxGridCells = 512 * 1024;  // 10 MB of textures
		constexpr uint32_t kMaxCoarsening = 8;          // the cells grow (x1.25) at most this often to fit
		// The collision mesh, in skin units (about world units).
		constexpr float kClusterSize = 1.25f;       // vertices within a cell this size are merged
		constexpr uint32_t kMaxClusterRetries = 3;  // each 1.5x coarser, while over the triangle budget
		constexpr uint32_t kMaxTriangles = 65536;
		constexpr float kMaxEdge = 2.0f;  // cluster cells: longer triangle edges are split
		constexpr uint32_t kMaxSplitDepth = 5;
		constexpr float kThinSheet = 0.6f;      // opposite faces closer than this are one sheet
		constexpr float kHeadWeight = 0.5f;     // a vertex this much on the head (or bones under it) is the head field's
		constexpr float kRayCell = 2.0f;        // cells of the grid the thickness rays walk
		constexpr float kThicknessTilt = 0.7f;  // radians: the tilt of four of the five thickness rays
		constexpr float kRayTolerance = 0.02f;  // barycentric slack, so rays do not slip between triangles
		constexpr uint32_t kMaxRayCells = 1u << 21;
		// Rebuilds.
		constexpr uint32_t kStableFrames = 10;  // what is worn must hold still this long before a rebuild
		constexpr uint32_t kEvictFrames = 300;  // an actor unseen this long is forgotten
		constexpr int kMaxSceneDepth = 64;
		constexpr uint32_t kProbeVertices = 8;  // vertices hashed per mesh: body morphs move them
		constexpr float kAbsent = 1e7f;         // units: where the vertices of a source not drawn this frame go

		using EShaderPropertyFlag = RE::BSShaderProperty::EShaderPropertyFlag;

		uint32_t RenderFrame()
		{
			return globals::state->frameCount;
		}

		float3 ToFloat3(const RE::NiPoint3& a_point)
		{
			return { a_point.x, a_point.y, a_point.z };
		}

		using Rows3 = std::array<float4, 3>;

		// A transform's 3x4 rows: rotation times scale, then translation.
		Rows3 Rows(const RE::NiTransform& a_transform)
		{
			Rows3 rows;
			for (int r = 0; r < 3; ++r) {
				rows[r] = { a_transform.rotate.entry[r][0] * a_transform.scale, a_transform.rotate.entry[r][1] * a_transform.scale,
					a_transform.rotate.entry[r][2] * a_transform.scale, a_transform.translate[r] };
			}
			return rows;
		}

		float3 Apply(const Rows3& a_rows, const float3& a_p)
		{
			const float4 p(a_p.x, a_p.y, a_p.z, 1.0f);
			return { a_rows[0].Dot(p), a_rows[1].Dot(p), a_rows[2].Dot(p) };
		}

		// Skin to world for a skin instance's bone, as the game skins it: bone world x skin to bone.
		RE::NiTransform SkinToWorld(const RE::NiSkinInstance* a_skin, uint32_t a_bone)
		{
			const RE::NiTransform* world = a_skin->boneWorldTransforms ? a_skin->boneWorldTransforms[a_bone] : nullptr;
			if (!world && a_skin->bones && a_skin->bones[a_bone])
				world = &a_skin->bones[a_bone]->world;
			const auto& skinToBone = a_skin->skinData->GetBoneDataSkinToBone(a_bone);
			return world ? (*world) * skinToBone : skinToBone;
		}

		bool IsUnder(const RE::NiAVObject* a_node, const RE::NiAVObject* a_ancestor)
		{
			for (int depth = 0; a_node && a_ancestor && depth < kMaxSceneDepth; ++depth, a_node = a_node->parent) {
				if (a_node == a_ancestor)
					return true;
			}
			return false;
		}

		struct Fnv  // FNV-1a
		{
			uint64_t hash = 14695981039346656037ull;

			void Mix(uint64_t a_value)
			{
				for (int b = 0; b < 8; ++b)
					hash = (hash ^ ((a_value >> (b * 8)) & 0xFF)) * 1099511628211ull;
			}
		};

		D3D11_BUFFER_DESC StructuredDesc(uint32_t a_stride, uint32_t a_count, D3D11_USAGE a_usage, UINT a_bind, UINT a_cpu)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = a_stride * a_count;
			desc.Usage = a_usage;
			desc.BindFlags = a_bind;
			desc.CPUAccessFlags = a_cpu;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = a_stride;
			return desc;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC BufferSRVDesc(uint32_t a_count)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			desc.Buffer.FirstElement = 0;
			desc.Buffer.NumElements = a_count;
			return desc;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC BufferUAVDesc(uint32_t a_count)
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			desc.Buffer.FirstElement = 0;
			desc.Buffer.NumElements = a_count;
			return desc;
		}

		// --- What an actor wears ---

		const RE::BSLightingShaderProperty* LitProperty(RE::BSGeometry* a_geometry)
		{
			auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
				return nullptr;
			return static_cast<const RE::BSLightingShaderProperty*>(property);
		}

		// The hair-tint material: hair (worn as equipment, or a stray head part), not a surface.
		bool IsHairTint(const RE::BSLightingShaderProperty* a_property)
		{
			return (a_property->material && a_property->material->GetFeature() == RE::BSShaderMaterial::Feature::kHairTint) ||
			       a_property->flags.any(EShaderPropertyFlag::kHairTint);
		}

		// A hash of a few of the mesh's vertex positions (0 without a CPU copy): body morphs move them.
		uint64_t Probe(RE::BSGeometry* a_geometry, const RE::NiSkinInstance* a_skin, uint32_t a_vertexCount)
		{
			const uint8_t* raw = nullptr;
			const RE::BSGraphics::VertexDesc* desc = nullptr;
			if (a_skin) {
				const auto* partition = a_skin->skinPartition.get();
				for (uint32_t p = 0; p < partition->numPartitions && !raw; ++p) {
					const auto& part = partition->partitions[p];
					if (part.buffData && part.buffData->rawVertexData) {
						raw = part.buffData->rawVertexData;
						desc = &part.vertexDesc;
					}
				}
			} else if (const auto* data = a_geometry->GetGeometryRuntimeData().rendererData; data && data->rawVertexData) {
				raw = data->rawVertexData;
				desc = &data->vertexDesc;
			}
			if (!raw || a_vertexCount == 0)
				return 0;
			const auto layout = GetVertexLayout(*desc);
			if (layout.position < 0 || layout.stride == 0 || GetDeclaredStride(*desc) != layout.stride)
				return 0;
			Fnv fnv;
			for (uint32_t k = 0; k < kProbeVertices; ++k) {
				const uint64_t v = static_cast<uint64_t>(a_vertexCount) * k / kProbeVertices;
				uint32_t bits[3];
				std::memcpy(bits, raw + v * layout.stride + layout.position, sizeof(bits));
				fnv.Mix(bits[0] | (static_cast<uint64_t>(bits[1]) << 32));
				fnv.Mix(bits[2]);
			}
			return fnv.hash;
		}

		struct SourceKey
		{
			const void* geometry = nullptr;  // compared only
			const void* skin = nullptr;      // null: rigid
			uint32_t vertexCount = 0;
			uint32_t triangleCount = 0;

			bool operator==(const SourceKey&) const = default;
		};

		// --- The build, copied on the render thread and run on a worker ---

		struct SourceCopy
		{
			bool rigid = false;
			bool twoSided = false;
			bool failed = false;  // its GPU readback failed
			uint32_t entryBase = 0;
			uint32_t entryCount = 0;
			std::vector<uint8_t> headBones;  // per entry: the head, or a bone under it
			std::vector<Rows3> rows;         // per entry: skin to world when copied
			SkinnedMeshCopy skinned;
			RigidMeshCopy rigidCopy;
		};

		struct BuildInput
		{
			uint64_t signature = 0;
			std::vector<SourceCopy> sources;
			uint32_t entryCount = 0;
			size_t unreadable = 0;
			float3 axisPoint;  // world, when copied: a point on the body's long axis
			float3 axisUp;     // world: along the body
		};

		struct MeshData
		{
			std::vector<CollisionVertex> vertices;
			std::vector<std::array<uint32_t, 4>> triangles;  // three vertices, the inside band byte
			std::vector<float4> spheres;                     // per entry: centre (its source's skin space), radius; radius < 0: no vertex
			std::string summary;
			std::string error;
		};

		// Up to eight bones and weights, merged.
		struct WeightSet
		{
			std::array<uint16_t, 8> bones{};
			std::array<float, 8> weights{};
			uint32_t count = 0;

			void Add(uint16_t a_bone, float a_weight)
			{
				if (!(a_weight > 0.0f))
					return;
				for (uint32_t i = 0; i < count; ++i) {
					if (bones[i] == a_bone) {
						weights[i] += a_weight;
						return;
					}
				}
				if (count < bones.size()) {
					bones[count] = a_bone;
					weights[count++] = a_weight;
					return;
				}
				const auto lightest = static_cast<size_t>(std::ranges::min_element(weights) - weights.begin());
				if (weights[lightest] < a_weight) {
					bones[lightest] = a_bone;
					weights[lightest] = a_weight;
				}
			}

			void Add(const WeightSet& a_other, float a_scale)
			{
				for (uint32_t i = 0; i < a_other.count; ++i)
					Add(a_other.bones[i], a_other.weights[i] * a_scale);
			}

			// The four heaviest as palette entries from a_base and unorm8 weights summing to 255.
			void Pack(uint32_t a_base, CollisionVertex& o_vertex) const
			{
				std::array<uint32_t, 8> order;
				std::iota(order.begin(), order.end(), 0u);
				std::sort(order.begin(), order.begin() + count, [&](uint32_t a, uint32_t b) { return weights[a] > weights[b]; });
				const uint32_t used = std::min(count, 4u);
				float total = 0.0f;
				for (uint32_t i = 0; i < used; ++i)
					total += weights[order[i]];
				std::array<uint32_t, 4> entry{ a_base, a_base, a_base, a_base };
				std::array<int32_t, 4> bytes{ 255, 0, 0, 0 };
				if (total > 0.0f) {
					int32_t sum = 0;
					for (uint32_t i = 0; i < used; ++i) {
						entry[i] = a_base + bones[order[i]];
						bytes[i] = static_cast<int32_t>(std::lround(weights[order[i]] / total * 255.0f));
						sum += bytes[i];
					}
					bytes[0] = std::clamp(bytes[0] + 255 - sum, 0, 255);
				}
				o_vertex.bones01 = (entry[0] & 0xFFFF) | (entry[1] << 16);
				o_vertex.bones23 = (entry[2] & 0xFFFF) | (entry[3] << 16);
				o_vertex.weights = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
			}

			// Skin to world blended over these bones, from a source's rows.
			Rows3 Blend(const std::vector<Rows3>& a_rows) const
			{
				Rows3 rows{};
				float total = 0.0f;
				for (uint32_t i = 0; i < count; ++i) {
					if (bones[i] >= a_rows.size())
						continue;
					for (int r = 0; r < 3; ++r)
						rows[r] += a_rows[bones[i]][r] * weights[i];
					total += weights[i];
				}
				if (!(total > 0.0f))
					return a_rows.empty() ? Rows3{} : a_rows[0];
				for (auto& row : rows)
					row /= total;
				return rows;
			}
		};

		using Triangle = std::array<uint32_t, 3>;

		// One source's collision mesh, in its skin space.
		struct SourceMesh
		{
			std::vector<float3> positions;
			std::vector<float3> normals;
			std::vector<WeightSet> weights;
			std::vector<Triangle> triangles;
			std::vector<uint8_t> bands;  // per triangle: its inside band byte
		};

		float3 FaceNormal(const std::vector<float3>& a_positions, const Triangle& a_triangle, float* o_twiceArea = nullptr)
		{
			const float3& a = a_positions[a_triangle[0]];
			float3 n = (a_positions[a_triangle[1]] - a).Cross(a_positions[a_triangle[2]] - a);
			const float length = n.Length();
			if (o_twiceArea)
				*o_twiceArea = length;
			return length > 1e-12f ? n / length : float3::Zero;
		}

		// Möller-Trumbore, with a little barycentric slack.
		bool RayTriangle(const float3& a_origin, const float3& a_direction, const float3& a_a, const float3& a_b, const float3& a_c, float& o_t)
		{
			const float3 e1 = a_b - a_a;
			const float3 e2 = a_c - a_a;
			const float3 p = a_direction.Cross(e2);
			const float det = e1.Dot(p);
			if (std::abs(det) < 1e-12f)
				return false;
			const float inverse = 1.0f / det;
			const float3 s = a_origin - a_a;
			const float u = s.Dot(p) * inverse;
			if (u < -kRayTolerance || u > 1.0f + kRayTolerance)
				return false;
			const float3 q = s.Cross(e1);
			const float v = a_direction.Dot(q) * inverse;
			if (v < -kRayTolerance || u + v > 1.0f + kRayTolerance)
				return false;
			o_t = e2.Dot(q) * inverse;
			return o_t > 0.0f;
		}

		// The triangles of one mesh by the cells of a coarse grid they overlap, for rays.
		class TriangleGrid
		{
		public:
			TriangleGrid(const SourceMesh& a_mesh, const std::vector<uint8_t>& a_dropped) :
				mesh(a_mesh)
			{
				float3 low(std::numeric_limits<float>::max());
				float3 high(-std::numeric_limits<float>::max());
				for (const auto& p : mesh.positions) {
					low = float3::Min(low, p);
					high = float3::Max(high, p);
				}
				cell = kRayCell;
				const float3 extent = float3::Max(high - low, float3(1e-3f));
				for (;;) {
					for (int a = 0; a < 3; ++a)
						dims[a] = static_cast<int32_t>(std::ceil(Axis(extent, a) / cell)) + 1;
					if (static_cast<uint64_t>(dims[0]) * dims[1] * dims[2] <= kMaxRayCells)
						break;
					cell *= 1.5f;
				}
				origin = low;
				const size_t cells = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
				start.assign(cells + 1, 0);
				const auto visit = [&](uint32_t a_triangle, auto&& a_action) {
					float3 lo = mesh.positions[mesh.triangles[a_triangle][0]];
					float3 hi = lo;
					for (int k = 1; k < 3; ++k) {
						lo = float3::Min(lo, mesh.positions[mesh.triangles[a_triangle][k]]);
						hi = float3::Max(hi, mesh.positions[mesh.triangles[a_triangle][k]]);
					}
					int32_t first[3], last[3];
					for (int a = 0; a < 3; ++a) {
						first[a] = Clamp(static_cast<int32_t>(std::floor((Axis(lo, a) - Axis(origin, a)) / cell)), a);
						last[a] = Clamp(static_cast<int32_t>(std::floor((Axis(hi, a) - Axis(origin, a)) / cell)), a);
					}
					for (int32_t z = first[2]; z <= last[2]; ++z)
						for (int32_t y = first[1]; y <= last[1]; ++y)
							for (int32_t x = first[0]; x <= last[0]; ++x)
								a_action(Index(x, y, z));
				};
				for (uint32_t t = 0; t < mesh.triangles.size(); ++t) {
					if (!a_dropped[t])
						visit(t, [&](size_t a_cell) { ++start[a_cell + 1]; });
				}
				std::partial_sum(start.begin(), start.end(), start.begin());
				items.resize(start.back());
				std::vector<uint32_t> fill(start.begin(), start.end() - 1);
				for (uint32_t t = 0; t < mesh.triangles.size(); ++t) {
					if (!a_dropped[t])
						visit(t, [&](size_t a_cell) { items[fill[a_cell]++] = t; });
				}
			}

			// How far a ray can go through the mesh's cells.
			float Extent() const
			{
				return cell * std::sqrt(static_cast<float>(dims[0] * dims[0] + dims[1] * dims[1] + dims[2] * dims[2]));
			}

			// The nearest hit within a_maxT on a triangle a_accept takes; -1 if none.
			template <class Accept>
			int32_t Cast(const float3& a_origin, const float3& a_direction, float a_maxT, Accept&& a_accept, float& o_t) const
			{
				int32_t best = -1;
				o_t = a_maxT;
				size_t lastCell = std::numeric_limits<size_t>::max();
				bool entered = false;
				const float step = cell * 0.5f;
				for (float s = 0.0f; s <= a_maxT + step && s <= o_t + cell; s += step) {
					const float3 p = a_origin + a_direction * s;
					int32_t c[3];
					bool inside = true;
					for (int a = 0; a < 3; ++a) {
						c[a] = static_cast<int32_t>(std::floor((Axis(p, a) - Axis(origin, a)) / cell));
						inside = inside && c[a] >= 0 && c[a] < dims[a];
					}
					if (!inside) {
						if (entered)
							break;  // out of the grid's box, which it cannot enter again
						continue;
					}
					entered = true;
					const size_t index = Index(c[0], c[1], c[2]);
					if (index == lastCell)
						continue;
					lastCell = index;
					for (uint32_t k = start[index]; k < start[index + 1]; ++k) {
						const uint32_t t = items[k];
						if (!a_accept(t))
							continue;
						const auto& triangle = mesh.triangles[t];
						float hit;
						if (RayTriangle(a_origin, a_direction, mesh.positions[triangle[0]], mesh.positions[triangle[1]], mesh.positions[triangle[2]], hit) && hit <= o_t) {
							o_t = hit;
							best = static_cast<int32_t>(t);
						}
					}
				}
				return best;
			}

		private:
			static float Axis(const float3& a_v, int a_axis) { return a_axis == 0 ? a_v.x : (a_axis == 1 ? a_v.y : a_v.z); }
			int32_t Clamp(int32_t a_value, int a_axis) const { return std::clamp(a_value, 0, dims[a_axis] - 1); }
			size_t Index(int32_t a_x, int32_t a_y, int32_t a_z) const { return (static_cast<size_t>(a_z) * dims[1] + a_y) * dims[0] + a_x; }

			const SourceMesh& mesh;
			float3 origin;
			float cell = kRayCell;
			int32_t dims[3]{};
			std::vector<uint32_t> start;
			std::vector<uint32_t> items;
		};

		struct ClusterKey
		{
			int32_t x, y, z;
			uint32_t boneOctant;
			bool operator==(const ClusterKey&) const = default;
		};

		struct ClusterKeyHash
		{
			size_t operator()(const ClusterKey& a_key) const
			{
				Fnv fnv;
				fnv.Mix(static_cast<uint32_t>(a_key.x) | (static_cast<uint64_t>(static_cast<uint32_t>(a_key.y)) << 32));
				fnv.Mix(static_cast<uint32_t>(a_key.z) | (static_cast<uint64_t>(a_key.boneOctant) << 32));
				return static_cast<size_t>(fnv.hash);
			}
		};

		// One decoded source as a collision mesh: the head's triangles dropped, vertices merged within
		// cells of a_clusterSize (never across bones or facing octants, so the two sides of a plate and
		// parts on different bones stay apart), sheets made one-sided and facing away from the body,
		// each triangle given its inside band, long triangles split.
		void BuildSourceMesh(const SourceCopy& a_source, const HairMeshData& a_mesh, const BuildInput& a_input, float a_clusterSize, SourceMesh& o_mesh)
		{
			o_mesh = {};
			const size_t vertexCount = a_mesh.positions.size();
			const auto& indices = a_mesh.indices;

			// The head's vertices (and the triangles wholly on it) are the head field's. A vertex on no
			// bone would be skinned to the camera: no triangle of it is kept.
			std::vector<uint8_t> onHead(vertexCount, 0);
			std::vector<uint8_t> unskinned(vertexCount, 0);
			if (!a_source.rigid) {
				for (size_t v = 0; v < vertexCount; ++v) {
					float weight = 0.0f;
					float total = 0.0f;
					for (int i = 0; i < 4; ++i) {
						const uint16_t bone = a_mesh.boneIndices[v][i];
						total += a_mesh.boneWeights[v][i];
						if (bone < a_source.headBones.size() && a_source.headBones[bone])
							weight += a_mesh.boneWeights[v][i];
					}
					onHead[v] = weight >= kHeadWeight;
					unskinned[v] = !(total > 1e-3f);
				}
			}
			std::vector<Triangle> kept;
			kept.reserve(indices.size() / 3);
			for (size_t t = 0; t + 2 < indices.size(); t += 3) {
				const Triangle triangle{ indices[t], indices[t + 1], indices[t + 2] };
				if (triangle[0] >= vertexCount || triangle[1] >= vertexCount || triangle[2] >= vertexCount)
					continue;
				if (onHead[triangle[0]] && onHead[triangle[1]] && onHead[triangle[2]])
					continue;
				if (unskinned[triangle[0]] || unskinned[triangle[1]] || unskinned[triangle[2]])
					continue;
				kept.push_back(triangle);
			}
			if (kept.empty())
				return;

			// Vertex normals: the mesh's own, or from its faces where it has none.
			std::vector<float3> normals(vertexCount, float3::Zero);
			for (const auto& triangle : kept) {
				const float3& a = a_mesh.positions[triangle[0]];
				const float3 n = (a_mesh.positions[triangle[1]] - a).Cross(a_mesh.positions[triangle[2]] - a);
				for (const uint32_t v : triangle)
					normals[v] += n;
			}
			for (size_t v = 0; v < vertexCount; ++v) {
				const bool own = a_mesh.normals.size() == vertexCount && a_mesh.normals[v].LengthSquared() > 0.25f;
				normals[v] = own ? a_mesh.normals[v] : normals[v];
				if (!(normals[v].LengthSquared() > 1e-20f))
					normals[v] = float3(0.0f, 0.0f, 1.0f);
				normals[v].Normalize();
			}

			// Merge vertices: within a cell, on the same main bone, facing the same octant.
			std::vector<int32_t> cluster(vertexCount, -1);
			std::unordered_map<ClusterKey, uint32_t, ClusterKeyHash> clusters;
			std::vector<std::array<double, 3>> sums;
			std::vector<uint32_t> counts;
			const auto weightsOf = [&](size_t a_v) {
				WeightSet set;
				if (a_source.rigid)
					set.Add(0, 1.0f);
				else
					for (int i = 0; i < 4; ++i)
						set.Add(a_mesh.boneIndices[a_v][i], a_mesh.boneWeights[a_v][i]);
				return set;
			};
			for (const auto& triangle : kept) {
				for (const uint32_t v : triangle) {
					if (cluster[v] >= 0)
						continue;
					const WeightSet set = weightsOf(v);
					uint32_t mainBone = 0;
					float mainWeight = -1.0f;
					for (uint32_t i = 0; i < set.count; ++i) {
						if (set.weights[i] > mainWeight) {
							mainWeight = set.weights[i];
							mainBone = set.bones[i];
						}
					}
					const float3& p = a_mesh.positions[v];
					const float3& n = normals[v];
					const uint32_t octant = (n.x >= 0.0f ? 1u : 0u) | (n.y >= 0.0f ? 2u : 0u) | (n.z >= 0.0f ? 4u : 0u);
					const ClusterKey key{ static_cast<int32_t>(std::floor(p.x / a_clusterSize)), static_cast<int32_t>(std::floor(p.y / a_clusterSize)),
						static_cast<int32_t>(std::floor(p.z / a_clusterSize)), (mainBone << 3) | octant };
					const auto [it, added] = clusters.try_emplace(key, static_cast<uint32_t>(o_mesh.positions.size()));
					if (added) {
						o_mesh.positions.emplace_back();
						o_mesh.normals.emplace_back(float3::Zero);
						o_mesh.weights.emplace_back();
						sums.push_back({ 0.0, 0.0, 0.0 });
						counts.push_back(0);
					}
					const uint32_t c = it->second;
					cluster[v] = static_cast<int32_t>(c);
					sums[c][0] += p.x;
					sums[c][1] += p.y;
					sums[c][2] += p.z;
					++counts[c];
					o_mesh.normals[c] += n;
					o_mesh.weights[c].Add(set, 1.0f);
				}
			}
			for (size_t c = 0; c < o_mesh.positions.size(); ++c) {
				o_mesh.positions[c] = float3(static_cast<float>(sums[c][0] / counts[c]), static_cast<float>(sums[c][1] / counts[c]), static_cast<float>(sums[c][2] / counts[c]));
				if (!(o_mesh.normals[c].LengthSquared() > 1e-20f))
					o_mesh.normals[c] = float3(0.0f, 0.0f, 1.0f);
				o_mesh.normals[c].Normalize();
			}

			// The merged triangles, without degenerate ones and repeats, wound to face their normals.
			std::unordered_set<uint64_t> seen;
			for (const auto& triangle : kept) {
				Triangle merged{ static_cast<uint32_t>(cluster[triangle[0]]), static_cast<uint32_t>(cluster[triangle[1]]), static_cast<uint32_t>(cluster[triangle[2]]) };
				if (merged[0] == merged[1] || merged[1] == merged[2] || merged[0] == merged[2])
					continue;
				float twiceArea = 0.0f;
				const float3 n = FaceNormal(o_mesh.positions, merged, &twiceArea);
				if (!(twiceArea > 1e-6f))
					continue;
				if (n.Dot(o_mesh.normals[merged[0]] + o_mesh.normals[merged[1]] + o_mesh.normals[merged[2]]) < 0.0f)
					std::swap(merged[1], merged[2]);
				const auto first = static_cast<size_t>(std::ranges::min_element(merged) - merged.begin());
				const uint64_t key = (static_cast<uint64_t>(merged[first]) << 42) | (static_cast<uint64_t>(merged[(first + 1) % 3]) << 21) | merged[(first + 2) % 3];
				if (seen.insert(key).second)
					o_mesh.triangles.push_back(merged);
			}
			const size_t triangleCount = o_mesh.triangles.size();
			if (triangleCount == 0)
				return;

			// Which way is out: away from the body's long axis, in the pose the meshes were copied in.
			std::vector<float3> faceNormals(triangleCount);
			std::vector<float> areas(triangleCount);
			std::vector<float> outward(triangleCount, 0.0f);
			{
				std::vector<float3> posed(o_mesh.positions.size());
				for (size_t c = 0; c < posed.size(); ++c)
					posed[c] = Apply(o_mesh.weights[c].Blend(a_source.rows), o_mesh.positions[c]);
				for (size_t t = 0; t < triangleCount; ++t) {
					faceNormals[t] = FaceNormal(o_mesh.positions, o_mesh.triangles[t], &areas[t]);
					const auto& triangle = o_mesh.triangles[t];
					const float3 n = FaceNormal(posed, triangle);
					float3 radial = (posed[triangle[0]] + posed[triangle[1]] + posed[triangle[2]]) / 3.0f - a_input.axisPoint;
					radial -= a_input.axisUp * radial.Dot(a_input.axisUp);
					const float length = radial.Length();
					outward[t] = length > 1e-3f ? n.Dot(radial / length) : 0.0f;
				}
			}

			// Sheets: of two faces closer than kThinSheet back to back (a thin plate, or one surface
			// with its back faces modelled), the one facing out stays. Across a sheet the field would
			// flip sign from cell to cell, and a plate thinner than a cell slips between them.
			std::vector<uint8_t> dropped(triangleCount, 0);
			std::vector<uint8_t> sheet(triangleCount, 0);
			{
				const TriangleGrid grid(o_mesh, dropped);
				for (uint32_t t = 0; t < triangleCount; ++t) {
					const float3& n = faceNormals[t];
					const auto& triangle = o_mesh.triangles[t];
					const float3 centroid = (o_mesh.positions[triangle[0]] + o_mesh.positions[triangle[1]] + o_mesh.positions[triangle[2]]) / 3.0f;
					constexpr float lift = 0.01f;
					float hit;
					const int32_t partner = grid.Cast(
						centroid + n * lift, -n, kThinSheet + lift, [&](uint32_t a_other) { return a_other != t && faceNormals[a_other].Dot(n) < -0.5f; }, hit);
					sheet[t] = partner >= 0;
					if (partner >= 0 && (outward[t] < outward[partner] || (outward[t] == outward[partner] && t > static_cast<uint32_t>(partner))))
						dropped[t] = 1;
				}
			}

			// Two-sided materials are single sheets: turned to face out, as most of their area does.
			if (a_source.twoSided) {
				double vote = 0.0;
				for (size_t t = 0; t < triangleCount; ++t) {
					if (!dropped[t])
						vote += static_cast<double>(areas[t]) * outward[t];
				}
				if (vote < 0.0) {
					for (size_t t = 0; t < triangleCount; ++t) {
						std::swap(o_mesh.triangles[t][1], o_mesh.triangles[t][2]);
						faceNormals[t] = -faceNormals[t];
					}
					for (auto& normal : o_mesh.normals)
						normal = -normal;
				}
			}

			// Each triangle's inside band: how far behind it the field reaches (within kInsideBandCos of
			// straight behind). Half as far as the mesh is solid there, the shortest of five rays (straight
			// in and four tilted) to the face each leaves through: no further than the middle of a limb,
			// so a point just past a forearm, out of its near side's outside band, is never taken for the
			// inside of its far side. At least a cell (the cells just behind every surface filter with
			// those in front), at most kMaxInsideBand. A ray that leaves the mesh's box through no face
			// is open (past a collar's rim, a hem, a pauldron's edge) and does not count; however far
			// away, a face it leaves through closes it. Behind a triangle none of whose rays closes, or
			// a sheet (one face of a thin pair, a two-sided material), nothing is solid: its band is 0,
			// no inside, and the field is the distance either side of it. An inside there is the open
			// air behind the surface (above a collar's rim, between a collar and the neck), and hair in
			// it was thrown out across the surface.
			const float maxInsideUnits = kMaxInsideBand * kCellSize;
			std::vector<uint8_t> bands(triangleCount, 0);
			{
				const TriangleGrid grid(o_mesh, dropped);
				const float tiltCos = std::cos(kThicknessTilt), tiltSin = std::sin(kThicknessTilt);
				for (uint32_t t = 0; t < triangleCount; ++t) {
					if (dropped[t])
						continue;
					const float3& n = faceNormals[t];
					const auto& triangle = o_mesh.triangles[t];
					const float3 centroid = (o_mesh.positions[triangle[0]] + o_mesh.positions[triangle[1]] + o_mesh.positions[triangle[2]]) / 3.0f;
					float3 u = n.Cross(std::abs(n.z) < 0.9f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f));
					u.Normalize();
					const float3 v = n.Cross(u);
					const float3 directions[5] = { -n, -n * tiltCos + u * tiltSin, -n * tiltCos - u * tiltSin, -n * tiltCos + v * tiltSin, -n * tiltCos - v * tiltSin };
					constexpr float lift = 0.01f;
					float thickness = grid.Extent();
					bool solid = false;
					if (!sheet[t] && !a_source.twoSided) {
						for (const auto& direction : directions) {
							float hit;
							if (grid.Cast(centroid - n * lift, direction, thickness, [&](uint32_t a_other) { return a_other != t && !dropped[a_other] && faceNormals[a_other].Dot(direction) > 0.1f; }, hit) >= 0) {
								thickness = std::min(thickness, hit);
								solid = true;
							}
						}
					}
					const float band = std::clamp(0.5f * thickness, kCellSize, maxInsideUnits);
					bands[t] = solid ? static_cast<uint8_t>(std::min(std::lround(band / kCellSize * kInsideBandSteps), 255l)) : 0;
				}
			}

			// Long triangles are split (shared midpoints), so each one's cells stay few. How long is in
			// cluster cells: at a fixed length, the splits of a coarser merge brought its triangles back.
			const float maxEdge = kMaxEdge * a_clusterSize;
			std::vector<Triangle> split;
			std::unordered_map<uint64_t, uint32_t> midpoints;
			const auto midpoint = [&](uint32_t a_a, uint32_t a_b) {
				const uint64_t key = (static_cast<uint64_t>(std::min(a_a, a_b)) << 32) | std::max(a_a, a_b);
				if (const auto it = midpoints.find(key); it != midpoints.end())
					return it->second;
				const auto index = static_cast<uint32_t>(o_mesh.positions.size());
				const float3 position = (o_mesh.positions[a_a] + o_mesh.positions[a_b]) * 0.5f;
				float3 normal = o_mesh.normals[a_a] + o_mesh.normals[a_b];
				normal = normal.LengthSquared() > 1e-12f ? normal / normal.Length() : o_mesh.normals[a_a];
				WeightSet set;
				set.Add(o_mesh.weights[a_a], 0.5f);
				set.Add(o_mesh.weights[a_b], 0.5f);
				o_mesh.positions.push_back(position);
				o_mesh.normals.push_back(normal);
				o_mesh.weights.push_back(set);
				midpoints.emplace(key, index);
				return index;
			};
			std::vector<std::pair<Triangle, uint32_t>> stack;
			for (size_t t = 0; t < triangleCount; ++t) {
				if (dropped[t])
					continue;
				stack.emplace_back(o_mesh.triangles[t], 0u);
				while (!stack.empty()) {
					const auto [triangle, depth] = stack.back();
					stack.pop_back();
					const float3& a = o_mesh.positions[triangle[0]];
					const float3& b = o_mesh.positions[triangle[1]];
					const float3& c = o_mesh.positions[triangle[2]];
					const float longest = std::max({ (b - a).LengthSquared(), (c - b).LengthSquared(), (a - c).LengthSquared() });
					if (depth >= kMaxSplitDepth || !(longest > maxEdge * maxEdge)) {
						split.push_back(triangle);
						o_mesh.bands.push_back(bands[t]);
						continue;
					}
					const uint32_t ab = midpoint(triangle[0], triangle[1]);
					const uint32_t bc = midpoint(triangle[1], triangle[2]);
					const uint32_t ca = midpoint(triangle[2], triangle[0]);
					stack.push_back({ Triangle{ triangle[0], ab, ca }, depth + 1 });
					stack.push_back({ Triangle{ ab, triangle[1], bc }, depth + 1 });
					stack.push_back({ Triangle{ ca, bc, triangle[2] }, depth + 1 });
					stack.push_back({ Triangle{ ab, bc, ca }, depth + 1 });
				}
			}
			o_mesh.triangles = std::move(split);
		}

		// The whole collision mesh: every source decoded once, then merged and split, coarser while
		// over the triangle budget. Any thread.
		void BuildCollisionMesh(const BuildInput& a_input, MeshData& o_data)
		{
			o_data = {};
			std::vector<HairMeshData> decoded(a_input.sources.size());
			size_t unreadable = a_input.unreadable;
			for (size_t s = 0; s < a_input.sources.size(); ++s) {
				const auto& source = a_input.sources[s];
				std::string error;
				const bool ok = !source.failed && (source.rigid ? DecodeRigidMesh(source.rigidCopy, decoded[s], error) : DecodeSkinnedMesh(source.skinned, false, decoded[s], error));
				if (!ok) {
					decoded[s] = {};
					++unreadable;
				}
			}

			std::vector<SourceMesh> meshes(a_input.sources.size());
			float clusterSize = kClusterSize;
			size_t triangles = 0;
			for (uint32_t attempt = 0;; ++attempt) {
				triangles = 0;
				for (size_t s = 0; s < meshes.size(); ++s) {
					if (decoded[s].positions.empty())
						continue;
					BuildSourceMesh(a_input.sources[s], decoded[s], a_input, clusterSize, meshes[s]);
					triangles += meshes[s].triangles.size();
				}
				if (triangles <= kMaxTriangles || attempt >= kMaxClusterRetries)
					break;
				clusterSize *= 1.5f;
			}
			if (triangles == 0) {
				o_data.error = unreadable ? std::format("none of its {} meshes could be read ({} without vertex data or in an unknown layout)", a_input.sources.size() + a_input.unreadable, unreadable) :
				                            std::format("no surface outside the head in its {} meshes", a_input.sources.size());
				return;
			}

			size_t sources = 0;
			for (size_t s = 0; s < meshes.size() && o_data.triangles.size() < kMaxTriangles; ++s) {
				const auto& mesh = meshes[s];
				if (mesh.triangles.empty())
					continue;
				++sources;
				const auto base = static_cast<uint32_t>(o_data.vertices.size());
				for (size_t v = 0; v < mesh.positions.size(); ++v) {
					CollisionVertex vertex{};
					vertex.position = mesh.positions[v];
					vertex.normal = PackCollisionNormal(mesh.normals[v]);
					mesh.weights[v].Pack(a_input.sources[s].entryBase, vertex);
					o_data.vertices.push_back(vertex);
				}
				for (size_t t = 0; t < mesh.triangles.size() && o_data.triangles.size() < kMaxTriangles; ++t) {
					const auto& triangle = mesh.triangles[t];
					o_data.triangles.push_back({ base + triangle[0], base + triangle[1], base + triangle[2], mesh.bands[t] });
				}
			}

			// Per palette entry, a sphere round every vertex skinned to it: skinned by any blend of
			// its entries, a vertex stays inside the box round their spheres.
			std::vector<std::array<double, 4>> sums(a_input.entryCount, { 0.0, 0.0, 0.0, 0.0 });
			const auto forEntries = [](const CollisionVertex& a_vertex, auto&& a_action) {
				const uint32_t entries[4] = { a_vertex.bones01 & 0xFFFF, a_vertex.bones01 >> 16, a_vertex.bones23 & 0xFFFF, a_vertex.bones23 >> 16 };
				for (int i = 0; i < 4; ++i) {
					if ((a_vertex.weights >> (i * 8)) & 0xFF)
						a_action(entries[i]);
				}
			};
			for (const auto& vertex : o_data.vertices) {
				forEntries(vertex, [&](uint32_t a_entry) {
					if (a_entry < sums.size()) {
						sums[a_entry][0] += vertex.position.x;
						sums[a_entry][1] += vertex.position.y;
						sums[a_entry][2] += vertex.position.z;
						sums[a_entry][3] += 1.0;
					}
				});
			}
			o_data.spheres.assign(a_input.entryCount, float4(0.0f, 0.0f, 0.0f, -1.0f));
			for (size_t e = 0; e < sums.size(); ++e) {
				if (sums[e][3] > 0.0)
					o_data.spheres[e] = float4(static_cast<float>(sums[e][0] / sums[e][3]), static_cast<float>(sums[e][1] / sums[e][3]), static_cast<float>(sums[e][2] / sums[e][3]), 0.0f);
			}
			for (const auto& vertex : o_data.vertices) {
				forEntries(vertex, [&](uint32_t a_entry) {
					if (a_entry < o_data.spheres.size()) {
						auto& sphere = o_data.spheres[a_entry];
						sphere.w = std::max(sphere.w, (vertex.position - float3(sphere.x, sphere.y, sphere.z)).Length());
					}
				});
			}

			o_data.summary = std::format("{} meshes{}: {} vertices, {} triangles ({:.2f}-unit cells{})", sources,
				unreadable ? std::format(" ({} unreadable)", unreadable) : std::string(), o_data.vertices.size(), o_data.triangles.size(), clusterSize,
				triangles > kMaxTriangles ? std::format("; {} over the budget left out", triangles - kMaxTriangles) : std::string());
		}
	}

	std::unique_ptr<Texture3D> MakeVolume(const std::array<uint32_t, 3>& a_size, DXGI_FORMAT a_format, bool a_srv, const char* a_name)
	{
		D3D11_TEXTURE3D_DESC desc{};
		desc.Width = a_size[0];
		desc.Height = a_size[1];
		desc.Depth = a_size[2];
		desc.MipLevels = 1;
		desc.Format = a_format;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (a_srv ? D3D11_BIND_SHADER_RESOURCE : 0);
		auto texture = std::make_unique<Texture3D>(desc, a_name);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = a_format;
		uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
		uav.Texture3D.MipSlice = 0;
		uav.Texture3D.FirstWSlice = 0;
		uav.Texture3D.WSize = a_size[2];
		texture->CreateUAV(uav);
		if (a_srv) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = a_format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
			srv.Texture3D.MostDetailedMip = 0;
			srv.Texture3D.MipLevels = 1;
			texture->CreateSRV(srv);
		}
		return texture;
	}

	uint32_t PackCollisionNormal(const float3& a_normal)
	{
		const float l1 = std::abs(a_normal.x) + std::abs(a_normal.y) + std::abs(a_normal.z);
		float2 p{ 0.0f, 0.0f };
		float z = 1.0f;
		if (l1 > 1e-12f) {
			p = { a_normal.x / l1, a_normal.y / l1 };
			z = a_normal.z;
		}
		if (z < 0.0f)
			p = { (1.0f - std::abs(p.y)) * (p.x >= 0.0f ? 1.0f : -1.0f), (1.0f - std::abs(p.x)) * (p.y >= 0.0f ? 1.0f : -1.0f) };
		const auto snorm = [](float a_value) {
			return static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(a_value, -1.0f, 1.0f) * 32767.0f))));
		};
		return snorm(p.x) | (snorm(p.y) << 16);
	}

	BodySkeleton FindBodySkeleton(RE::NiAVObject* a_head)
	{
		BodySkeleton skeleton;
		const auto named = [](RE::NiAVObject* a_node, const char* a_name) -> RE::NiAVObject* {
			return a_node && _stricmp(a_node->name.c_str(), a_name) == 0 ? a_node : nullptr;
		};
		const auto set = [&](BodySlot a_slot, RE::NiAVObject* a_start, RE::NiAVObject* a_end) {
			if (a_start && a_end) {
				skeleton.start[static_cast<uint32_t>(a_slot)] = a_start;
				skeleton.end[static_cast<uint32_t>(a_slot)] = a_end;
			}
		};
		RE::NiAVObject* neck = a_head ? named(a_head->parent, "NPC Neck [Neck]") : nullptr;
		RE::NiAVObject* spine2 = neck ? named(neck->parent, "NPC Spine2 [Spn2]") : nullptr;
		RE::NiAVObject* spine1 = spine2 ? named(spine2->parent, "NPC Spine1 [Spn1]") : nullptr;
		RE::NiAVObject* spine = spine1 ? named(spine1->parent, "NPC Spine [Spn0]") : nullptr;
		set(BodySlot::Neck, neck, a_head);
		set(BodySlot::Chest, spine2, neck);
		set(BodySlot::Back, spine1, spine2);
		set(BodySlot::Waist, spine, spine1);
		if (!spine2)
			return skeleton;
		static const RE::BSFixedString clavicles[2] = { "NPC L Clavicle [LClv]", "NPC R Clavicle [RClv]" };
		static const RE::BSFixedString upperArms[2] = { "NPC L UpperArm [LUar]", "NPC R UpperArm [RUar]" };
		static const RE::BSFixedString forearms[2] = { "NPC L Forearm [LLar]", "NPC R Forearm [RLar]" };
		for (uint32_t side = 0; side < 2; ++side) {
			auto* clavicle = spine2->GetObjectByName(clavicles[side]);
			auto* upperArm = spine2->GetObjectByName(upperArms[side]);
			auto* forearm = upperArm ? upperArm->GetObjectByName(forearms[side]) : nullptr;
			set(static_cast<BodySlot>(static_cast<uint32_t>(BodySlot::LeftShoulder) + side), clavicle, upperArm);
			set(static_cast<BodySlot>(static_cast<uint32_t>(BodySlot::LeftArm) + side), upperArm, forearm);
		}
		return skeleton;
	}

	struct BodyCollision::LiveSource
	{
		RE::BSGeometry* geometry = nullptr;
		RE::NiSkinInstance* skin = nullptr;  // null: rigid
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		bool twoSided = false;

		SourceKey Key() const { return { geometry, skin, vertexCount, triangleCount }; }
	};

	struct BodyCollision::ActorBody
	{
		uint32_t lastSeenFrame = 0;  // RenderFrame()
		uint32_t updateFrame = UINT32_MAX;
		bool log = false;
		std::string name;

		// The collision mesh in use.
		std::vector<SourceKey> sources;       // in palette order
		std::vector<uint32_t> sourceEntries;  // each one's first palette entry
		uint32_t entryCount = 0;
		std::vector<float4> spheres;  // per entry: centre (its source's skin space), radius; radius < 0: none
		std::unique_ptr<Buffer> vertices;
		std::unique_ptr<Buffer> triangles;
		std::unique_ptr<Buffer> palette;
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		uint64_t signature = 0;  // what was worn when it was built (or last tried)

		// This frame's pose.
		std::vector<float4> rows;          // per entry: skin to world, absolute
		std::vector<float4> previousRows;  // last frame's
		uint32_t rowsFrame = UINT32_MAX;   // RenderFrame() of rows
		bool havePrevious = false;
		RE::NiMatrix3 axes;  // the actor's root: the grid's axes
		float3 origin;
		float scale = 1.0f;
		bool havePreviousRoot = false;  // the root's pose last frame, for its move over the frame
		RE::NiMatrix3 previousAxes;
		float3 previousOrigin;
		float previousScale = 1.0f;
		float reach = 0.0f;  // the longest reach asked for this frame (units)
		float previousReach = 0.0f;
		uint32_t reachFrame = UINT32_MAX;

		// What it wears, and the next build.
		uint64_t seenSignature = 0;
		uint32_t seenFrame = 0;               // RenderFrame() since which it has been worn as it is
		std::shared_ptr<BuildInput> pending;  // copied, waiting for GPU readbacks
		std::future<std::unique_ptr<MeshData>> job;
		std::vector<SourceKey> jobSources;
		std::vector<uint32_t> jobEntries;
		uint32_t jobEntryCount = 0;
		uint64_t jobSignature = 0;

		bool Building() const { return pending || job.valid(); }
		uint64_t GpuBytes() const { return static_cast<uint64_t>(vertexCount) * sizeof(CollisionVertex) + static_cast<uint64_t>(triangleCount) * 16 + static_cast<uint64_t>(entryCount) * 6 * sizeof(float4); }
	};

	BodyCollision::BodyCollision() = default;

	BodyCollision::~BodyCollision()
	{
		Reset();
	}

	void BodyCollision::Reset()
	{
		// A running build owns its input; its future waits for it here.
		actors.clear();
		skinned.reset();
		skinnedCapacity = 0;
		cells.reset();
		motion.reset();
		surface.reset();
		std::ranges::fill(fieldCapacity, 0u);
		constants.reset();
		sampler = nullptr;
		fieldActor = 0;
		fieldFrame = UINT32_MAX;
		fieldView = {};
		stats = {};
		fieldsThisFrame = 0;
	}

	void BodyCollision::BeginFrame()
	{
		const uint32_t frame = RenderFrame();
		// Never while a build runs: its future would wait for it here.
		std::erase_if(actors, [&](const auto& a_item) {
			const auto& body = *a_item.second;
			const bool running = body.job.valid() && body.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
			return !running && frame - body.lastSeenFrame > kEvictFrames;
		});

		stats = {};
		stats.fieldsBuilt = fieldsThisFrame;
		fieldsThisFrame = 0;
		for (const auto& [id, body] : actors) {
			if (body->vertices) {
				++stats.actors;
				stats.triangles += body->triangleCount;
				stats.gpuBytes += body->GpuBytes();
			}
			stats.pendingBuilds += body->Building() ? 1 : 0;
		}
		stats.gpuBytes += static_cast<uint64_t>(skinnedCapacity) * sizeof(SkinnedCollisionVertex) +
		                  static_cast<uint64_t>(fieldCapacity[0]) * fieldCapacity[1] * fieldCapacity[2] * (sizeof(uint32_t) + 2 * 4 * sizeof(uint16_t));
	}

	bool BodyCollision::HasMesh(RE::FormID a_actor) const
	{
		const auto it = actors.find(a_actor);
		return it != actors.end() && it->second->vertices;
	}

	void BodyCollision::Update(RE::Actor* a_actor, RE::NiAVObject* a_head, RE::BSGeometry* a_face, bool a_log)
	{
		if (!a_actor || !a_head)
			return;
		auto& slot = actors[a_actor->GetFormID()];
		if (!slot)
			slot = std::make_unique<ActorBody>();
		auto& body = *slot;
		const uint32_t frame = RenderFrame();
		body.lastSeenFrame = frame;
		if (body.updateFrame == frame)
			return;
		body.updateFrame = frame;
		body.log = a_log;
		if (body.name.empty()) {
			const char* name = a_actor->GetName();
			body.name = std::format("{} ({:08X})", name && *name ? name : "actor", a_actor->GetFormID());
		}

		if (body.job.valid() && body.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			TakeBuild(body);

		auto* root = a_actor->Get3D(false);
		if (!root) {
			body.rowsFrame = UINT32_MAX;
			return;
		}

		// What it wears this frame: every lit geometry under its 3D (shown), but its head parts (the
		// hair among them) and hair worn as equipment; its head mesh for the neck.
		std::vector<LiveSource> live;
		Fnv fnv;
		const auto add = [&](RE::BSGeometry* a_geometry, bool a_underHead) {
			const auto* property = LitProperty(a_geometry);
			if (!property || IsHairTint(property))
				return;
			const auto& data = a_geometry->GetGeometryRuntimeData();
			LiveSource source;
			source.geometry = a_geometry;
			source.twoSided = property->flags.any(EShaderPropertyFlag::kTwoSided);
			if (auto* skin = data.skinInstance.get()) {
				if (!skin->skinPartition || !skin->skinData || !skin->bones || skin->skinData->GetBoneCount() == 0)
					return;
				source.skin = skin;
				source.vertexCount = skin->skinPartition->vertexCount;
				for (uint32_t p = 0; p < skin->skinPartition->numPartitions; ++p)
					source.triangleCount += skin->skinPartition->partitions[p].triangles;
			} else {
				// Rigid: what hangs on the body (shields, weapons, quivers), not on the head.
				auto* shape = a_geometry->AsTriShape();
				if (a_underHead || !shape || !data.rendererData)
					return;
				const auto& counts = shape->GetTrishapeRuntimeData();
				source.vertexCount = counts.vertexCount;
				source.triangleCount = counts.triangleCount;
			}
			if (source.vertexCount == 0 || source.triangleCount == 0)
				return;
			if (std::ranges::any_of(live, [&](const LiveSource& a_other) { return a_other.geometry == a_geometry; }))
				return;
			fnv.Mix(reinterpret_cast<uintptr_t>(a_geometry));
			fnv.Mix(reinterpret_cast<uintptr_t>(source.skin));
			fnv.Mix(source.vertexCount | (static_cast<uint64_t>(source.triangleCount) << 32) | (static_cast<uint64_t>(source.twoSided) << 63));
			// Not the head mesh's: its vertices move with every expression.
			if (a_geometry != a_face)
				fnv.Mix(Probe(a_geometry, source.skin, source.vertexCount));
			live.push_back(source);
		};
		const RE::NiAVObject* faceNode = a_actor->GetFaceNodeSkinned();
		struct Item
		{
			RE::NiAVObject* object;
			int depth;
			bool underHead;
		};
		std::vector<Item> stack{ { root, 0, false } };
		while (!stack.empty()) {
			const Item item = stack.back();
			stack.pop_back();
			auto* object = item.object;
			if (!object || object == faceNode || object->GetAppCulled() || item.depth > kMaxSceneDepth)
				continue;
			const bool underHead = item.underHead || object == a_head;
			if (auto* geometry = object->AsGeometry()) {
				add(geometry, underHead);
				continue;
			}
			if (auto* node = object->AsNode()) {
				for (auto& child : node->GetChildren())
					stack.push_back({ child.get(), item.depth + 1, underHead });
			}
		}
		if (a_face && !a_face->GetAppCulled())
			add(a_face, false);
		const uint64_t signature = fnv.hash;

		// A rebuild once what is worn has held still (the first at once), one at a time.
		if (signature != body.seenSignature) {
			body.seenSignature = signature;
			body.seenFrame = frame;
		}
		const bool first = !body.vertices && body.signature == 0;
		if (!body.Building() && signature != body.signature && (first || frame - body.seenFrame >= kStableFrames))
			StartBuild(body, live, root, a_head, signature);
		if (body.pending)
			PollBuild(body);

		// This frame's pose of the collision mesh: each source's bones as drawn now, or, for a source
		// no longer drawn, nothing far away.
		if (!body.vertices) {
			body.rowsFrame = UINT32_MAX;
			return;
		}
		const bool consecutive = body.rowsFrame != UINT32_MAX && body.rowsFrame + 1 == frame && body.rows.size() == static_cast<size_t>(body.entryCount) * 3;
		if (consecutive)
			body.previousRows.swap(body.rows);
		body.havePrevious = consecutive;
		body.rows.assign(static_cast<size_t>(body.entryCount) * 3, float4(0.0f, 0.0f, 0.0f, kAbsent));
		for (size_t s = 0; s < body.sources.size(); ++s) {
			const auto it = std::ranges::find_if(live, [&](const LiveSource& a_live) { return a_live.Key() == body.sources[s]; });
			if (it == live.end())
				continue;
			const uint32_t base = body.sourceEntries[s];
			const uint32_t end = s + 1 < body.sourceEntries.size() ? body.sourceEntries[s + 1] : body.entryCount;
			const auto write = [&](uint32_t a_entry, const Rows3& a_rows) {
				for (int r = 0; r < 3; ++r)
					body.rows[static_cast<size_t>(a_entry) * 3 + r] = a_rows[r];
			};
			if (it->skin) {
				const uint32_t bones = std::min(it->skin->skinData->GetBoneCount(), end - base);
				for (uint32_t b = 0; b < bones; ++b)
					write(base + b, Rows(SkinToWorld(it->skin, b)));
			} else if (base < end) {
				write(base, Rows(it->geometry->world));
			}
		}
		const auto& world = root->world;
		body.havePreviousRoot = body.rowsFrame != UINT32_MAX && body.rowsFrame + 1 == frame;
		body.previousAxes = body.axes;
		body.previousOrigin = body.origin;
		body.previousScale = body.scale;
		body.axes = world.rotate;
		body.origin = ToFloat3(world.translate);
		body.scale = world.scale > 1e-3f ? world.scale : 1.0f;
		body.rowsFrame = frame;
	}

	void BodyCollision::StartBuild(ActorBody& a_body, const std::vector<LiveSource>& a_sources, RE::NiAVObject* a_root, RE::NiAVObject* a_head, uint64_t a_signature)
	{
		auto input = std::make_shared<BuildInput>();
		input->signature = a_signature;
		a_body.jobSources.clear();
		a_body.jobEntries.clear();
		for (const auto& live : a_sources) {
			SourceCopy copy;
			copy.twoSided = live.twoSided;
			std::string error;
			if (live.skin) {
				const uint32_t bones = live.skin->skinData->GetBoneCount();
				if (input->entryCount + bones > 0xFFFF)
					break;
				if (!CopySkinnedMesh(live.geometry, true, copy.skinned, error)) {
					logger::debug("[HairStrands] {}: body collision skips {}: {}", a_body.name, live.geometry->name.c_str(), error);
					++input->unreadable;
					continue;
				}
				copy.entryCount = bones;
				copy.headBones.resize(bones);
				copy.rows.resize(bones);
				for (uint32_t b = 0; b < bones; ++b) {
					copy.headBones[b] = live.skin->bones[b] && IsUnder(live.skin->bones[b], a_head);
					copy.rows[b] = Rows(SkinToWorld(live.skin, b));
				}
			} else {
				if (input->entryCount + 1 > 0xFFFF)
					break;
				if (!CopyRigidMesh(live.geometry, copy.rigidCopy, error)) {
					logger::debug("[HairStrands] {}: body collision skips {}: {}", a_body.name, live.geometry->name.c_str(), error);
					++input->unreadable;
					continue;
				}
				copy.rigid = true;
				copy.entryCount = 1;
				copy.headBones = { 0 };
				copy.rows = { Rows(live.geometry->world) };
			}
			copy.entryBase = input->entryCount;
			input->entryCount += copy.entryCount;
			a_body.jobSources.push_back(live.Key());
			a_body.jobEntries.push_back(copy.entryBase);
			input->sources.push_back(std::move(copy));
		}
		// The body's long axis, for which way is out: up the root through the spine.
		const auto& rootWorld = a_root->world;
		input->axisUp = float3(rootWorld.rotate.entry[0][2], rootWorld.rotate.entry[1][2], rootWorld.rotate.entry[2][2]);
		input->axisUp.Normalize();
		const BodySkeleton skeleton = FindBodySkeleton(a_head);
		const auto* spine = skeleton.start[static_cast<uint32_t>(BodySlot::Back)];
		input->axisPoint = ToFloat3(spine ? spine->world.translate : rootWorld.translate);

		a_body.jobEntryCount = input->entryCount;
		a_body.jobSignature = a_signature;
		a_body.pending = std::move(input);
	}

	void BodyCollision::PollBuild(ActorBody& a_body)
	{
		bool waiting = false;
		for (auto& source : a_body.pending->sources) {
			if (source.failed)
				continue;
			const auto status = source.rigid ? PollMeshCopy(source.rigidCopy) : PollMeshCopy(source.skinned);
			if (status == ReadbackStatus::Pending)
				waiting = true;
			else if (status == ReadbackStatus::Failed)
				source.failed = true;
		}
		if (waiting)
			return;
		a_body.job = std::async(std::launch::async, [input = std::move(a_body.pending)]() -> std::unique_ptr<MeshData> {
			auto data = std::make_unique<MeshData>();
			try {
				BuildCollisionMesh(*input, *data);
			} catch (const std::exception& e) {
				*data = {};
				data->error = e.what();  // bad_alloc on an absurd mesh must not reach the render thread
			}
			return data;
		});
	}

	void BodyCollision::TakeBuild(ActorBody& a_body)
	{
		std::unique_ptr<MeshData> data;
		try {
			data = a_body.job.get();
		} catch (const std::exception& e) {
			logger::error("[HairStrands] {}: body collision build failed: {}", a_body.name, e.what());
		}
		a_body.signature = a_body.jobSignature;
		const auto log = [&](std::string_view a_text) {
			if (a_body.log)
				logger::info("[HairStrands] {}: {}", a_body.name, a_text);
			else
				logger::debug("[HairStrands] {}: {}", a_body.name, a_text);
		};
		const auto drop = [&]() {
			a_body.vertices.reset();
			a_body.triangles.reset();
			a_body.palette.reset();
			a_body.vertexCount = 0;
			a_body.triangleCount = 0;
			a_body.sources.clear();
			a_body.sourceEntries.clear();
			a_body.entryCount = 0;
			a_body.rowsFrame = UINT32_MAX;
			fieldFrame = UINT32_MAX;  // the field may be this actor's old mesh
		};
		if (!data || !data->error.empty() || data->triangles.empty() || a_body.jobEntryCount == 0) {
			drop();
			log(std::format("no body collision ({}); collision uses bone capsules", data && !data->error.empty() ? data->error : std::string("nothing to collide with")));
			return;
		}
		const auto vertexCount = static_cast<uint32_t>(data->vertices.size());
		const auto triangleCount = static_cast<uint32_t>(data->triangles.size());
		const uint32_t entryCount = a_body.jobEntryCount;
		std::unique_ptr<Buffer> vertices, triangles, palette;
		try {
			D3D11_SUBRESOURCE_DATA vertexInit{ data->vertices.data(), 0, 0 };
			vertices = std::make_unique<Buffer>(StructuredDesc(sizeof(CollisionVertex), vertexCount, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0), &vertexInit, "HairStrands::BodyVertices");
			vertices->CreateSRV(BufferSRVDesc(vertexCount));
			D3D11_SUBRESOURCE_DATA triangleInit{ data->triangles.data(), 0, 0 };
			triangles = std::make_unique<Buffer>(StructuredDesc(sizeof(uint32_t) * 4, triangleCount, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0), &triangleInit, "HairStrands::BodyTriangles");
			triangles->CreateSRV(BufferSRVDesc(triangleCount));
			palette = std::make_unique<Buffer>(StructuredDesc(sizeof(float4), entryCount * 6, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE), nullptr, "HairStrands::BodyPalette");
			palette->CreateSRV(BufferSRVDesc(entryCount * 6));
		} catch (const std::exception& e) {
			drop();
			logger::error("[HairStrands] {}: could not create the body collision buffers: {}", a_body.name, e.what());
			return;
		}
		drop();
		a_body.vertices = std::move(vertices);
		a_body.triangles = std::move(triangles);
		a_body.palette = std::move(palette);
		a_body.vertexCount = vertexCount;
		a_body.triangleCount = triangleCount;
		a_body.sources = std::move(a_body.jobSources);
		a_body.sourceEntries = std::move(a_body.jobEntries);
		a_body.entryCount = entryCount;
		a_body.spheres = std::move(data->spheres);
		a_body.spheres.resize(entryCount, float4(0.0f, 0.0f, 0.0f, -1.0f));
		a_body.jobSources.clear();
		a_body.jobEntries.clear();
		log(std::format("body collision from {}", data->summary));
	}

	bool BodyCollision::EnsureShared(uint32_t a_vertices, const std::array<uint32_t, 3>& a_cells)
	{
		auto* device = globals::d3d::device;
		try {
			if (!constants)
				constants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<BodySdfCB>(), "HairStrands::BodySdfCB");
			if (!sampler) {
				D3D11_SAMPLER_DESC desc{};
				desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
				desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
				desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
				desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
				desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
				desc.MaxLOD = D3D11_FLOAT32_MAX;
				DX::ThrowIfFailed(device->CreateSamplerState(&desc, sampler.put()));
				Util::SetResourceName(sampler.get(), "HairStrands::BodySampler");
			}
			if (!skinned || skinnedCapacity < a_vertices) {
				skinned.reset();
				skinnedCapacity = 0;
				const uint32_t capacity = (a_vertices + 4095) / 4096 * 4096;
				skinned = std::make_unique<Buffer>(StructuredDesc(sizeof(SkinnedCollisionVertex), capacity, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0), nullptr, "HairStrands::BodySkinned");
				skinned->CreateSRV(BufferSRVDesc(capacity));
				skinned->CreateUAV(BufferUAVDesc(capacity));
				skinnedCapacity = capacity;
			}
			if (!cells || a_cells[0] > fieldCapacity[0] || a_cells[1] > fieldCapacity[1] || a_cells[2] > fieldCapacity[2]) {
				// Grown on every axis it must, kept on the others: actors take turns with it.
				std::array<uint32_t, 3> size;
				for (int a = 0; a < 3; ++a)
					size[a] = std::max(fieldCapacity[a], (a_cells[a] + 7) / 8 * 8);
				cells.reset();
				motion.reset();
				surface.reset();
				std::ranges::fill(fieldCapacity, 0u);
				fieldFrame = UINT32_MAX;
				cells = MakeVolume(size, DXGI_FORMAT_R32_UINT, false, "HairStrands::BodyCells");
				motion = MakeVolume(size, DXGI_FORMAT_R16G16B16A16_FLOAT, true, "HairStrands::BodyMotion");
				surface = MakeVolume(size, DXGI_FORMAT_R16G16B16A16_FLOAT, true, "HairStrands::BodySurface");
				for (int a = 0; a < 3; ++a)
					fieldCapacity[a] = size[a];
			}
		} catch (const std::exception& e) {
			logger::error("[HairStrands] Could not create the body field: {}", e.what());
			skinned.reset();
			skinnedCapacity = 0;
			cells.reset();
			motion.reset();
			surface.reset();
			std::ranges::fill(fieldCapacity, 0u);
			fieldFrame = UINT32_MAX;
			return false;
		}
		return true;
	}

	bool BodyCollision::Prepare(RE::FormID a_actor, const float3& a_eye, const float3& a_centre, float a_reach, const BodySdfPrograms& a_programs, BodyFieldView& o_view)
	{
		const auto it = actors.find(a_actor);
		const uint32_t frame = RenderFrame();
		if (it == actors.end() || !a_programs.Ready())
			return false;
		auto& body = *it->second;
		if (!body.vertices || body.rowsFrame != frame)
			return false;
		// Every hair of the actor shares its field: as far as the longest of them reaches, this frame
		// or the last.
		if (body.reachFrame != frame) {
			body.previousReach = body.reachFrame + 1 == frame ? body.reach : 0.0f;
			body.reach = 0.0f;
			body.reachFrame = frame;
		}
		body.reach = std::max(body.reach, a_reach);
		if (fieldActor == a_actor && fieldFrame == frame && a_reach <= fieldReach) {
			o_view = fieldView;
			return true;
		}
		a_reach = std::max(body.reach, body.previousReach);

		// The grid, in the actor's axes: what the hair can reach, where the body is (a box round each
		// palette entry's sphere), and the outside band past that.
		const RE::NiMatrix3& axes = body.axes;
		const auto toLocal = [&](const float3& a_world) {
			const float3 d = a_world - body.origin;
			return float3(axes.entry[0][0] * d.x + axes.entry[1][0] * d.y + axes.entry[2][0] * d.z,
				axes.entry[0][1] * d.x + axes.entry[1][1] * d.y + axes.entry[2][1] * d.z,
				axes.entry[0][2] * d.x + axes.entry[1][2] * d.y + axes.entry[2][2] * d.z);
		};
		float3 bodyLow(std::numeric_limits<float>::max());
		float3 bodyHigh(-std::numeric_limits<float>::max());
		for (uint32_t e = 0; e < body.entryCount; ++e) {
			const float4& sphere = body.spheres[e];
			const float4* rows = &body.rows[static_cast<size_t>(e) * 3];
			const float scale = float3(rows[0].x, rows[1].x, rows[2].x).Length();
			if (sphere.w < 0.0f || !(scale > 1e-4f))
				continue;
			const Rows3 entryRows{ rows[0], rows[1], rows[2] };
			const float3 centre = toLocal(Apply(entryRows, float3(sphere.x, sphere.y, sphere.z)));
			const float radius = sphere.w * scale;
			bodyLow = float3::Min(bodyLow, centre - float3(radius));
			bodyHigh = float3::Max(bodyHigh, centre + float3(radius));
		}
		float cellSize = kCellSize * body.scale;
		const float3 reachCentre = toLocal(a_centre + a_eye);
		const float3 low = float3::Max(reachCentre - float3(a_reach), bodyLow - float3(kOutsideBand * cellSize));
		const float3 high = float3::Min(reachCentre + float3(a_reach), bodyHigh + float3(kOutsideBand * cellSize));
		if (!(low.x < high.x && low.y < high.y && low.z < high.z))
			return false;
		// Snapped to cells fixed on the actor, so the field holds still as the actor moves.
		std::array<uint32_t, 3> size{};
		float3 start;
		for (uint32_t attempt = 0;; ++attempt) {
			start = float3(std::floor(low.x / cellSize), std::floor(low.y / cellSize), std::floor(low.z / cellSize));
			size = { static_cast<uint32_t>(std::max(std::ceil(high.x / cellSize) - start.x, 1.0f)), static_cast<uint32_t>(std::max(std::ceil(high.y / cellSize) - start.y, 1.0f)),
				static_cast<uint32_t>(std::max(std::ceil(high.z / cellSize) - start.z, 1.0f)) };
			const bool fits = size[0] <= kMaxGridAxis && size[1] <= kMaxGridAxis && size[2] <= kMaxGridAxis && static_cast<uint64_t>(size[0]) * size[1] * size[2] <= kMaxGridCells;
			if (fits)
				break;
			if (attempt >= kMaxCoarsening)
				return false;
			cellSize *= 1.25f;
		}
		if (!EnsureShared(body.vertexCount, size))
			return false;

		// Camera-relative to grid cells: (axes^T (p + eye - origin)) / cell - start.
		BodySdfCB cb{};
		const float3 eyeLocal = toLocal(a_eye);
		for (int a = 0; a < 3; ++a) {
			const float startAxis = a == 0 ? start.x : (a == 1 ? start.y : start.z);
			const float eyeAxis = a == 0 ? eyeLocal.x : (a == 1 ? eyeLocal.y : eyeLocal.z);
			cb.worldToGrid[a] = { axes.entry[0][a] / cellSize, axes.entry[1][a] / cellSize, axes.entry[2][a] / cellSize, eyeAxis / cellSize - startAxis };
			cb.gridToWorld[a] = { axes.entry[a][0] * cellSize, axes.entry[a][1] * cellSize, axes.entry[a][2] * cellSize, 0.0f };
			cb.gridSize[a] = size[a];
		}
		cb.vertexCount = body.vertexCount;
		cb.triangleCount = body.triangleCount;
		cb.entryCount = body.entryCount;
		cb.outsideBand = kOutsideBand;
		const float coarsening = cellSize / (kCellSize * body.scale);
		cb.insideScale = 1.0f / (kInsideBandSteps * coarsening);
		cb.cellSize = cellSize;
		cb.maxMotion = kMaxMotion * body.scale;
		cb.insideCos = kInsideCos;
		cb.maxExtent = kMaxTriangleExtent;
		cb.insideBandCos = kInsideBandCos;
		constants->Update(cb);

		// The root's move over the frame, x -> R (x - o0) + o1 with R = (s1 / s0) A1 A0^T, as a
		// camera-relative point's move: (R - I) p + R (eye - o0) + o1 - eye. None over a jump.
		std::array<float4, 3> rootMove{};
		if (body.havePreviousRoot && (body.origin - body.previousOrigin).Length() <= kMaxMotion * body.scale) {
			const float ratio = body.scale / body.previousScale;
			const float3 fromOrigin = a_eye - body.previousOrigin;
			const float3 toOrigin = body.origin - a_eye;
			const float to[3] = { toOrigin.x, toOrigin.y, toOrigin.z };
			for (int r = 0; r < 3; ++r) {
				float row[3];
				for (int c = 0; c < 3; ++c) {
					row[c] = ratio * (body.axes.entry[r][0] * body.previousAxes.entry[c][0] + body.axes.entry[r][1] * body.previousAxes.entry[c][1] +
										 body.axes.entry[r][2] * body.previousAxes.entry[c][2]);
				}
				const float translation = row[0] * fromOrigin.x + row[1] * fromOrigin.y + row[2] * fromOrigin.z + to[r];
				rootMove[r] = { row[0] - (r == 0 ? 1.0f : 0.0f), row[1] - (r == 1 ? 1.0f : 0.0f), row[2] - (r == 2 ? 1.0f : 0.0f), translation };
			}
		}

		// The palette: this frame's rows, then last frame's, both relative to this frame's camera.
		auto* context = globals::d3d::context;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(body.palette->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		auto* out = static_cast<float4*>(mapped.pData);
		const float eye[3] = { a_eye.x, a_eye.y, a_eye.z };
		const auto& previous = body.havePrevious ? body.previousRows : body.rows;
		const size_t rowCount = static_cast<size_t>(body.entryCount) * 3;
		for (size_t i = 0; i < rowCount; ++i) {
			out[i] = body.rows[i];
			out[i].w -= eye[i % 3];
			out[rowCount + i] = previous[i];
			out[rowCount + i].w -= eye[i % 3];
		}
		context->Unmap(body.palette->resource.get(), 0);

		globals::profiler->BeginPass("HairStrands::BodyField");
		ID3D11Buffer* cbBuffer = constants->CB();
		context->CSSetConstantBuffers(0, 1, &cbBuffer);
		ID3D11ShaderResourceView* srvs[4] = { body.vertices->srv.get(), body.palette->srv.get(), body.triangles->srv.get(), nullptr };
		context->CSSetShaderResources(0, 4, srvs);
		ID3D11UnorderedAccessView* uavs[4] = { skinned->uav.get(), nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		context->CSSetShader(a_programs.skin.get(), nullptr, 0);
		context->Dispatch((body.vertexCount + 63) / 64, 1, 1);

		// The skinned vertices move from the UAV slot to t3.
		uavs[0] = nullptr;
		uavs[1] = cells->uav.get();
		context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		srvs[3] = skinned->srv.get();
		context->CSSetShaderResources(0, 4, srvs);
		const UINT empty[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
		context->ClearUnorderedAccessViewUint(cells->uav.get(), empty);
		context->CSSetShader(a_programs.splat.get(), nullptr, 0);
		context->Dispatch((body.triangleCount + 63) / 64, 1, 1);

		uavs[2] = motion->uav.get();
		uavs[3] = surface->uav.get();
		context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		context->CSSetShader(a_programs.finalize.get(), nullptr, 0);
		context->Dispatch((size[0] + 3) / 4, (size[1] + 3) / 4, (size[2] + 3) / 4);

		ID3D11UnorderedAccessView* noUAVs[4]{};
		context->CSSetUnorderedAccessViews(0, 4, noUAVs, nullptr);
		ID3D11ShaderResourceView* noSRVs[4]{};
		context->CSSetShaderResources(0, 4, noSRVs);
		globals::profiler->EndPass();

		fieldView.toGrid = { cb.worldToGrid[0], cb.worldToGrid[1], cb.worldToGrid[2] };
		fieldView.size = float3(static_cast<float>(size[0]), static_cast<float>(size[1]), static_cast<float>(size[2]));
		fieldView.texel = float3(1.0f / fieldCapacity[0], 1.0f / fieldCapacity[1], 1.0f / fieldCapacity[2]);
		fieldView.trust = kOutsideBand * cellSize;
		fieldView.rootMove = rootMove;
		fieldView.motion = motion->srv.get();
		fieldView.surface = surface->srv.get();
		fieldView.sampler = sampler.get();
		fieldActor = a_actor;
		fieldFrame = frame;
		fieldReach = a_reach;
		++fieldsThisFrame;
		o_view = fieldView;
		return true;
	}
}
