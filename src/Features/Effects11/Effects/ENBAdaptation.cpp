#include "ENBAdaptation.h"

#include "../EffectManager.h"
#include "../SettingManager.h"
#include "../TextureManager.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

void ENBAdaptation::Execute()
{
	outputTextureName = nullptr;

	auto& textureManager = TextureManager::GetSingleton();

	auto* currentSRV = textureManager.GetDownsampleTextureBlurry();
	if (!currentSRV) {
		return;
	}

	SetShaderResourceVariable("TextureCurrent", currentSRV);

	if (!textureCurrent.texture)
		return;

	ExecuteTechnique("Downsample", textureCurrent);

	SetShaderResourceVariable("TextureCurrent", textureCurrent.srv.get());

	const auto [texturePreviousName, textureAdaptationName] = GetTextureNames();

	// Set input texture (previous frame's adaptation value)
	auto* texturePrevious = textureManager.GetCommonTexture(texturePreviousName);
	if (!texturePrevious) {
		return;
	}
	SetShaderResourceVariable("TexturePrevious", texturePrevious->srv.get());

	// Execute adaptation technique, writing to output texture
	auto* textureAdaptation = textureManager.GetCommonTexture(textureAdaptationName);
	if (!textureAdaptation) {
		return;
	}
	if (ExecuteTechnique("Draw", *textureAdaptation))
		outputTextureName = textureAdaptationName;
}

std::pair<const char*, const char*> ENBAdaptation::GetTextureNames()
{
	const bool swap = (TextureManager::GetSingleton().GetTextureSwap() & 1) != 0;
	return swap ? std::pair{ "TextureAdaptationSwap", "TextureAdaptation" } : std::pair{ "TextureAdaptation", "TextureAdaptationSwap" };
}

ID3D11ShaderResourceView* ENBAdaptation::GetOutputSRV() const
{
	if (!outputTextureName)
		return nullptr;
	auto* texture = TextureManager::GetSingleton().FindCommonTexture(outputTextureName);
	return texture ? texture->srv.get() : nullptr;
}

void ENBAdaptation::UpdateEffectVariables()
{
	auto& settingManager = SettingManager::GetSingleton();

	if (!idsCached) {
		idForceMinMax = settingManager.GetSettingID("ForceMinMaxValues", "ADAPTATION");
		idAdaptTime = settingManager.GetSettingID("AdaptationTime", "ADAPTATION");
		idAdaptMin = settingManager.GetSettingID("AdaptationMin", "ADAPTATION");
		idAdaptMax = settingManager.GetSettingID("AdaptationMax", "ADAPTATION");
		idAdaptSens = settingManager.GetSettingID("AdaptationSensitivity", "ADAPTATION");
		idsCached = true;
	}

	auto forceMinMaxValues = settingManager.GetValue<bool>(idForceMinMax);

	float adaptationTime = settingManager.GetValue<float>(idAdaptTime);
	float deltaTime = (globals::game::deltaTime) ? (*globals::game::deltaTime) : 0.0f;

	float4 adaptationParameters{};
	adaptationParameters.x = !forceMinMaxValues ? 0.0f : settingManager.GetValue<float>(idAdaptMin);
	adaptationParameters.y = !forceMinMaxValues ? 65535.0f : settingManager.GetValue<float>(idAdaptMax);
	adaptationParameters.z = settingManager.GetValue<float>(idAdaptSens);
	adaptationParameters.w = std::clamp((adaptationTime > 0.0f) ? (deltaTime / adaptationTime) : 1.0f, 0.0f, 1.0f);

	SetVectorVariable("AdaptationParameters", &adaptationParameters, sizeof(adaptationParameters));
}

void ENBAdaptation::CreateEffectTextures()
{
	textureCurrent = CreateTexture(16, 16, DXGI_FORMAT_R32_FLOAT, "ENBAdaptation::TextureCurrent");
}
