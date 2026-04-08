#include "NeckSeamFix.h"

#include <cctype>
#include <unordered_map>
#include "Deferred.h"
#include "ShaderCache.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NeckSeamFix::Settings,
	SearchRadius,
	DepthThreshold,
	BlendStrength,
	LateSearchRadius,
	LateBlendStrength)

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

	std::string GetLowerTextureHint(RE::BSShaderProperty* a_shaderProperty)
	{
		if (!a_shaderProperty)
			return {};

		if (auto* baseTexture = a_shaderProperty->GetBaseTexture()) {
			const char* rawName = baseTexture->name.c_str();
			if (rawName && rawName[0] != '\0')
				return ToLowerCopy(rawName);
		}

		auto* material = a_shaderProperty->GetBaseMaterial();
		if (!material || material->GetType() != RE::BSShaderMaterial::Type::kLighting)
			return {};

		auto* lightingMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(material);
		if (auto textureSet = lightingMaterial->GetTextureSet()) {
			if (const char* diffusePath = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse); diffusePath && diffusePath[0] != '\0')
				return ToLowerCopy(diffusePath);
		}

		if (lightingMaterial->diffuseTexture) {
			const char* rawName = lightingMaterial->diffuseTexture->name.c_str();
			if (rawName && rawName[0] != '\0')
				return ToLowerCopy(rawName);
		}

		return {};
	}

	bool IsHeadGeometryName(std::string_view a_geometryName)
	{
		return a_geometryName.find("head") != std::string_view::npos;
	}

	void LogClassificationSample(bool a_isSkinned, bool a_isSkinCandidate, bool a_isHead, std::string_view a_geometryName, std::string_view a_textureHint)
	{
		static int loggedSamples = 0;
		if (loggedSamples >= 40)
			return;

		++loggedSamples;
		logger::info("[Neck Seam Fix] skinned={} skin={} head={} geom='{}' tex='{}'",
			a_isSkinned ? 1 : 0,
			a_isSkinCandidate ? 1 : 0,
			a_isHead ? 1 : 0,
			a_geometryName.empty() ? "<unnamed>" : a_geometryName,
			a_textureHint.empty() ? "<none>" : a_textureHint);
	}

	uint16_t GetGeometryMaskId(const RE::BSGeometry* a_geometry)
	{
		static std::unordered_map<const RE::BSGeometry*, uint16_t> geometryIds;
		static uint16_t nextId = 1;

		auto [it, inserted] = geometryIds.try_emplace(a_geometry, nextId);
		if (inserted) {
			++nextId;
			if (nextId == 0)
				nextId = 1;
		}

		return it->second;
	}
}

void NeckSeamFix::ReleaseRenderResources()
{
	seamOutputsValid = false;

	delete seamMainTexture;
	seamMainTexture = nullptr;

	delete seamAlbedoTexture;
	seamAlbedoTexture = nullptr;

	delete seamSpecularTexture;
	seamSpecularTexture = nullptr;

	delete seamReflectanceTexture;
	seamReflectanceTexture = nullptr;

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
	auto& specular = runtimeData.renderTargets[SPECULAR];
	auto& reflectance = runtimeData.renderTargets[REFLECTANCE];
	auto& normalRoughness = runtimeData.renderTargets[NORMALROUGHNESS];
	auto& masks = runtimeData.renderTargets[MASKS];
	auto& labels = runtimeData.renderTargets[LABELS_RENDER_TARGET];

	if (!main.texture || !main.SRV || !main.UAV || !albedo.texture || !albedo.SRV || !specular.texture || !specular.SRV || !reflectance.texture || !reflectance.SRV || !normalRoughness.texture || !normalRoughness.SRV || !masks.texture || !masks.SRV || !labels.texture || !labels.SRV)
		return false;

	D3D11_TEXTURE2D_DESC mainDesc{};
	main.texture->GetDesc(&mainDesc);
	D3D11_TEXTURE2D_DESC albedoDesc{};
	albedo.texture->GetDesc(&albedoDesc);
	D3D11_TEXTURE2D_DESC specularDesc{};
	specular.texture->GetDesc(&specularDesc);
	D3D11_TEXTURE2D_DESC reflectanceDesc{};
	reflectance.texture->GetDesc(&reflectanceDesc);
	D3D11_TEXTURE2D_DESC normalDesc{};
	normalRoughness.texture->GetDesc(&normalDesc);
	D3D11_TEXTURE2D_DESC masksDesc{};
	masks.texture->GetDesc(&masksDesc);

	bool resourcesMatch =
		seamMainTexture && seamMainTexture->resource &&
		seamAlbedoTexture && seamAlbedoTexture->resource &&
		seamSpecularTexture && seamSpecularTexture->resource &&
		seamReflectanceTexture && seamReflectanceTexture->resource &&
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
		seamSpecularTexture->desc.Width == specularDesc.Width &&
		seamSpecularTexture->desc.Height == specularDesc.Height &&
		seamSpecularTexture->desc.Format == specularDesc.Format &&
		seamReflectanceTexture->desc.Width == reflectanceDesc.Width &&
		seamReflectanceTexture->desc.Height == reflectanceDesc.Height &&
		seamReflectanceTexture->desc.Format == reflectanceDesc.Format &&
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
	seamSpecularTexture = createRWTextureFromRT(specular);
	seamReflectanceTexture = createRWTextureFromRT(reflectance);
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
				"Maximum pixel radius searched around actor-skin seams.\n"
				"Higher values catch wider gaps and wider blend zones.");
		}

		ImGui::SliderFloat("Depth Threshold", &settings.DepthThreshold, 0.001f, 0.05f, "%.4f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Maximum linearised depth difference allowed between the two\n"
				"visible actor-skin surfaces around the seam.");
		}

		ImGui::SliderFloat("Blend Strength", &settings.BlendStrength, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"How strongly seam-adjacent pixels are blended toward the\n"
				"opposing skin surface. 1.0 = strongest blend.");
		}

		ImGui::SliderFloat("Late Search Radius", &settings.LateSearchRadius, 1.0f, 8.0f, "%.1f px");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Maximum pixel radius searched by the post-composite\n"
				"color offset pass. Higher values create a wider tone fade.");
		}

		ImGui::SliderFloat("Late Blend Strength", &settings.LateBlendStrength, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"How strongly post-composite seam color offsets are applied.\n"
				"This preserves local detail and only shifts baseline tone.");
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
	if (!neckSeamPerGeometryCB)
		neckSeamPerGeometryCB = new ConstantBuffer(ConstantBufferDesc<NeckSeamPerGeometryCB>());

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
		cbData.LateSearchRadius = settings.LateSearchRadius;
		cbData.LateBlendStrength = settings.LateBlendStrength;
		neckSeamCB->Update(cbData);
	}

	// Bind constant buffer at slot 1  (slot 0 is reserved for PerFrame by convention)
	{
		ID3D11Buffer* cb[1] = { neckSeamCB->CB() };
		context->CSSetConstantBuffers(1, 1, cb);
	}

	auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
	auto specular = renderer->GetRuntimeData().renderTargets[SPECULAR];
	auto reflectance = renderer->GetRuntimeData().renderTargets[REFLECTANCE];
	auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto masks = renderer->GetRuntimeData().renderTargets[MASKS];
	auto labels = renderer->GetRuntimeData().renderTargets[LABELS_RENDER_TARGET];

	if (!labels.SRV)
		return;

	// SRV inputs
	{
		ID3D11ShaderResourceView* srvs[8]{
			Util::GetCurrentSceneDepthSRV(),  // t0 — raw depth
			masks.SRV,                        // t1 — material masks to blend through the seam
			labels.SRV,                       // t2 — actor-skin label texture
			main.SRV,                         // t3 — direct lighting / source color
			albedo.SRV,                       // t4 — albedo
			normalRoughness.SRV,              // t5 — encoded normal + gloss
			specular.SRV,                     // t6 — specular lighting
			reflectance.SRV,                  // t7 — reflectance / material lobes
		};
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	}

	// UAV outputs — seam-fixed buffers used by later passes.
	{
		ID3D11UnorderedAccessView* uavs[8] = {
			seamMainTexture->uav.get(),
			seamAlbedoTexture->uav.get(),
			seamNormalRoughnessTexture->uav.get(),
			seamMasksTexture->uav.get(),
			seamDepthTexture->uav.get(),
			seamDepthTexture16->uav.get(),
			seamSpecularTexture->uav.get(),
			seamReflectanceTexture->uav.get()
		};
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
	}

	context->CSSetShader(shader, nullptr, 0);

	auto dispatchCount = Util::GetScreenDispatchCount();
	context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

	// --- Unbind ---
	ID3D11Buffer* nullCB[1] = { nullptr };
	context->CSSetConstantBuffers(1, 1, nullCB);

	ID3D11ShaderResourceView* nullSRVs[8]{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(nullSRVs), nullSRVs);

	ID3D11UnorderedAccessView* nullUAVs[8]{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAVs), nullUAVs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	// Copy the seam-fixed direct lighting back into MAIN so downstream passes
	// read the corrected color while the G-buffer/depth use the dedicated seam outputs.
	context->CopyResource(main.texture, seamMainTexture->resource.get());

	seamOutputsValid = true;
}

void NeckSeamFix::DrawSeamFixLate()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Neck Seam Fix - Late Color");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	if (!seamOutputsValid || !neckSeamCB || !EnsureResources())
		return;

	auto shader = GetLateComputeShader();
	if (!shader)
		return;

	auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto labels = renderer->GetRuntimeData().renderTargets[LABELS_RENDER_TARGET];
	if (!main.texture || !main.SRV || !labels.SRV || !seamMainTexture || !seamMainTexture->uav)
		return;

	{
		NeckSeamCB cbData{};
		cbData.SearchRadius = settings.SearchRadius;
		cbData.DepthThreshold = settings.DepthThreshold;
		cbData.BlendStrength = settings.BlendStrength;
		cbData.LateSearchRadius = settings.LateSearchRadius;
		cbData.LateBlendStrength = settings.LateBlendStrength;
		neckSeamCB->Update(cbData);
	}

	// DeferredCompositeCS leaves kMAIN bound as a UAV. Unbind first so the late
	// pass can read final gamma-space MAIN and write the corrected scratch copy.
	ID3D11UnorderedAccessView* nullCompositeUAVs[3]{ nullptr, nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullCompositeUAVs), nullCompositeUAVs, nullptr);

	ID3D11ShaderResourceView* nullCompositeSRVs[17]{
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
		nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
	};
	context->CSSetShaderResources(0, ARRAYSIZE(nullCompositeSRVs), nullCompositeSRVs);

	ID3D11Buffer* cb[1] = { neckSeamCB->CB() };
	context->CSSetConstantBuffers(1, 1, cb);

	ID3D11ShaderResourceView* srvs[3]{
		seamDepthTexture && seamDepthTexture->srv ? seamDepthTexture->srv.get() : Util::GetCurrentSceneDepthSRV(),
		labels.SRV,
		main.SRV
	};
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	ID3D11UnorderedAccessView* uavs[1]{
		seamMainTexture->uav.get()
	};
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	context->CSSetShader(shader, nullptr, 0);

	auto dispatchCount = Util::GetScreenDispatchCount();
	context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

	ID3D11Buffer* nullCB[1] = { nullptr };
	context->CSSetConstantBuffers(1, 1, nullCB);

	ID3D11ShaderResourceView* nullSRVs[3]{ nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(nullSRVs), nullSRVs);

	ID3D11UnorderedAccessView* nullUAVs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAVs), nullUAVs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	context->CopyResource(main.texture, seamMainTexture->resource.get());
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

void NeckSeamFix::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	auto* state = globals::state;
	auto* deferred = globals::deferred;
	if (!state || !deferred || !deferred->deferredPass || !a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return;

	auto& extraDescriptor = state->permutationData.ExtraShaderDescriptor;
	extraDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::NeckSeamActorSkin);

	const bool isLightingShader = a_pass->shader && a_pass->shader->shaderType.get() == RE::BSShader::Type::Lighting;
	const bool isSkinned = a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kSkinned);
	const char* rawName = a_pass->geometry->name.c_str();
	const std::string loweredName = rawName && rawName[0] != '\0' ? ToLowerCopy(rawName) : std::string{};
	const bool isSkinCandidate = isLightingShader && isSkinned && a_pass->shaderProperty->flags.any(
		RE::BSShaderProperty::EShaderPropertyFlag::kFace,
		RE::BSShaderProperty::EShaderPropertyFlag::kFaceGenRGBTint);
	const bool isHeadCandidate = isSkinCandidate && IsHeadGeometryName(loweredName);

	uint32_t geometryMaskId = 0;
	if (isSkinCandidate) {
		geometryMaskId = GetGeometryMaskId(a_pass->geometry);
		extraDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::NeckSeamActorSkin);
	}

	if (isLightingShader && neckSeamPerGeometryCB) {
		NeckSeamPerGeometryCB cbData{};
		cbData.ObjectId = static_cast<float>(geometryMaskId);
		cbData.Flags = isHeadCandidate ? 1.0f : 0.0f;
		neckSeamPerGeometryCB->Update(cbData);

		ID3D11Buffer* buffer = neckSeamPerGeometryCB->CB();
		globals::d3d::context->PSSetConstantBuffers(7, 1, &buffer);
	}

	if (!isLightingShader || !isSkinned)
		return;

	if (a_pass->geometry) {
		const std::string textureHint = GetLowerTextureHint(a_pass->shaderProperty);
		LogClassificationSample(true, isSkinCandidate, isHeadCandidate, loweredName, textureHint);
	}

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

	if (neckSeamLateCS) {
		neckSeamLateCS->Release();
		neckSeamLateCS = nullptr;
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

ID3D11ComputeShader* NeckSeamFix::GetLateComputeShader()
{
	if (!neckSeamLateCS) {
		logger::debug("Compiling NeckSeamFixLateCS");
		neckSeamLateCS = static_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\NeckSeamFix\\NeckSeamFixLateCS.hlsl", {}, "cs_5_0"));
	}
	return neckSeamLateCS;
}

void NeckSeamFix::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
{
	globals::features::neckSeamFix.BSLightingShader_SetupGeometry(a_pass);
	func(a_shader, a_pass, a_renderFlags);
}
