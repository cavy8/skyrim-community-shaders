#include "MeshExtract.h"

#include <DirectXTex.h>

#include "Globals.h"
#include "Utils/D3D.h"

namespace Strands
{
	namespace
	{
		// Mip size read back for the coverage mask: fine enough for the outline of each lock
		// and each tapered tip, small enough to copy and decode in a few milliseconds.
		constexpr uint32_t kCoverageSize = 512;

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

	bool BeginCoverageReadback(const RE::BSRenderPass* a_pass, CoverageReadback& o_readback, std::string& o_error)
	{
		o_readback = {};
		const auto* property = a_pass->shaderProperty;
		if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get()) {
			o_error = "not a lighting shader";
			return false;
		}
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(static_cast<const RE::BSLightingShaderProperty*>(property)->material);
		const auto* source = material ? material->diffuseTexture.get() : nullptr;
		const auto* texture = source ? source->rendererTexture : nullptr;
		if (!texture || !texture->texture) {
			o_error = "no diffuse texture";
			return false;
		}
		winrt::com_ptr<ID3D11Texture2D> texture2D;
		if (FAILED(texture->texture->QueryInterface(IID_PPV_ARGS(texture2D.put())))) {
			o_error = "diffuse texture is not 2D";
			return false;
		}
		D3D11_TEXTURE2D_DESC desc{};
		texture2D->GetDesc(&desc);
		if (!DirectX::HasAlpha(desc.Format)) {
			o_error = "diffuse texture has no alpha";
			return false;
		}

		// The first mip no larger than kCoverageSize. Block-compressed copies need whole blocks.
		const bool compressed = DirectX::IsCompressed(desc.Format);
		const auto mipSize = [&](uint32_t a_mip) { return std::pair{ std::max(desc.Width >> a_mip, 1u), std::max(desc.Height >> a_mip, 1u) }; };
		uint32_t mip = 0;
		while (mip + 1 < desc.MipLevels) {
			const auto [width, height] = mipSize(mip);
			const auto [nextWidth, nextHeight] = mipSize(mip + 1);
			if (std::max(width, height) <= kCoverageSize || (compressed && (nextWidth % 4 || nextHeight % 4)))
				break;
			++mip;
		}
		const auto [width, height] = mipSize(mip);
		if (compressed && (width % 4 || height % 4)) {
			o_error = "diffuse texture mip is not block aligned";
			return false;
		}

		D3D11_TEXTURE2D_DESC stagingDesc{};
		stagingDesc.Width = width;
		stagingDesc.Height = height;
		stagingDesc.MipLevels = 1;
		stagingDesc.ArraySize = 1;
		stagingDesc.Format = desc.Format;
		stagingDesc.SampleDesc.Count = 1;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(globals::d3d::device->CreateTexture2D(&stagingDesc, nullptr, o_readback.staging.put()))) {
			o_error = "cannot create the staging texture";
			return false;
		}
		Util::SetResourceName(o_readback.staging.get(), "HairStrands::CoverageReadback");
		globals::d3d::context->CopySubresourceRegion(o_readback.staging.get(), 0, 0, 0, 0, texture2D.get(), D3D11CalcSubresource(mip, 0, desc.MipLevels), nullptr);
		o_readback.format = desc.Format;
		o_readback.width = width;
		o_readback.height = height;
		return true;
	}

	ReadbackStatus PollCoverageReadback(CoverageReadback& io_readback)
	{
		if (!io_readback.staging)
			return ReadbackStatus::Failed;
		auto* context = globals::d3d::context;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT hr = context->Map(io_readback.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
			return ReadbackStatus::Pending;
		if (FAILED(hr)) {
			io_readback.staging = nullptr;
			return ReadbackStatus::Failed;
		}
		bool ok = SUCCEEDED(DirectX::ComputePitch(io_readback.format, io_readback.width, io_readback.height, io_readback.rowPitch, io_readback.slicePitch)) &&
		          io_readback.rowPitch > 0 && io_readback.rowPitch <= mapped.RowPitch;
		if (ok) {
			const size_t rows = io_readback.slicePitch / io_readback.rowPitch;
			io_readback.bytes.resize(io_readback.slicePitch);
			for (size_t row = 0; row < rows; ++row)
				std::memcpy(io_readback.bytes.data() + row * io_readback.rowPitch, static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch, io_readback.rowPitch);
		}
		context->Unmap(io_readback.staging.get(), 0);
		io_readback.staging = nullptr;
		return ok ? ReadbackStatus::Done : ReadbackStatus::Failed;
	}

	bool DecodeCoverage(const CoverageReadback& a_readback, CoverageMask& o_mask, std::string& o_error)
	{
		o_mask = {};
		if (a_readback.bytes.empty()) {
			o_error = "no texture data";
			return false;
		}
		DirectX::Image image{};
		image.width = a_readback.width;
		image.height = a_readback.height;
		image.format = DirectX::MakeLinear(a_readback.format);  // alpha is linear either way
		image.rowPitch = a_readback.rowPitch;
		image.slicePitch = a_readback.slicePitch;
		image.pixels = const_cast<uint8_t*>(a_readback.bytes.data());

		constexpr DXGI_FORMAT kDecoded = DXGI_FORMAT_R8G8B8A8_UNORM;
		DirectX::ScratchImage scratch;
		const DirectX::Image* decoded = &image;
		if (image.format != kDecoded) {
			const HRESULT hr = DirectX::IsCompressed(image.format) ? DirectX::Decompress(image, kDecoded, scratch) : DirectX::Convert(image, kDecoded, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, scratch);
			decoded = SUCCEEDED(hr) ? scratch.GetImage(0, 0, 0) : nullptr;
		}
		if (!decoded || !decoded->pixels) {
			o_error = std::format("cannot decode texture format {}", static_cast<int>(a_readback.format));
			return false;
		}

		o_mask.width = static_cast<uint32_t>(decoded->width);
		o_mask.height = static_cast<uint32_t>(decoded->height);
		o_mask.alpha.resize(static_cast<size_t>(o_mask.width) * o_mask.height);
		for (uint32_t y = 0; y < o_mask.height; ++y) {
			const uint8_t* row = decoded->pixels + y * decoded->rowPitch;
			for (uint32_t x = 0; x < o_mask.width; ++x)
				o_mask.alpha[static_cast<size_t>(y) * o_mask.width + x] = row[x * 4 + 3];
		}
		return true;
	}
}
