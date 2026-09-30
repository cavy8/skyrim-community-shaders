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
			uint32_t stride = 0;
		};

		VertexLayout GetLayout(const RE::BSGraphics::VertexDesc& a_desc)
		{
			VertexLayout layout;
			uint32_t offset = 0;
			if (a_desc.HasFlag(Vertex::VF_VERTEX)) {
				// SSE positions are always three floats plus the bitangent's x, whether or not
				// VF_FULLPREC is set (a Fallout 4 flag many SSE meshes carry, and many do not).
				layout.position = static_cast<int32_t>(offset);
				offset += 16;
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

		// How much of a texel's own colour the strands keep, by its alpha: painted hair keeps
		// all of it, and the soft edge of a lock (often painted over black) blends into its fill.
		constexpr float kColourAlphaLow = 0.1f;
		constexpr float kColourAlphaHigh = 0.6f;

		float ColourWeight(uint8_t a_alpha)
		{
			const float t = std::clamp((a_alpha / 255.0f - kColourAlphaLow) / (kColourAlphaHigh - kColourAlphaLow), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		// One level of the colour pyramid: (colour x weight, weight) per texel, or once filled,
		// (colour, 1).
		struct ColourLevel
		{
			uint32_t width = 0;
			uint32_t height = 0;
			std::vector<float4> texels;
		};

		ColourLevel Downsample(const ColourLevel& a_level)
		{
			ColourLevel half{ std::max(a_level.width / 2, 1u), std::max(a_level.height / 2, 1u), {} };
			half.texels.resize(static_cast<size_t>(half.width) * half.height);
			for (uint32_t y = 0; y < half.height; ++y) {
				for (uint32_t x = 0; x < half.width; ++x) {
					float4 sum;
					for (uint32_t dy = 0; dy < 2; ++dy) {
						for (uint32_t dx = 0; dx < 2; ++dx)
							sum += a_level.texels[static_cast<size_t>(std::min(y * 2 + dy, a_level.height - 1)) * a_level.width + std::min(x * 2 + dx, a_level.width - 1)];
					}
					half.texels[static_cast<size_t>(y) * half.width + x] = sum * 0.25f;
				}
			}
			return half;
		}

		// Bilinear, clamped, at a position in texels (texel centres at +0.5).
		float4 SampleLevel(const ColourLevel& a_level, float a_x, float a_y)
		{
			const float x = std::clamp(a_x - 0.5f, 0.0f, a_level.width - 1.0f);
			const float y = std::clamp(a_y - 0.5f, 0.0f, a_level.height - 1.0f);
			const auto x0 = static_cast<uint32_t>(x), y0 = static_cast<uint32_t>(y);
			const uint32_t x1 = std::min(x0 + 1, a_level.width - 1), y1 = std::min(y0 + 1, a_level.height - 1);
			const auto at = [&](uint32_t a_cx, uint32_t a_cy) { return a_level.texels[static_cast<size_t>(a_cy) * a_level.width + a_cx]; };
			return float4::Lerp(float4::Lerp(at(x0, y0), at(x1, y0), x - x0), float4::Lerp(at(x0, y1), at(x1, y1), x - x0), y - y0);
		}

		uint32_t PackColour(const float4& a_colour)
		{
			const auto channel = [](float a_value) { return static_cast<uint32_t>(std::lround(std::clamp(a_value, 0.0f, 1.0f) * 255.0f)); };
			return channel(a_colour.x) | (channel(a_colour.y) << 8) | (channel(a_colour.z) << 16) | 0xFF000000u;
		}

		// Push-pull: average the alpha-weighted colour down a pyramid, then, from the top, give
		// each level's missing weight the filled colour of the level above. Every level then has
		// the painted hair's colour everywhere and serves as that mip.
		void BuildStrandColour(const DirectX::Image& a_image, bool a_srgb, StrandColourImage& o_colour)
		{
			std::vector<ColourLevel> levels(1);
			auto& base = levels[0];
			base.width = static_cast<uint32_t>(a_image.width);
			base.height = static_cast<uint32_t>(a_image.height);
			base.texels.resize(static_cast<size_t>(base.width) * base.height);
			for (uint32_t y = 0; y < base.height; ++y) {
				const uint8_t* row = a_image.pixels + y * a_image.rowPitch;
				for (uint32_t x = 0; x < base.width; ++x) {
					const uint8_t* texel = row + x * 4;
					const float weight = ColourWeight(texel[3]);
					base.texels[static_cast<size_t>(y) * base.width + x] = float4(texel[0] / 255.0f * weight, texel[1] / 255.0f * weight, texel[2] / 255.0f * weight, weight);
				}
			}
			while (levels.back().width > 1 || levels.back().height > 1)
				levels.push_back(Downsample(levels.back()));

			float4& top = levels.back().texels[0];
			if (top.w <= 1e-6f)
				return;  // nothing painted
			top = float4(top.x / top.w, top.y / top.w, top.z / top.w, 1.0f);
			for (size_t k = levels.size() - 1; k-- > 0;) {
				auto& level = levels[k];
				const auto& above = levels[k + 1];
				const float scaleX = static_cast<float>(above.width) / level.width;
				const float scaleY = static_cast<float>(above.height) / level.height;
				for (uint32_t y = 0; y < level.height; ++y) {
					for (uint32_t x = 0; x < level.width; ++x) {
						float4& texel = level.texels[static_cast<size_t>(y) * level.width + x];
						const float4 fill = SampleLevel(above, (x + 0.5f) * scaleX, (y + 0.5f) * scaleY);
						const float missing = 1.0f - std::min(texel.w, 1.0f);
						texel = float4(texel.x + fill.x * missing, texel.y + fill.y * missing, texel.z + fill.z * missing, 1.0f);
					}
				}
			}

			o_colour.width = base.width;
			o_colour.height = base.height;
			o_colour.srgb = a_srgb;
			o_colour.mips.resize(levels.size());
			for (size_t k = 0; k < levels.size(); ++k) {
				o_colour.mips[k].resize(levels[k].texels.size());
				std::ranges::transform(levels[k].texels, o_colour.mips[k].begin(), PackColour);
			}
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
		// The descriptor's size nibble must agree, or every attribute would be read from the
		// wrong bytes and the strands would scatter.
		const auto rawDesc = std::bit_cast<uint64_t>(dataPartition->vertexDesc);
		const uint32_t declaredStride = static_cast<uint32_t>(rawDesc & 0xF) * 4;
		if (declaredStride != layout.stride) {
			o_error = std::format("unexpected vertex layout (flags {:#x}, stride {} but {} declared)", rawDesc >> 44, layout.stride, declaredStride);
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
				const auto* f = reinterpret_cast<const float*>(vertex + layout.position);
				o_mesh.positions[v] = { f[0], f[1], f[2] };
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

	namespace
	{
		const RE::BSLightingShaderMaterialBase* LightingMaterial(const RE::BSRenderPass* a_pass)
		{
			const auto* property = a_pass->shaderProperty;
			if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
				return nullptr;
			return static_cast<const RE::BSLightingShaderMaterialBase*>(static_cast<const RE::BSLightingShaderProperty*>(property)->material);
		}

		winrt::com_ptr<ID3D11Texture2D> Texture2D(const RE::NiSourceTexture* a_source)
		{
			winrt::com_ptr<ID3D11Texture2D> texture2D;
			const auto* texture = a_source ? a_source->rendererTexture : nullptr;
			if (texture && texture->texture)
				texture->texture->QueryInterface(IID_PPV_ARGS(texture2D.put()));
			return texture2D;
		}

		/** Copies the first mip no larger than kCoverageSize to a staging texture. */
		bool CopyToStaging(ID3D11Texture2D* a_texture, const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, CoverageReadback& o_readback, std::string& o_error)
		{
			// Block-compressed copies need whole blocks.
			const bool compressed = DirectX::IsCompressed(a_desc.Format);
			const auto mipSize = [&](uint32_t a_mip) { return std::pair{ std::max(a_desc.Width >> a_mip, 1u), std::max(a_desc.Height >> a_mip, 1u) }; };
			uint32_t mip = 0;
			while (mip + 1 < a_desc.MipLevels) {
				const auto [width, height] = mipSize(mip);
				const auto [nextWidth, nextHeight] = mipSize(mip + 1);
				if (std::max(width, height) <= kCoverageSize || (compressed && (nextWidth % 4 || nextHeight % 4)))
					break;
				++mip;
			}
			const auto [width, height] = mipSize(mip);
			if (compressed && (width % 4 || height % 4)) {
				o_error = "texture mip is not block aligned";
				return false;
			}

			D3D11_TEXTURE2D_DESC stagingDesc{};
			stagingDesc.Width = width;
			stagingDesc.Height = height;
			stagingDesc.MipLevels = 1;
			stagingDesc.ArraySize = 1;
			stagingDesc.Format = a_desc.Format;
			stagingDesc.SampleDesc.Count = 1;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(globals::d3d::device->CreateTexture2D(&stagingDesc, nullptr, o_readback.staging.put()))) {
				o_error = "cannot create the staging texture";
				return false;
			}
			Util::SetResourceName(o_readback.staging.get(), a_name);
			globals::d3d::context->CopySubresourceRegion(o_readback.staging.get(), 0, 0, 0, 0, a_texture, D3D11CalcSubresource(mip, 0, a_desc.MipLevels), nullptr);
			o_readback.format = a_desc.Format;
			o_readback.width = width;
			o_readback.height = height;
			return true;
		}

		/** Decodes a readback to RGBA8, as stored (an sRGB format only relabelled). */
		const DirectX::Image* DecodeRGBA8(const CoverageReadback& a_readback, DirectX::ScratchImage& o_scratch, std::string& o_error)
		{
			if (a_readback.bytes.empty()) {
				o_error = "no texture data";
				return nullptr;
			}
			DirectX::Image image{};
			image.width = a_readback.width;
			image.height = a_readback.height;
			image.format = DirectX::MakeLinear(a_readback.format);
			image.rowPitch = a_readback.rowPitch;
			image.slicePitch = a_readback.slicePitch;
			image.pixels = const_cast<uint8_t*>(a_readback.bytes.data());

			constexpr DXGI_FORMAT kDecoded = DXGI_FORMAT_R8G8B8A8_UNORM;
			if (image.format == kDecoded) {
				if (FAILED(o_scratch.InitializeFromImage(image))) {
					o_error = "cannot copy the texture";
					return nullptr;
				}
			} else {
				const HRESULT hr = DirectX::IsCompressed(image.format) ? DirectX::Decompress(image, kDecoded, o_scratch) : DirectX::Convert(image, kDecoded, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, o_scratch);
				if (FAILED(hr)) {
					o_error = std::format("cannot decode texture format {}", static_cast<int>(a_readback.format));
					return nullptr;
				}
			}
			const DirectX::Image* decoded = o_scratch.GetImage(0, 0, 0);
			if (!decoded || !decoded->pixels) {
				o_error = "cannot decode the texture";
				return nullptr;
			}
			return decoded;
		}
	}

	bool BeginCoverageReadback(const RE::BSRenderPass* a_pass, CoverageReadback& o_readback, std::string& o_error)
	{
		o_readback = {};
		const auto* material = LightingMaterial(a_pass);
		if (!material) {
			o_error = "not a lighting shader";
			return false;
		}
		const auto texture2D = Texture2D(material->diffuseTexture.get());
		if (!texture2D) {
			o_error = "no 2D diffuse texture";
			return false;
		}
		D3D11_TEXTURE2D_DESC desc{};
		texture2D->GetDesc(&desc);
		if (!DirectX::HasAlpha(desc.Format)) {
			o_error = "diffuse texture has no alpha";
			return false;
		}
		return CopyToStaging(texture2D.get(), desc, "HairStrands::CoverageReadback", o_readback, o_error);
	}

	bool BeginFlowReadback(const RE::BSRenderPass* a_pass, CoverageReadback& o_readback, std::string& o_error)
	{
		// As Hair Specular reads it (Lighting.hlsl): the back-lighting slot of a material with
		// the back-lighting flag, larger than the engine's small default textures.
		o_readback = {};
		o_error.clear();
		const auto* material = LightingMaterial(a_pass);
		if (!material || !a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kBackLighting))
			return false;
		const auto texture2D = Texture2D(material->specularBackLightingTexture.get());
		if (!texture2D)
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		texture2D->GetDesc(&desc);
		if (desc.Width <= 32 || desc.Height <= 32)
			return false;
		return CopyToStaging(texture2D.get(), desc, "HairStrands::FlowReadback", o_readback, o_error);
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

	bool DecodeCoverage(const CoverageReadback& a_readback, CoverageMask& o_mask, StrandColourImage& o_colour, std::string& o_error)
	{
		o_mask = {};
		o_colour = {};
		DirectX::ScratchImage scratch;
		const DirectX::Image* decoded = DecodeRGBA8(a_readback, scratch, o_error);
		if (!decoded)
			return false;

		o_mask.width = static_cast<uint32_t>(decoded->width);
		o_mask.height = static_cast<uint32_t>(decoded->height);
		o_mask.alpha.resize(static_cast<size_t>(o_mask.width) * o_mask.height);
		o_mask.shade.resize(o_mask.alpha.size());
		for (uint32_t y = 0; y < o_mask.height; ++y) {
			const uint8_t* row = decoded->pixels + y * decoded->rowPitch;
			for (uint32_t x = 0; x < o_mask.width; ++x) {
				const uint8_t* texel = row + x * 4;
				const size_t i = static_cast<size_t>(y) * o_mask.width + x;
				o_mask.alpha[i] = texel[3];
				o_mask.shade[i] = static_cast<uint8_t>((texel[0] * 77u + texel[1] * 150u + texel[2] * 29u) * texel[3] / (256u * 255u));
			}
		}
		// The colour bytes are as stored (the format was only relabelled linear): the strand
		// texture takes the source's colour space so it samples the same.
		BuildStrandColour(*decoded, DirectX::IsSRGB(a_readback.format), o_colour);
		return true;
	}

	bool DecodeFlow(const CoverageReadback& a_readback, FlowMap& o_flow, std::string& o_error)
	{
		o_flow = {};
		DirectX::ScratchImage scratch;
		const DirectX::Image* decoded = DecodeRGBA8(a_readback, scratch, o_error);
		if (!decoded)
			return false;
		o_flow.width = static_cast<uint32_t>(decoded->width);
		o_flow.height = static_cast<uint32_t>(decoded->height);
		o_flow.rg.resize(static_cast<size_t>(o_flow.width) * o_flow.height * 2);
		for (uint32_t y = 0; y < o_flow.height; ++y) {
			const uint8_t* row = decoded->pixels + y * decoded->rowPitch;
			for (uint32_t x = 0; x < o_flow.width; ++x) {
				const size_t i = (static_cast<size_t>(y) * o_flow.width + x) * 2;
				o_flow.rg[i] = row[x * 4];
				o_flow.rg[i + 1] = row[x * 4 + 1];
			}
		}
		return true;
	}
}
