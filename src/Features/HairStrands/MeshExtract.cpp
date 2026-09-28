#include "MeshExtract.h"

namespace Strands
{
	namespace
	{
		using DirectX::PackedVector::XMConvertHalfToFloat;
		using RE::BSGraphics::Vertex;

		// SSE packs vertex attributes in a fixed order; offsets follow from the flags alone
		// (the descriptor's offset nibbles are not trusted, see the hair-mesh notes).
		struct VertexLayout
		{
			int32_t position = -1;
			int32_t uv = -1;
			int32_t normal = -1;
			int32_t skinning = -1;
			bool fullPrecision = false;
			uint32_t stride = 0;
		};

		VertexLayout GetLayout(const RE::BSGraphics::VertexDesc& a_desc)
		{
			VertexLayout layout;
			uint32_t offset = 0;
			if (a_desc.HasFlag(Vertex::VF_VERTEX)) {
				layout.position = static_cast<int32_t>(offset);
				layout.fullPrecision = a_desc.HasFlag(Vertex::VF_FULLPREC);
				offset += layout.fullPrecision ? 16 : 8;
			}
			if (a_desc.HasFlag(Vertex::VF_UV)) {
				layout.uv = static_cast<int32_t>(offset);
				offset += 4;
			}
			if (a_desc.HasFlag(Vertex::VF_UV_2))
				offset += 4;
			if (a_desc.HasFlag(Vertex::VF_NORMAL)) {
				layout.normal = static_cast<int32_t>(offset);
				offset += 4;
				if (a_desc.HasFlag(Vertex::VF_TANGENT))
					offset += 4;
			}
			if (a_desc.HasFlag(Vertex::VF_COLORS))
				offset += 4;
			if (a_desc.HasFlag(Vertex::VF_SKINNED)) {
				layout.skinning = static_cast<int32_t>(offset);
				offset += 12;
			}
			if (a_desc.HasFlag(Vertex::VF_EYEDATA))
				offset += 4;
			layout.stride = offset;
			return layout;
		}

		float Half(const uint8_t* a_data)
		{
			return XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(a_data));
		}

		float UnpackByte(uint8_t a_value)
		{
			return a_value / 255.0f * 2.0f - 1.0f;
		}
	}

	bool ExtractHairMesh(RE::BSGeometry* a_geometry, HairMeshData& o_mesh, std::string& o_error)
	{
		o_mesh = {};
		if (!a_geometry) {
			o_error = "no geometry";
			return false;
		}

		auto& geometryData = a_geometry->GetGeometryRuntimeData();
		auto* skinInstance = geometryData.skinInstance.get();
		if (!skinInstance || !skinInstance->skinPartition || !skinInstance->skinData) {
			o_error = "not skinned";
			return false;
		}
		auto* partition = skinInstance->skinPartition.get();
		auto* skinData = skinInstance->skinData.get();
		const uint32_t vertexCount = partition->vertexCount;
		const uint32_t boneCount = skinData->GetBoneCount();
		if (vertexCount == 0 || partition->numPartitions == 0 || boneCount == 0) {
			o_error = "empty skin partition";
			return false;
		}

		// All partitions share one vertex buffer; find the first that kept its CPU copy.
		const RE::NiSkinPartition::Partition* dataPartition = nullptr;
		for (uint32_t p = 0; p < partition->numPartitions; ++p) {
			const auto& part = partition->partitions[p];
			if (part.buffData && part.buffData->rawVertexData) {
				dataPartition = &part;
				break;
			}
		}
		if (!dataPartition) {
			o_error = "no CPU vertex data";
			return false;
		}

		const auto layout = GetLayout(dataPartition->vertexDesc);
		if (layout.uv < 0 || layout.skinning < 0 || layout.stride == 0) {
			o_error = "vertex data has no UV or skinning";
			return false;
		}

		o_mesh.positions.resize(vertexCount);
		o_mesh.uvs.resize(vertexCount);
		o_mesh.boneIndices.assign(vertexCount, { 0, 0, 0, 0 });
		o_mesh.boneWeights.assign(vertexCount, { 0.0f, 0.0f, 0.0f, 0.0f });
		if (layout.normal >= 0)
			o_mesh.normals.resize(vertexCount);

		// Dynamic shapes (head parts) keep positions outside the partition data.
		auto* dynamicShape = netimmerse_cast<RE::BSDynamicTriShape*>(a_geometry);
		if (layout.position < 0) {
			if (!dynamicShape) {
				o_error = "vertex data has no positions";
				return false;
			}
			auto& dynamicData = dynamicShape->GetDynamicTrishapeRuntimeData();
			RE::BSSpinLockGuard guard(dynamicData.lock);
			if (!dynamicData.dynamicData || dynamicData.dataSize < vertexCount * sizeof(float4)) {
				o_error = "dynamic positions missing";
				return false;
			}
			const auto* positions = static_cast<const float4*>(dynamicData.dynamicData);
			for (uint32_t v = 0; v < vertexCount; ++v)
				o_mesh.positions[v] = { positions[v].x, positions[v].y, positions[v].z };
		}

		const uint8_t* raw = dataPartition->buffData->rawVertexData;
		for (uint32_t v = 0; v < vertexCount; ++v) {
			const uint8_t* vertex = raw + static_cast<size_t>(v) * layout.stride;
			if (layout.position >= 0) {
				const uint8_t* p = vertex + layout.position;
				if (layout.fullPrecision) {
					const auto* f = reinterpret_cast<const float*>(p);
					o_mesh.positions[v] = { f[0], f[1], f[2] };
				} else {
					o_mesh.positions[v] = { Half(p), Half(p + 2), Half(p + 4) };
				}
			}
			const uint8_t* uv = vertex + layout.uv;
			o_mesh.uvs[v] = { Half(uv), Half(uv + 2) };
			if (layout.normal >= 0) {
				const uint8_t* n = vertex + layout.normal;
				float3 normal{ UnpackByte(n[0]), UnpackByte(n[1]), UnpackByte(n[2]) };
				normal.Normalize();
				o_mesh.normals[v] = normal;
			}
		}

		// Triangles, and each vertex's partition (its bone indices are partition-local).
		std::vector<int32_t> vertexPartition(vertexCount, -1);
		for (uint32_t p = 0; p < partition->numPartitions; ++p) {
			const auto& part = partition->partitions[p];
			if (!part.triList || part.triangles == 0)
				continue;
			const uint32_t indexCount = part.triangles * 3u;
			uint32_t maxIndex = 0;
			for (uint32_t i = 0; i < indexCount; ++i)
				maxIndex = std::max<uint32_t>(maxIndex, part.triList[i]);
			// SSE keeps global indices; older-style partitions index their own vertex map.
			const bool localIndices = part.vertexMap && maxIndex < part.vertices && part.vertices < vertexCount;
			for (uint32_t i = 0; i < indexCount; ++i) {
				uint32_t index = part.triList[i];
				if (localIndices)
					index = part.vertexMap[index];
				if (index >= vertexCount) {
					o_error = "triangle index out of range";
					return false;
				}
				o_mesh.indices.push_back(index);
				if (vertexPartition[index] < 0)
					vertexPartition[index] = static_cast<int32_t>(p);
			}
		}
		if (o_mesh.indices.empty()) {
			o_error = "no triangles";
			return false;
		}

		for (uint32_t v = 0; v < vertexCount; ++v) {
			const int32_t p = vertexPartition[v];
			if (p < 0)
				continue;
			const auto& part = partition->partitions[p];
			const uint8_t* skin = raw + static_cast<size_t>(v) * layout.stride + layout.skinning;
			float total = 0.0f;
			for (int i = 0; i < 4; ++i) {
				const float weight = Half(skin + i * 2);
				const uint8_t local = skin[8 + i];
				if (weight <= 0.0f || local >= part.numBones || !part.bones)
					continue;
				const uint16_t bone = part.bones[local];
				if (bone >= boneCount)
					continue;
				o_mesh.boneIndices[v][i] = bone;
				o_mesh.boneWeights[v][i] = weight;
				total += weight;
			}
			if (total > 0.0f) {
				for (auto& weight : o_mesh.boneWeights[v])
					weight /= total;
			}
		}

		o_mesh.boneNames.resize(boneCount);
		o_mesh.boneBindPositions.resize(boneCount);
		for (uint32_t b = 0; b < boneCount; ++b) {
			if (skinInstance->bones && skinInstance->bones[b])
				o_mesh.boneNames[b] = skinInstance->bones[b]->name.c_str();
			const auto origin = skinData->GetBoneDataSkinToBone(b).Invert().translate;
			o_mesh.boneBindPositions[b] = { origin.x, origin.y, origin.z };
		}
		return true;
	}
}
