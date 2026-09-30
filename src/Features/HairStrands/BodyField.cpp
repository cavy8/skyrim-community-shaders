#include "BodyField.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <numbers>

#include "Globals.h"
#include "MeshExtract.h"

namespace Strands
{
	namespace
	{
		constexpr float kBodyMinSegment = 0.5f;            // units; a shorter bone segment gets no collider
		constexpr float kBodyFieldMaxRadius = 40.0f;       // units from the segment; further out is not this body part
		constexpr float kBodyFieldSampleSpacing = 0.5f;    // units between the surface points taken from each triangle
		constexpr float kBodyFieldLayerGap = 0.6f;         // units: a gap this wide between radii in a texel starts a surface further in
		constexpr float kBodyFieldMinWeight = 0.05f;       // bilinear weight a texel centre needs from its samples
		constexpr uint32_t kBodyFieldFillPasses = 3;       // small holes filled from their neighbours
		constexpr float kBodyFieldMargin = 0.35f;          // units added to every radius: the rest depth plus a gap, so hair lies just outside
		constexpr float kBodyFieldMinCoverage = 0.05f;     // of a collider's map; less and it is dropped
		constexpr float kBodyFieldMaxMedian = 30.0f;       // units; a collider whose median radius is larger is not the body
		constexpr float kBodyChestMinCoverage = 0.2f;      // the chest collider must look like a chest,
		constexpr float kBodyChestMinMedian = 2.5f;        // round the spine (bone units): else the bind poses
		constexpr float kBodyChestMaxMedian = 25.0f;       // are not what this reads them as, and the capsules stay
		constexpr size_t kBodyFieldMaxTriangles = 300000;  // colliding triangles read at most
		constexpr uint32_t kBodyFieldMaxSteps = 64;        // samples along a triangle edge at most
		constexpr int kMaxSceneDepth = 64;

		constexpr const char* kSlotNames[kBodySlots] = { "neck", "chest", "back", "waist", "left shoulder", "right shoulder", "left arm", "right arm" };

		RE::NiAVObject* IfNamed(RE::NiAVObject* a_node, const char* a_name)
		{
			return a_node && _stricmp(a_node->name.c_str(), a_name) == 0 ? a_node : nullptr;
		}

		std::array<float4, 3> Rows(const RE::NiTransform& a_transform)
		{
			std::array<float4, 3> rows;
			for (int r = 0; r < 3; ++r) {
				rows[r] = { a_transform.rotate.entry[r][0] * a_transform.scale, a_transform.rotate.entry[r][1] * a_transform.scale,
					a_transform.rotate.entry[r][2] * a_transform.scale, a_transform.translate[r] };
			}
			return rows;
		}

		float3 Apply(const std::array<float4, 3>& a_rows, const float3& a_p)
		{
			const float4 p(a_p.x, a_p.y, a_p.z, 1.0f);
			return { a_rows[0].Dot(p), a_rows[1].Dot(p), a_rows[2].Dot(p) };
		}

		// Distance from field-space point a_q to its closest point on the segment (0, 0, 0)-(0, 0, a_length).
		float SegmentDistance(const float3& a_q, float a_length)
		{
			return (a_q - float3(0.0f, 0.0f, std::clamp(a_q.z, 0.0f, a_length))).Length();
		}

		struct Sample
		{
			float column;
			float row;
			float radius;
		};

		// One collider's map from its samples: per texel the outermost layer of samples, a plane fitted
		// to those round the texel's centre (bilinear weights) and read at the centre, small holes
		// filled, the margin added, then each texel's slopes. Writes (radius, slope along the column,
		// slope along the row) per texel.
		void FitMap(const std::vector<Sample>& a_samples, float* o_texels)
		{
			constexpr uint32_t columns = kBodyFieldColumns;
			constexpr uint32_t rows = kBodyFieldRows;
			const auto texelOf = [](const Sample& a_sample) {
				const uint32_t x = static_cast<uint32_t>(a_sample.column) % columns;
				const uint32_t y = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(a_sample.row), 0, static_cast<int32_t>(rows) - 1));
				return y * columns + x;
			};

			// The outermost layer: radii from the largest down to the first gap wider than the layer gap
			// (armour over the body, the body under a cloak).
			std::vector<std::vector<float>> radii(kBodyFieldTexels);
			for (const auto& sample : a_samples)
				radii[texelOf(sample)].push_back(sample.radius);
			std::vector<float> floor(kBodyFieldTexels, std::numeric_limits<float>::infinity());
			for (uint32_t t = 0; t < kBodyFieldTexels; ++t) {
				auto& list = radii[t];
				if (list.empty())
					continue;
				std::ranges::sort(list, std::greater<>());
				float lowest = list[0];
				for (size_t k = 1; k < list.size() && list[k - 1] - list[k] <= kBodyFieldLayerGap; ++k)
					lowest = list[k];
				floor[t] = lowest;
			}

			// A plane per texel through the outer samples round its centre, read at the centre: a plain
			// weighted mean leans towards wherever samples crowd, low on steep slopes. Clamped to the
			// samples' own range.
			struct Fit
			{
				double w = 0, x = 0, y = 0, xx = 0, xy = 0, yy = 0, r = 0, rx = 0, ry = 0;
				float low = std::numeric_limits<float>::infinity();
				float high = 0.0f;
			};
			std::vector<Fit> fits(kBodyFieldTexels);
			for (const auto& sample : a_samples) {
				if (sample.radius < floor[texelOf(sample)])
					continue;
				const float x = sample.column - 0.5f, y = sample.row - 0.5f;
				const float bx = std::floor(x), by = std::floor(y);
				const float fx = x - bx, fy = y - by;
				for (int j = 0; j < 2; ++j) {
					const int32_t ty = static_cast<int32_t>(by) + j;
					if (ty < 0 || ty >= static_cast<int32_t>(rows))
						continue;
					const double wy = j ? fy : 1.0 - fy, dy = j ? fy - 1.0 : fy;
					for (int i = 0; i < 2; ++i) {
						const uint32_t tx = (static_cast<uint32_t>(bx) + i) % columns;
						const double wx = i ? fx : 1.0 - fx, dx = i ? fx - 1.0 : fx;
						const double w = wx * wy, r = sample.radius;
						auto& fit = fits[ty * columns + tx];
						fit.w += w, fit.x += w * dx, fit.y += w * dy, fit.xx += w * dx * dx, fit.xy += w * dx * dy, fit.yy += w * dy * dy;
						fit.r += w * r, fit.rx += w * r * dx, fit.ry += w * r * dy;
						fit.low = std::min(fit.low, sample.radius);
						fit.high = std::max(fit.high, sample.radius);
					}
				}
			}
			std::vector<float> field(kBodyFieldTexels, 0.0f);
			for (uint32_t t = 0; t < kBodyFieldTexels; ++t) {
				const auto& f = fits[t];
				if (f.w <= kBodyFieldMinWeight)
					continue;
				double value = f.r / f.w;
				// Cramer's rule for the plane's value at the centre.
				const double det = f.w * (f.xx * f.yy - f.xy * f.xy) - f.x * (f.x * f.yy - f.xy * f.y) + f.y * (f.x * f.xy - f.xx * f.y);
				if (std::abs(det) > 1e-6 * f.w * f.w * f.w)
					value = (f.r * (f.xx * f.yy - f.xy * f.xy) - f.x * (f.rx * f.yy - f.xy * f.ry) + f.y * (f.rx * f.xy - f.xx * f.ry)) / det;
				field[t] = std::clamp(static_cast<float>(value), f.low, f.high);
			}

			// Small holes, from at least four filled neighbours: gaps between triangles, not open ends.
			for (uint32_t pass = 0; pass < kBodyFieldFillPasses; ++pass) {
				std::vector<float> filled = field;
				for (uint32_t y = 0; y < rows; ++y) {
					for (uint32_t x = 0; x < columns; ++x) {
						if (field[y * columns + x] > 0.0f)
							continue;
						float sum = 0.0f;
						uint32_t count = 0;
						for (uint32_t ny = y ? y - 1 : 0; ny <= std::min(y + 1, rows - 1); ++ny) {
							for (uint32_t k = 0; k < 3; ++k) {
								const float value = field[ny * columns + (x + columns - 1 + k) % columns];
								if (value > 0.0f) {
									sum += value;
									++count;
								}
							}
						}
						if (count >= 4)
							filled[y * columns + x] = sum / count;
					}
				}
				field.swap(filled);
			}
			for (auto& radius : field) {
				if (radius > 0.0f)
					radius += kBodyFieldMargin;
			}

			// Slopes per texel, central differences over filled neighbours: interpolated like the radius,
			// they turn the surface normal smoothly. The bilinear patch's own slopes jump at every texel
			// edge, and a point resting across one was pushed back and forth.
			for (uint32_t y = 0; y < rows; ++y) {
				for (uint32_t x = 0; x < columns; ++x) {
					const float centre = field[y * columns + x];
					float* texel = o_texels + (y * columns + x) * 3;
					texel[0] = centre;
					texel[1] = texel[2] = 0.0f;
					if (!(centre > 0.0f))
						continue;
					const auto slope = [&](float a_before, bool a_haveBefore, float a_after, bool a_haveAfter) {
						a_haveBefore = a_haveBefore && a_before > 0.0f;
						a_haveAfter = a_haveAfter && a_after > 0.0f;
						if (a_haveBefore && a_haveAfter)
							return 0.5f * (a_after - a_before);
						if (a_haveBefore)
							return centre - a_before;
						return a_haveAfter ? a_after - centre : 0.0f;
					};
					texel[1] = slope(field[y * columns + (x + columns - 1) % columns], true, field[y * columns + (x + 1) % columns], true);
					texel[2] = slope(y > 0 ? field[(y - 1) * columns + x] : 0.0f, y > 0, y + 1 < rows ? field[(y + 1) * columns + x] : 0.0f, y + 1 < rows);
				}
			}
		}
	}

	float2 BodyFieldTexel(const float3& a_q, float a_length)
	{
		constexpr float capScale = kBodyFieldCapRows / (std::numbers::pi_v<float> * 0.5f);
		const float rho = std::sqrt(a_q.x * a_q.x + a_q.y * a_q.y);
		float row;
		if (a_q.z < 0.0f)
			row = std::atan2(rho, -a_q.z) * capScale;
		else if (a_q.z > a_length)
			row = kBodyFieldRows - std::atan2(rho, a_q.z - a_length) * capScale;
		else
			row = kBodyFieldCapRows + kBodyFieldSideRows * a_q.z / std::max(a_length, 1e-4f);
		constexpr float turn = 2.0f * std::numbers::pi_v<float>;
		const float column = (std::atan2(a_q.y, a_q.x) / turn + 1.0f) * kBodyFieldColumns;
		return { column, row };
	}

	BodySkeleton FindBodySkeleton(RE::NiAVObject* a_head)
	{
		BodySkeleton skeleton;
		const auto set = [&](BodySlot a_slot, RE::NiAVObject* a_start, RE::NiAVObject* a_end) {
			if (a_start && a_end) {
				skeleton.start[static_cast<uint32_t>(a_slot)] = a_start;
				skeleton.end[static_cast<uint32_t>(a_slot)] = a_end;
			}
		};
		RE::NiAVObject* neck = a_head ? IfNamed(a_head->parent, "NPC Neck [Neck]") : nullptr;
		RE::NiAVObject* spine2 = neck ? IfNamed(neck->parent, "NPC Spine2 [Spn2]") : nullptr;
		RE::NiAVObject* spine1 = spine2 ? IfNamed(spine2->parent, "NPC Spine1 [Spn1]") : nullptr;
		RE::NiAVObject* spine = spine1 ? IfNamed(spine1->parent, "NPC Spine [Spn0]") : nullptr;
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

	int32_t BodySkeleton::SlotOf(const RE::NiAVObject* a_node) const
	{
		for (int depth = 0; a_node && depth < kMaxSceneDepth; ++depth, a_node = a_node->parent) {
			for (uint32_t s = 0; s < kBodySlots; ++s) {
				if (start[s] && a_node == start[s])
					return static_cast<int32_t>(s);
			}
			// A segment's far joint that starts no collider: the head, a forearm. What hangs from it
			// moves with it, not with the collider's bone.
			for (uint32_t s = 0; s < kBodySlots; ++s) {
				if (end[s] && a_node == end[s])
					return -1;
			}
		}
		return -1;
	}

	std::vector<RE::BSGeometry*> FindBodyMeshes(RE::NiAVObject* a_root, const RE::NiAVObject* a_faceNode, RE::BSGeometry* a_face, uint64_t& o_signature)
	{
		std::vector<RE::BSGeometry*> meshes;
		uint64_t hash = 14695981039346656037ull;  // FNV-1a
		const auto mix = [&](uint64_t a_value) {
			for (int b = 0; b < 8; ++b)
				hash = (hash ^ ((a_value >> (b * 8)) & 0xFF)) * 1099511628211ull;
		};
		const auto add = [&](RE::BSGeometry* a_geometry) {
			const auto& data = a_geometry->GetGeometryRuntimeData();
			auto* skin = data.skinInstance.get();
			if (!skin || !skin->skinPartition || !skin->skinData || !skin->bones)
				return;
			// Lit geometry only: effect shaders are glows, auras and particles, not a surface.
			if (!data.shaderProperty || data.shaderProperty->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
				return;
			meshes.push_back(a_geometry);
			mix(reinterpret_cast<uintptr_t>(a_geometry));
			mix(skin->skinPartition->vertexCount);
		};
		std::vector<std::pair<RE::NiAVObject*, int>> stack{ { a_root, 0 } };
		while (!stack.empty()) {
			const auto [object, depth] = stack.back();
			stack.pop_back();
			// Head parts (the hair among them) are skipped whole; hidden parts, with what hangs from them.
			if (!object || object == a_faceNode || object->GetAppCulled() || depth > kMaxSceneDepth)
				continue;
			if (auto* geometry = object->AsGeometry()) {
				add(geometry);
				continue;
			}
			if (auto* node = object->AsNode()) {
				for (auto& child : node->GetChildren())
					stack.emplace_back(child.get(), depth + 1);
			}
		}
		// The head mesh's neck: its head-bone vertices belong to no collider (the head field has them).
		if (a_face && !a_face->GetAppCulled())
			add(a_face);
		o_signature = hash;
		return meshes;
	}

	bool PrepareBodyField(const BodySkeleton& a_skeleton, const std::vector<RE::BSGeometry*>& a_meshes, BodyFieldInput& o_input, std::string& o_error)
	{
		o_input = {};
		// Each collider's field axes in its bone's space: +Z along the segment, X and Y any pair
		// across it (as the shaders' field-to-world rows carry them).
		std::array<RE::NiTransform, kBodySlots> fieldFromBone{};
		for (uint32_t s = 0; s < kBodySlots; ++s) {
			auto& shape = o_input.shapes[s];
			if (!a_skeleton.start[s] || !a_skeleton.end[s])
				continue;
			const RE::NiPoint3 end = a_skeleton.start[s]->world.Invert() * a_skeleton.end[s]->world.translate;
			const float length = end.Length();
			if (!(length > kBodyMinSegment) || length > kBodyFieldMaxRadius)
				continue;
			const RE::NiPoint3 z = end / length;
			RE::NiPoint3 x = z.Cross(std::abs(z.z) < 0.9f ? RE::NiPoint3(0.0f, 0.0f, 1.0f) : RE::NiPoint3(1.0f, 0.0f, 0.0f));
			x.Unitize();
			const RE::NiPoint3 y = z.Cross(x);
			for (int r = 0; r < 3; ++r) {
				shape.boneFromField.entry[r][0] = x[r];
				shape.boneFromField.entry[r][1] = y[r];
				shape.boneFromField.entry[r][2] = z[r];
			}
			shape.length = length;
			shape.present = true;
			fieldFromBone[s].rotate = shape.boneFromField.Transpose();
		}

		size_t triangles = 0;
		uint32_t unreadable = 0;
		for (auto* geometry : a_meshes) {
			if (triangles >= kBodyFieldMaxTriangles)
				break;
			// Meshes with no bone on a collider (hands, feet, legs) are not copied at all.
			auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
			auto* skinData = skin->skinData.get();
			const uint32_t boneCount = skinData->GetBoneCount();
			std::vector<int32_t> boneSlot(boneCount, -1);
			for (uint32_t b = 0; b < boneCount; ++b) {
				if (skin->bones[b])
					boneSlot[b] = a_skeleton.SlotOf(skin->bones[b]);
			}
			if (std::ranges::all_of(boneSlot, [](int32_t a_slot) { return a_slot < 0; }))
				continue;
			HairMeshData mesh;
			std::string error;
			if (!ExtractHairMesh(geometry, mesh, error) || mesh.boneNames.size() != boneCount) {
				++unreadable;
				continue;
			}

			// A vertex belongs to the collider of its heaviest bone.
			std::vector<float> boneWeight(boneCount, 0.0f);
			std::vector<int8_t> vertexSlot(mesh.positions.size(), -1);
			for (size_t v = 0; v < mesh.positions.size(); ++v) {
				const auto& weights = mesh.boneWeights[v];
				const auto heaviest = static_cast<size_t>(std::ranges::max_element(weights) - weights.begin());
				const uint16_t bone = mesh.boneIndices[v][heaviest];
				if (weights[heaviest] <= 0.0f || bone >= boneCount || boneSlot[bone] < 0)
					continue;
				vertexSlot[v] = static_cast<int8_t>(boneSlot[bone]);
				boneWeight[bone] += weights[heaviest];
			}

			// A triangle collides with every collider one of its vertices belongs to.
			BodyMesh out;
			uint32_t used = 0;
			for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
				uint8_t mask = 0;
				for (int k = 0; k < 3; ++k) {
					const int8_t slot = vertexSlot[mesh.indices[t + k]];
					if (slot >= 0 && o_input.shapes[slot].present)
						mask |= static_cast<uint8_t>(1u << slot);
				}
				if (!mask)
					continue;
				out.indices.insert(out.indices.end(), mesh.indices.begin() + t, mesh.indices.begin() + t + 3);
				out.slots.push_back(mask);
				used |= mask;
			}
			if (out.slots.empty())
				continue;

			// Skin space to each collider's field space. A mesh skinned to the collider's bone takes that
			// bone's bind pose, so every part of it keeps its bind-pose shape round the bone. Otherwise
			// (pauldrons on their own bone) the bone it is most skinned to, placed where it is now.
			for (uint32_t s = 0; s < kBodySlots; ++s) {
				if (!(used & (1u << s)))
					continue;
				int32_t via = -1;
				for (uint32_t b = 0; b < boneCount && via < 0; ++b) {
					if (skin->bones[b] == a_skeleton.start[s])
						via = static_cast<int32_t>(b);
				}
				RE::NiTransform boneFromSkin;
				if (via >= 0) {
					boneFromSkin = skinData->GetBoneDataSkinToBone(via);
				} else {
					for (uint32_t b = 0; b < boneCount; ++b) {
						if (boneSlot[b] == static_cast<int32_t>(s) && (via < 0 || boneWeight[b] > boneWeight[via]))
							via = static_cast<int32_t>(b);
					}
					if (via < 0)
						continue;
					boneFromSkin = a_skeleton.start[s]->world.Invert() * skin->bones[via]->world * skinData->GetBoneDataSkinToBone(via);
				}
				out.toField[s] = Rows(fieldFromBone[s] * boneFromSkin);
			}
			out.positions = std::move(mesh.positions);
			triangles += out.slots.size();
			o_input.meshes.push_back(std::move(out));
		}
		if (o_input.meshes.empty()) {
			o_error = unreadable ? std::format("none of the {} worn meshes could be read ({} without CPU vertex data or in an unknown layout)", a_meshes.size(), unreadable) :
			                       std::format("no triangle of the {} worn meshes is skinned to the upper body", a_meshes.size());
			return false;
		}
		return true;
	}

	void BuildBodyField(const BodyFieldInput& a_input, BodyFieldData& o_data)
	{
		o_data = {};
		o_data.shapes = a_input.shapes;
		o_data.field.assign(static_cast<size_t>(kBodySlots) * kBodyFieldTexels * 3, 0.0f);

		// Points no further apart than the sample spacing over every triangle, per collider it belongs to.
		std::array<std::vector<Sample>, kBodySlots> samples;
		for (const auto& mesh : a_input.meshes) {
			for (size_t t = 0; t < mesh.slots.size(); ++t) {
				const uint32_t i0 = mesh.indices[t * 3], i1 = mesh.indices[t * 3 + 1], i2 = mesh.indices[t * 3 + 2];
				if (i0 >= mesh.positions.size() || i1 >= mesh.positions.size() || i2 >= mesh.positions.size())
					continue;
				for (uint32_t s = 0; s < kBodySlots; ++s) {
					if (!(mesh.slots[t] & (1u << s)))
						continue;
					const float length = a_input.shapes[s].length;
					const float3 a = Apply(mesh.toField[s], mesh.positions[i0]);
					const float3 ab = Apply(mesh.toField[s], mesh.positions[i1]) - a;
					const float3 ac = Apply(mesh.toField[s], mesh.positions[i2]) - a;
					const float longest = std::max({ ab.Length(), ac.Length(), (ac - ab).Length() });
					if (!std::isfinite(longest) || longest > kBodyFieldMaxRadius)
						continue;
					const uint32_t steps = std::clamp(static_cast<uint32_t>(std::ceil(longest / kBodyFieldSampleSpacing)), 1u, kBodyFieldMaxSteps);
					for (uint32_t i = 0; i <= steps; ++i) {
						for (uint32_t j = 0; i + j <= steps; ++j) {
							const float3 q = a + ab * (static_cast<float>(i) / steps) + ac * (static_cast<float>(j) / steps);
							const float radius = SegmentDistance(q, length);
							if (radius > 1e-3f && radius <= kBodyFieldMaxRadius) {
								const float2 texel = BodyFieldTexel(q, length);
								samples[s].push_back({ texel.x, texel.y, radius });
							}
						}
					}
				}
			}
		}

		std::string summary;
		for (uint32_t s = 0; s < kBodySlots; ++s) {
			auto& shape = o_data.shapes[s];
			float* texels = o_data.field.data() + static_cast<size_t>(s) * kBodyFieldTexels * 3;
			if (shape.present && !samples[s].empty())
				FitMap(samples[s], texels);
			samples[s] = {};
			std::vector<float> radii;
			for (uint32_t t = 0; t < kBodyFieldTexels; ++t) {
				if (texels[t * 3] > 0.0f)
					radii.push_back(texels[t * 3]);
			}
			shape.coverage = static_cast<float>(radii.size()) / kBodyFieldTexels;
			float median = 0.0f;
			if (!radii.empty()) {
				std::ranges::nth_element(radii, radii.begin() + radii.size() / 2);
				median = radii[radii.size() / 2];
				shape.bound = *std::ranges::max_element(radii);
			}
			if (s == static_cast<uint32_t>(BodySlot::Chest) && !(shape.present && shape.coverage >= kBodyChestMinCoverage && median >= kBodyChestMinMedian && median <= kBodyChestMaxMedian)) {
				o_data.error = std::format("the chest collider does not fit round the spine ({:.0f}% covered, median radius {:.1f})", shape.coverage * 100.0f, median);
				return;
			}
			if (!shape.present || shape.coverage < kBodyFieldMinCoverage || median > kBodyFieldMaxMedian) {
				shape.present = false;
				std::fill_n(texels, kBodyFieldTexels * 3, 0.0f);
				continue;
			}
			summary += std::format("{}{} {:.0f}%", summary.empty() ? "" : ", ", kSlotNames[s], shape.coverage * 100.0f);
		}
		o_data.summary = std::move(summary);
	}
}
