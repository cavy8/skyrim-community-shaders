#include "CardField.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

namespace Strands
{
	namespace
	{
		constexpr float kCellSize = 0.5f;      // units between cells, at actor scale 1: a braid is a few cells thick
		constexpr float kOutsideBand = 2.0f;   // cells the field reaches either side of a card
		constexpr float kMinClearance = 0.1f;  // units: the least a strand is kept off a card, at actor scale 1
		constexpr float kMaxClearance = 0.4f;  // units: the most, where its styled place is further off
		constexpr float kMaxMotion = 64.0f;    // units: a card moving further in a frame jumped
		constexpr float kStaticMargin = 1.0f;  // units round the head's cards: some ride the neck or SMP bones
		constexpr float kChainMargin = 0.5f;   // units round a chain's cards, past their reach from its joints
		constexpr uint32_t kMaxGridAxis = 128;
		constexpr uint32_t kMaxGridCells = 512 * 1024;  // 10 MB of textures
		constexpr uint32_t kMaxCoarsening = 8;          // the cells grow (x1.25) at most this often to fit
		// BodySdf.cs.hlsl's sign tests: unused, as no card has an inside band.
		constexpr float kInsideCos = -0.25f;
		constexpr float kInsideBandCos = -0.7f;
		constexpr float kInsideScale = 1.0f / 32.0f;

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

		float SegmentDistance(const CardsToStrands::Vec3& a_p, const CardsToStrands::Vec3& a_a, const CardsToStrands::Vec3& a_b)
		{
			const CardsToStrands::Vec3 ab = a_b - a_a;
			const float lengthSquared = ab.LengthSquared();
			const float t = lengthSquared > 1e-12f ? std::clamp((a_p - a_a).Dot(ab) / lengthSquared, 0.0f, 1.0f) : 0.0f;
			return (a_p - (a_a + ab * t)).Length();
		}
	}

	std::unique_ptr<CardCollisionMesh> BuildCardCollisionMesh(const std::vector<CardVertex>& a_vertices, const std::vector<uint32_t>& a_indices, const std::vector<CardsToStrands::ChainCurve>& a_chains)
	{
		const auto triangleCount = static_cast<uint32_t>(a_indices.size() / 3);
		if (a_vertices.empty() || triangleCount == 0)
			return nullptr;

		auto mesh = std::make_unique<CardCollisionMesh>();
		mesh->vertexCount = static_cast<uint32_t>(a_vertices.size());
		mesh->triangleCount = triangleCount;
		mesh->staticLow = float3(std::numeric_limits<float>::max());
		mesh->staticHigh = float3(-std::numeric_limits<float>::max());
		mesh->chainReach.assign(a_chains.size(), 0.0f);

		std::vector<CollisionVertex> vertices(a_vertices.size());
		for (size_t v = 0; v < a_vertices.size(); ++v) {
			const CardVertex& card = a_vertices[v];
			vertices[v] = { card.position, PackCollisionNormal(card.normal), card.bones01, card.bones23, card.weights, 0 };

			// Which chain it hangs on, if any: the chain of its first weighted joint.
			const uint32_t bones[4] = { card.bones01 & 0xFFFF, card.bones01 >> 16, card.bones23 & 0xFFFF, card.bones23 >> 16 };
			int32_t chain = -1;
			for (uint32_t k = 0; k < 4 && chain < 0; ++k) {
				if (((card.weights >> (k * 8)) & 0xFF) == 0)
					continue;
				for (size_t c = 0; c < a_chains.size(); ++c) {
					if (bones[k] >= a_chains[c].firstBone && bones[k] < a_chains[c].firstBone + a_chains[c].joints.size()) {
						chain = static_cast<int32_t>(c);
						break;
					}
				}
			}
			if (chain < 0) {
				mesh->staticLow = float3::Min(mesh->staticLow, card.position);
				mesh->staticHigh = float3::Max(mesh->staticHigh, card.position);
				continue;
			}
			const auto& joints = a_chains[chain].joints;
			const CardsToStrands::Vec3 p(card.position.x, card.position.y, card.position.z);
			float nearest = joints.empty() ? 0.0f : (p - joints[0]).Length();
			for (size_t j = 0; j + 1 < joints.size(); ++j)
				nearest = std::min(nearest, SegmentDistance(p, joints[j], joints[j + 1]));
			mesh->chainReach[chain] = std::max(mesh->chainReach[chain], nearest);
		}

		std::vector<uint32_t> triangles(static_cast<size_t>(triangleCount) * 4);
		for (uint32_t t = 0; t < triangleCount; ++t) {
			for (uint32_t k = 0; k < 3; ++k)
				triangles[t * 4 + k] = std::min(a_indices[t * 3 + k], mesh->vertexCount - 1);
			triangles[t * 4 + 3] = 0;  // no inside band: a card is a sheet
		}

		D3D11_SUBRESOURCE_DATA verticesInit{ vertices.data(), 0, 0 };
		mesh->vertices = std::make_unique<Buffer>(StructuredDesc(sizeof(CollisionVertex), mesh->vertexCount, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0), &verticesInit, "HairStrands::CardCollisionVertices");
		mesh->vertices->CreateSRV(BufferSRVDesc(mesh->vertexCount));
		D3D11_SUBRESOURCE_DATA trianglesInit{ triangles.data(), 0, 0 };
		mesh->triangles = std::make_unique<Buffer>(StructuredDesc(16, triangleCount, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0), &trianglesInit, "HairStrands::CardCollisionTriangles");
		mesh->triangles->CreateSRV(BufferSRVDesc(triangleCount));
		return mesh;
	}

	bool CardField::EnsureShared(uint32_t a_vertices, uint32_t a_entries, const std::array<uint32_t, 3>& a_cells)
	{
		try {
			if (!buildConstants)
				buildConstants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<BodySdfCB>(), "HairStrands::CardFieldBuildCB");
			if (!viewConstants)
				viewConstants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CardFieldCB>(), "HairStrands::CardFieldCB");
			if (!palette || paletteCapacity < a_entries) {
				palette.reset();
				paletteCapacity = 0;
				const uint32_t capacity = (a_entries + 63) / 64 * 64;
				palette = std::make_unique<Buffer>(StructuredDesc(sizeof(float4), capacity * 6, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE), nullptr, "HairStrands::CardFieldPalette");
				palette->CreateSRV(BufferSRVDesc(capacity * 6));
				paletteCapacity = capacity;
			}
			if (!skinned || skinnedCapacity < a_vertices) {
				skinned.reset();
				skinnedCapacity = 0;
				const uint32_t capacity = (a_vertices + 4095) / 4096 * 4096;
				skinned = std::make_unique<Buffer>(StructuredDesc(sizeof(SkinnedCollisionVertex), capacity, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0), nullptr, "HairStrands::CardFieldSkinned");
				skinned->CreateSRV(BufferSRVDesc(capacity));
				skinned->CreateUAV(BufferUAVDesc(capacity));
				skinnedCapacity = capacity;
			}
			if (!cells || a_cells[0] > fieldCapacity[0] || a_cells[1] > fieldCapacity[1] || a_cells[2] > fieldCapacity[2]) {
				// Grown on every axis it must, kept on the others: hairs take turns with it.
				std::array<uint32_t, 3> size;
				for (int a = 0; a < 3; ++a)
					size[a] = std::max(fieldCapacity[a], (a_cells[a] + 7) / 8 * 8);
				cells.reset();
				motion.reset();
				surface.reset();
				fieldCapacity = {};
				cells = MakeVolume(size, DXGI_FORMAT_R32_UINT, false, "HairStrands::CardFieldCells");
				motion = MakeVolume(size, DXGI_FORMAT_R16G16B16A16_FLOAT, true, "HairStrands::CardFieldMotion");
				surface = MakeVolume(size, DXGI_FORMAT_R16G16B16A16_FLOAT, true, "HairStrands::CardFieldSurface");
				fieldCapacity = size;
			}
		} catch (const std::exception& e) {
			logger::error("[HairStrands] Could not create the card field: {}", e.what());
			Reset();
			return false;
		}
		return true;
	}

	bool CardField::Prepare(const CardFieldRequest& a_request, const BodySdfPrograms& a_programs, CardFieldView& o_view)
	{
		const CardCollisionMesh* mesh = a_request.mesh;
		if (!mesh || !a_programs.Ready() || !a_request.palette || a_request.bones == 0 || a_request.headBone >= a_request.bones ||
			a_request.palette->size() < static_cast<size_t>(a_request.bones) * 6)
			return false;
		const auto& rows = *a_request.palette;

		// The head's axes and scale: the grid is fixed on the head, so the field holds still on it.
		const float4* head = &rows[static_cast<size_t>(a_request.headBone) * 3];
		const float scale = float3(head[0].x, head[1].x, head[2].x).Length();
		if (!(scale > 1e-4f))
			return false;
		float axes[3][3];  // the head's rotation: column c is its axis c
		for (int r = 0; r < 3; ++r) {
			axes[r][0] = head[r].x / scale;
			axes[r][1] = head[r].y / scale;
			axes[r][2] = head[r].z / scale;
		}
		const float3 origin(head[0].w, head[1].w, head[2].w);  // camera-relative
		const auto toLocal = [&](const float3& a_p) {
			const float3 d = a_p - origin;
			return float3(axes[0][0] * d.x + axes[1][0] * d.y + axes[2][0] * d.z, axes[0][1] * d.x + axes[1][1] * d.y + axes[2][1] * d.z,
				axes[0][2] * d.x + axes[1][2] * d.y + axes[2][2] * d.z);
		};
		const auto skinToWorld = [&](const float3& a_p) {
			const float4 p(a_p.x, a_p.y, a_p.z, 1.0f);
			return float3(head[0].Dot(p), head[1].Dot(p), head[2].Dot(p));
		};

		// Where the cards are: the head's round their skin-space box, each chain's round its joints.
		float3 low(std::numeric_limits<float>::max());
		float3 high(-std::numeric_limits<float>::max());
		if (mesh->staticLow.x <= mesh->staticHigh.x) {
			for (int corner = 0; corner < 8; ++corner) {
				const float3 p((corner & 1) ? mesh->staticHigh.x : mesh->staticLow.x, (corner & 2) ? mesh->staticHigh.y : mesh->staticLow.y,
					(corner & 4) ? mesh->staticHigh.z : mesh->staticLow.z);
				const float3 local = toLocal(skinToWorld(p));
				low = float3::Min(low, local - float3(kStaticMargin * scale));
				high = float3::Max(high, local + float3(kStaticMargin * scale));
			}
		}
		for (size_t c = 0; c < a_request.chainJoints.size() && c < mesh->chainReach.size(); ++c) {
			if (!a_request.chainJoints[c])
				continue;
			const float margin = (mesh->chainReach[c] + kChainMargin) * scale;
			for (const auto& joint : *a_request.chainJoints[c]) {
				const float3 local = toLocal(float3(joint.x, joint.y, joint.z));
				low = float3::Min(low, local - float3(margin));
				high = float3::Max(high, local + float3(margin));
			}
		}
		// Only as far as the strands reach.
		const float3 reachCentre = toLocal(a_request.reachCentre);
		low = float3::Max(low, reachCentre - float3(a_request.reach));
		high = float3::Min(high, reachCentre + float3(a_request.reach));

		float cellSize = kCellSize * scale;
		std::array<uint32_t, 3> size{};
		float3 start;
		for (uint32_t attempt = 0;; ++attempt) {
			const float3 bandLow = low - float3(kOutsideBand * cellSize);
			const float3 bandHigh = high + float3(kOutsideBand * cellSize);
			if (!(bandLow.x < bandHigh.x && bandLow.y < bandHigh.y && bandLow.z < bandHigh.z))
				return false;
			start = float3(std::floor(bandLow.x / cellSize), std::floor(bandLow.y / cellSize), std::floor(bandLow.z / cellSize));
			size = { static_cast<uint32_t>(std::max(std::ceil(bandHigh.x / cellSize) - start.x, 1.0f)), static_cast<uint32_t>(std::max(std::ceil(bandHigh.y / cellSize) - start.y, 1.0f)),
				static_cast<uint32_t>(std::max(std::ceil(bandHigh.z / cellSize) - start.z, 1.0f)) };
			const bool fits = size[0] <= kMaxGridAxis && size[1] <= kMaxGridAxis && size[2] <= kMaxGridAxis && static_cast<uint64_t>(size[0]) * size[1] * size[2] <= kMaxGridCells;
			if (fits)
				break;
			if (attempt >= kMaxCoarsening)
				return false;
			cellSize *= 1.25f;
		}
		if (!EnsureShared(mesh->vertexCount, a_request.bones, size))
			return false;

		// Camera-relative to grid cells: axes^T (p - origin) / cell - start.
		BodySdfCB cb{};
		const float3 originLocal = toLocal(float3());
		for (int a = 0; a < 3; ++a) {
			const float startAxis = a == 0 ? start.x : (a == 1 ? start.y : start.z);
			const float originAxis = a == 0 ? originLocal.x : (a == 1 ? originLocal.y : originLocal.z);
			cb.worldToGrid[a] = { axes[0][a] / cellSize, axes[1][a] / cellSize, axes[2][a] / cellSize, originAxis / cellSize - startAxis };
			cb.gridToWorld[a] = { axes[a][0] * cellSize, axes[a][1] * cellSize, axes[a][2] * cellSize, 0.0f };
			cb.gridSize[a] = size[a];
		}
		cb.vertexCount = mesh->vertexCount;
		cb.triangleCount = mesh->triangleCount;
		cb.entryCount = a_request.bones;
		cb.outsideBand = kOutsideBand;
		cb.insideScale = kInsideScale;
		cb.cellSize = cellSize;
		cb.maxMotion = kMaxMotion * scale;
		cb.insideCos = kInsideCos;
		// Cards are long and the cells small: a card may span the whole grid.
		cb.maxExtent = static_cast<float>(std::max({ size[0], size[1], size[2] })) + 2.0f * kOutsideBand;
		cb.insideBandCos = kInsideBandCos;
		buildConstants->Update(cb);

		// The palette: this frame's rows, then last frame's, both relative to this frame's camera.
		auto* context = globals::d3d::context;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(palette->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		auto* out = static_cast<float4*>(mapped.pData);
		const size_t rowCount = static_cast<size_t>(a_request.bones) * 3;
		const float shift[3] = { a_request.previousToCurrent.x, a_request.previousToCurrent.y, a_request.previousToCurrent.z };
		std::memcpy(out, rows.data(), rowCount * sizeof(float4));
		for (size_t i = 0; i < rowCount; ++i) {
			out[rowCount + i] = rows[rowCount + i];
			out[rowCount + i].w += shift[i % 3];
		}
		context->Unmap(palette->resource.get(), 0);

		globals::profiler->BeginPass("HairStrands::CardField");
		ID3D11Buffer* cbBuffer = buildConstants->CB();
		context->CSSetConstantBuffers(0, 1, &cbBuffer);
		ID3D11ShaderResourceView* srvs[4] = { mesh->vertices->srv.get(), palette->srv.get(), mesh->triangles->srv.get(), nullptr };
		context->CSSetShaderResources(0, 4, srvs);
		ID3D11UnorderedAccessView* uavs[4] = { skinned->uav.get(), nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		context->CSSetShader(a_programs.skin.get(), nullptr, 0);
		context->Dispatch((mesh->vertexCount + 63) / 64, 1, 1);

		// The skinned vertices move from the UAV slot to t3.
		uavs[0] = nullptr;
		uavs[1] = cells->uav.get();
		context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		srvs[3] = skinned->srv.get();
		context->CSSetShaderResources(0, 4, srvs);
		const UINT empty[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
		context->ClearUnorderedAccessViewUint(cells->uav.get(), empty);
		context->CSSetShader(a_programs.splat.get(), nullptr, 0);
		context->Dispatch((mesh->triangleCount + 63) / 64, 1, 1);

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

		CardFieldCB view{};
		for (int a = 0; a < 3; ++a)
			view.toGrid[a] = cb.worldToGrid[a];
		view.gridSize = float3(static_cast<float>(size[0]), static_cast<float>(size[1]), static_cast<float>(size[2]));
		view.minClearance = kMinClearance * scale;
		view.maxClearance = kMaxClearance * scale;
		view.trust = kOutsideBand * cellSize;
		view.cellSize = cellSize;
		viewConstants->Update(view);

		o_view.constants = viewConstants->CB();
		o_view.motion = motion->srv.get();
		o_view.surface = surface->srv.get();
		++fieldsBuilt;
		return true;
	}

	void CardField::Reset()
	{
		palette.reset();
		paletteCapacity = 0;
		skinned.reset();
		skinnedCapacity = 0;
		cells.reset();
		motion.reset();
		surface.reset();
		fieldCapacity = {};
	}

	uint32_t CardField::TakeFieldCount()
	{
		return std::exchange(fieldsBuilt, 0u);
	}

	uint64_t CardField::GpuBytes() const
	{
		const uint64_t cellCount = static_cast<uint64_t>(fieldCapacity[0]) * fieldCapacity[1] * fieldCapacity[2];
		return cellCount * (4 + 8 + 8) + static_cast<uint64_t>(skinnedCapacity) * sizeof(SkinnedCollisionVertex) + static_cast<uint64_t>(paletteCapacity) * 6 * sizeof(float4);
	}
}
