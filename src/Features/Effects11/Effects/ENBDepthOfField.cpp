#include "ENBDepthOfField.h"

#include "../SettingManager.h"
#include "../TextureManager.h"
#include "Globals.h"
#include "State.h"

void ENBDepthOfField::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto* renderer = globals::game::renderer;
	if (!renderer)
		return;

	auto& textureMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!textureMain.texture || !textureMain.SRV)
		return;

	auto* textureHDRTemp = textureManager.GetCommonTexture("TextureHDRTemp");
	auto* textureHDRTemp2 = textureManager.GetCommonTexture("TextureHDRTemp2");
	if (!textureHDRTemp || !textureHDRTemp2)
		return;

	const bool swap = (textureManager.GetTextureSwap() & 1) != 0;

	auto& textureApertureRead = effectTextureCache[swap ? "TextureApertureSwap" : "TextureAperture"];
	auto& textureApertureWrite = effectTextureCache[swap ? "TextureAperture" : "TextureApertureSwap"];
	auto& textureReadFocus = effectTextureCache["TextureReadFocus"];
	auto& textureFocusRead = effectTextureCache[swap ? "TextureFocusSwap" : "TextureFocus"];
	auto& textureFocusWrite = effectTextureCache[swap ? "TextureFocus" : "TextureFocusSwap"];

	if (!textureApertureRead.srv || !textureApertureWrite.rtv || !textureReadFocus.rtv ||
		!textureFocusRead.srv || !textureFocusWrite.rtv)
		return;

	SetShaderResourceVariable("TexturePrevious", textureApertureRead.srv.get());
	ExecuteTechnique("Aperture", textureApertureWrite);

	apertureSRV = textureApertureWrite.srv.get();
	apertureFrame = globals::state->frameCount;

	SetShaderResourceVariable("TextureAperture", textureApertureWrite.srv.get());
	ExecuteTechnique("ReadFocus", textureReadFocus);

	SetShaderResourceVariable("TexturePrevious", textureFocusRead.srv.get());
	SetShaderResourceVariable("TextureCurrent", textureReadFocus.srv.get());
	ExecuteTechnique("Focus", textureFocusWrite);

	SetShaderResourceVariable("TextureFocus", textureFocusWrite.srv.get());
	SetShaderResourceVariable("TextureOriginal", textureMain.SRV);

	[[maybe_unused]] auto [executed, inOutput, inTemp] = ExecuteTechniqueSequence(GetSelectedTechnique(), textureMain.SRV, *textureHDRTemp, *textureHDRTemp2);

	if (executed) {
		auto* result = inOutput ? textureHDRTemp : textureHDRTemp2;
		if (result->texture)
			globals::d3d::context->CopyResource(textureMain.texture, result->texture.get());
	}
}

void ENBDepthOfField::UpdateEffectVariables()
{
	auto& settingManager = SettingManager::GetSingleton();

	if (!idsCached) {
		idApertureTime = settingManager.GetSettingID("ApertureTime", "DEPTHOFFIELD");
		idFocusingTime = settingManager.GetSettingID("FocusingTime", "DEPTHOFFIELD");
		idEnableAdaptation = settingManager.GetSettingID("EnableAdaptation", "EFFECT");
		idsCached = true;
	}

	ID3D11ShaderResourceView* adaptationSRV = nullptr;
	if (idEnableAdaptation != 0xFFFFFFFF && settingManager.GetValue<bool>(idEnableAdaptation)) {
		const char* previousAdaptation = (TextureManager::GetSingleton().GetTextureSwap() & 1) ? "TextureAdaptationSwap" : "TextureAdaptation";
		auto* texture = GetCachedCommonTexture(previousAdaptation);
		adaptationSRV = texture ? texture->srv.get() : nullptr;
	}
	SetShaderResourceVariable("TextureAdaptation", adaptationSRV);

	const float deltaTime = globals::game::deltaTime ? (*globals::game::deltaTime) : 0.0f;
	const float apertureTime = settingManager.GetValue<float>(idApertureTime);
	const float focusingTime = settingManager.GetValue<float>(idFocusingTime);

	float4 dofParameters{};
	dofParameters.z = std::clamp((apertureTime > 0.0f) ? (deltaTime / apertureTime) : 1.0f, 0.0f, 1.0f);
	dofParameters.w = std::clamp((focusingTime > 0.0f) ? (deltaTime / focusingTime) : 1.0f, 0.0f, 1.0f);

	SetVectorVariable("DofParameters", &dofParameters, sizeof(dofParameters));
}

ID3D11ShaderResourceView* ENBDepthOfField::GetApertureSRV() const
{
	return apertureFrame == globals::state->frameCount ? apertureSRV : nullptr;
}

void ENBDepthOfField::CreateEffectTextures()
{
	effectTextureCache["TextureAperture"] = CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, "ENBDepthOfField::TextureAperture");
	effectTextureCache["TextureApertureSwap"] = CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, "ENBDepthOfField::TextureApertureSwap");
	effectTextureCache["TextureReadFocus"] = CreateTexture(16, 16, DXGI_FORMAT_R32_FLOAT, "ENBDepthOfField::TextureReadFocus");
	effectTextureCache["TextureFocus"] = CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, "ENBDepthOfField::TextureFocus");
	effectTextureCache["TextureFocusSwap"] = CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, "ENBDepthOfField::TextureFocusSwap");
}
