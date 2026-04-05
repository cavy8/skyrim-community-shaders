#include "NeckSeamFix.h"

#include <array>
#include <cctype>

#include "Deferred.h"
#include "ShaderCache.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NeckSeamFix::Settings,
	SearchRadius,
	DepthThreshold,
	BlendStrength)

namespace
{
	std::string ToLowerCopy(std::string_view value)
	{
		std::string lowered;
		lowered.reserve(value.size());
		for (char c : value)
			lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		return lowered;
	}
}

void NeckSeamFix::ReleaseRenderResources()
{
	seamOutputsValid = false;

	delete seamMainTexture;
	seamMainTexture = nullptr;

	delete seamAlbedoTexture;
	seamAlbedoTexture = nullptr;

	delete seamNormalRoughnessTexture;
	seamNormalRoughnessTexture = nullptr;

	delete seamMasksTexture;
	seamMasksTexture = nullptr;

	delete seamDepthTexture;
	seamDepthTexture = nullptr;

	delete seamDepthTexture16;
	seamDepthTexture16 = nullptr;
}

bool NeckSeamFix::EnsureResources()
{
	auto renderer = globals::game::renderer;
	if (!renderer)
		return false;

	auto& runtimeData = renderer->GetRuntimeData();
	auto& main = runtimeData.renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& albedo = runtimeData.renderTargets[ALBEDO];
	auto& normalRoughness = runtimeData.renderTargets[NORMALROUGHNESS];
	auto& masks = runtimeData.renderTargets[MASKS];
	auto& labels = runtimeData.renderTargets[LABELS_RENDER_TARGET];

	if (!main.texture || !main.SRV || !main.UAV || !albedo.texture || !albedo.SRV || !normalRoughness.texture || !normalRoughness.SRV || !masks.texture || !masks.SRV || !labels.texture || !labels.SRV)
		return false;

	D3D11_TEXTURE2D_DESC mainDesc{};
	main.texture->GetDesc(&mainDesc);
	D3D11_TEXTURE2D_DESC albedoDesc{};
	albedo.texture->GetDesc(&albedoDesc);
	D3D11_TEXTURE2D_DESC normalDesc{};
	normalRoughness.texture->GetDesc(&normalDesc);
	D3D11_TEXTURE2D_DESC masksDesc{};
	masks.texture->GetDesc(&masksDesc);

	bool resourcesMatch =
		seamMainTexture && seamMainTexture->resource &&
		seamAlbedoTexture && seamAlbedoTexture->resource &&
		seamNormalRoughnessTexture && seamNormalRoughnessTexture->resource &&
		seamMasksTexture && seamMasksTexture->resource &&
		seamDepthTexture && seamDepthTexture->resource &&
		seamDepthTexture16 && seamDepthTexture16->resource &&
		seamMainTexture->desc.Width == mainDesc.Width &&
		seamMainTexture->desc.Height == mainDesc.Height &&
		seamMainTexture->desc.Format == mainDesc.Format &&
		seamAlbedoTexture->desc.Width == albedoDesc.Width &&
		seamAlbedoTexture->desc.Height == albedoDesc.Height &&
		seamAlbedoTexture->desc.Format == albedoDesc.Format &&
		seamNormalRoughnessTexture->desc.Width == normalDesc.Width &&
		seamNormalRoughnessTexture->desc.Height == normalDesc.Height &&
		seamNormalRoughnessTexture->desc.Format == normalDesc.Format &&
		seamMasksTexture->desc.Width == masksDesc.Width &&
		seamMasksTexture->desc.Height == masksDesc.Height &&
		seamMasksTexture->desc.Format == masksDesc.Format &&
		seamDepthTexture->desc.Width == mainDesc.Width &&
		seamDepthTexture->desc.Height == mainDesc.Height &&
		seamDepthTexture->desc.Format == DXGI_FORMAT_R32_FLOAT &&
		seamDepthTexture16->desc.Width == mainDesc.Width &&
		seamDepthTexture16->desc.Height == mainDesc.Height &&
		seamDepthTexture16->desc.Format == DXGI_FORMAT_R16_UNORM;

	if (resourcesMatch)
		return true;

	ReleaseRenderResources();

	auto createRWTextureFromRT = [](const auto& source) {
		D3D11_TEXTURE2D_DESC texDesc{};
		source.texture->GetDesc(&texDesc);
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		auto texture = new Texture2D(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		source.SRV->GetDesc(&srvDesc);
		texture->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		texture->CreateUAV(uavDesc);

		return texture;
	};

	seamMainTexture = createRWTextureFromRT(main);
	seamAlbedoTexture = createRWTextureFromRT(albedo);
	seamNormalRoughnessTexture = createRWTextureFromRT(normalRoughness);
	seamMasksTexture = createRWTextureFromRT(masks);

	D3D11_TEXTURE2D_DESC depthDesc = mainDesc;
	depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
	depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
	main.SRV->GetDesc(&depthSrvDesc);
	depthSrvDesc.Format = depthDesc.Format;

	D3D11_UNORDERED_ACCESS_VIEW_DESC depthUavDesc{};
	main.UAV->GetDesc(&depthUavDesc);
	depthUavDesc.Format = depthDesc.Format;

	seamDepthTexture = new Texture2D(depthDesc);
	seamDepthTexture->CreateSRV(depthSrvDesc);
	seamDepthTexture->CreateUAV(depthUavDesc);

	depthDesc.Format = DXGI_FORMAT_R16_UNORM;
	depthSrvDesc.Format = depthDesc.Format;
	depthUavDesc.Format = depthDesc.Format;

	seamDepthTexture16 = new Texture2D(depthDesc);
	seamDepthTexture16->CreateSRV(depthSrvDesc);
	seamDepthTexture16->CreateUAV(depthUavDesc);

	return true;
}

// =============================================================================
// DrawSettings
// =============================================================================

void NeckSeamFix::DrawSettings()
{
	if (ImGui::TreeNodeEx("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Search Radius", &settings.SearchRadius, 1.0f, 4.0f, "%.1f px");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Maximum pixel radius searched around body-to-skin seams.\n"
				"Higher values catch wider gaps and wider blend zones.");
		}

		ImGui::SliderFloat("Depth Threshold", &settings.DepthThreshold, 0.001f, 0.05f, "%.4f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Maximum linearised depth difference allowed between the body\n"
				"mesh and the neighbouring skin surface.");
		}

		ImGui::SliderFloat("Blend Strength", &settings.BlendStrength, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"How strongly seam-adjacent pixels are blended toward the\n"
				"opposing skin surface. 1.0 = strongest blend.");
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

// =============================================================================
// SetupResources
// =============================================================================

void NeckSeamFix::SetupResources()
{
	if (!neckSeamCB)
		neckSeamCB = new ConstantBuffer(ConstantBufferDesc<NeckSeamCB>());

	seamOutputsValid = false;
	ReleaseRenderResources();
}

// =============================================================================
// DrawSeamFix  (main render call)
// =============================================================================

void NeckSeamFix::DrawSeamFix()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Neck Seam Fix");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	seamOutputsValid = false;

	if (!neckSeamCB || !EnsureResources())
		return;

	auto shader = GetComputeShader();
	if (!shader)
		return;

	// Upload constant buffer
	{
		NeckSeamCB cbData{};
		cbData.SearchRadius = settings.SearchRadius;
		cbData.DepthThreshold = settings.DepthThreshold;
		cbData.BlendStrength = settings.BlendStrength;
		cbData.pad = 0.0f;
		neckSeamCB->Update(cbData);
	}

	// Bind constant buffer at slot 1  (slot 0 is reserved for PerFrame by convention)
	{
		ID3D11Buffer* cb[1] = { neckSeamCB->CB() };
		context->CSSetConstantBuffers(1, 1, cb);
	}

	auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
	auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto masks = renderer->GetRuntimeData().renderTargets[MASKS];
	auto labels = renderer->GetRuntimeData().renderTargets[LABELS_RENDER_TARGET];

	if (!labels.SRV)
		return;

	// SRV inputs
	{
		ID3D11ShaderResourceView* srvs[6]{
			Util::GetCurrentSceneDepthSRV(),  // t0 — raw depth
			masks.SRV,                        // t1 — MASKS (skin flag in .x)
			labels.SRV,                       // t2 — body mesh label texture
			main.SRV,                         // t3 — direct lighting / source color
			albedo.SRV,                       // t4 — albedo
			normalRoughness.SRV,              // t5 — encoded normal + gloss
		};
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	}

	// UAV outputs — seam-fixed buffers used by later passes.
	{
		ID3D11UnorderedAccessView* uavs[6] = {
			seamMainTexture->uav.get(),
			seamAlbedoTexture->uav.get(),
			seamNormalRoughnessTexture->uav.get(),
			seamMasksTexture->uav.get(),
			seamDepthTexture->uav.get(),
			seamDepthTexture16->uav.get()
		};
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
	}

	context->CSSetShader(shader, nullptr, 0);

	auto dispatchCount = Util::GetScreenDispatchCount();
	context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

	// --- Unbind ---
	ID3D11Buffer* nullCB[1] = { nullptr };
	context->CSSetConstantBuffers(1, 1, nullCB);

	ID3D11ShaderResourceView* nullSRVs[6]{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(nullSRVs), nullSRVs);

	ID3D11UnorderedAccessView* nullUAVs[6]{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAVs), nullUAVs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	// Copy the seam-fixed direct lighting back into MAIN so downstream passes
	// read the corrected color while the G-buffer/depth use the dedicated seam outputs.
	context->CopyResource(main.texture, seamMainTexture->resource.get());

	seamOutputsValid = true;
}

// =============================================================================
// Settings serialisation
// =============================================================================

void NeckSeamFix::LoadSettings(json& o_json)
{
	settings = o_json;
}

void NeckSeamFix::SaveSettings(json& o_json)
{
	o_json = settings;
}

void NeckSeamFix::RestoreDefaultSettings()
{
	settings = {};
}

void NeckSeamFix::PostPostLoad()
{
	Hooks::Install();
}

bool NeckSeamFix::IsTrackedBodyGeometry(const RE::BSGeometry* a_geometry) const
{
	if (!a_geometry)
		return false;

	static constexpr std::array<std::string_view, 8> trackedBodyNameParts{
		"femalebody_0",
		"femalebody_1",
		"malebody_0",
		"malebody_1",
		"femalebody_0.nif",
		"femalebody_1.nif",
		"malebody_0.nif",
		"malebody_1.nif"
	};

	const char* rawName = a_geometry->name.c_str();
	if (!rawName || rawName[0] == '\0')
		return false;

	const std::string loweredName = ToLowerCopy(rawName);
	for (auto needle : trackedBodyNameParts) {
		if (loweredName.find(needle) != std::string::npos)
			return true;
	}

	return false;
}

void NeckSeamFix::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	auto* state = globals::state;
	auto* deferred = globals::deferred;
	if (!state || !deferred || !deferred->deferredPass || !a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return;

	auto& extraDescriptor = state->permutationData.ExtraShaderDescriptor;
	extraDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::NeckSeamBody);

	const bool isLightingShader = a_pass->shader && a_pass->shader->shaderType.get() == RE::BSShader::Type::Lighting;
	const bool isFace = a_pass->shaderProperty->flags.any(
		RE::BSShaderProperty::EShaderPropertyFlag::kFace,
		RE::BSShaderProperty::EShaderPropertyFlag::kFaceGenRGBTint);

	if (!isLightingShader || isFace)
		return;

	if (IsTrackedBodyGeometry(a_pass->geometry))
		extraDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::NeckSeamBody);
}

// =============================================================================
// Shader cache
// =============================================================================

void NeckSeamFix::ClearShaderCache()
{
	seamOutputsValid = false;

	if (neckSeamCS) {
		neckSeamCS->Release();
		neckSeamCS = nullptr;
	}
}

ID3D11ComputeShader* NeckSeamFix::GetComputeShader()
{
	if (!neckSeamCS) {
		logger::debug("Compiling NeckSeamFixCS");
		neckSeamCS = static_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\NeckSeamFix\\NeckSeamFixCS.hlsl", {}, "cs_5_0"));
	}
	return neckSeamCS;
}

void NeckSeamFix::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
{
	globals::features::neckSeamFix.BSLightingShader_SetupGeometry(a_pass);
	func(a_shader, a_pass, a_renderFlags);
}
