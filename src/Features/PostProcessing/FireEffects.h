#pragma once

#include "PostProcessFeature.h"

/** @brief Effects 11 [FIRE] and [LIGHTSPRITE] for the Post Processing pipeline: curve and intensity for additive flames and glows. */
struct FireEffects : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Fire"; }
	virtual inline std::string GetDisplayName() const override { return T("feature.post_processing.fire.name", "Fire and Light Sprites"); }
	virtual inline std::string GetDesc() const override { return T("feature.post_processing.fire.description", "Brightness and contrast of flames, spell bolts and glowing light sprites, sorted the way Effects 11 sorts them."); }
	virtual bool WritesToMainTexture() const override { return false; }

	struct Settings
	{
		float FireIntensity = 1.0f;
		float FireCurve = 1.0f;
		float LightSpriteIntensity = 1.0f;
		float LightSpriteCurve = 1.0f;
		float FireColorPreservation = 0.0f;
	} settings;

	virtual void SetupResources() override {}
	virtual void ClearShaderCache() override {}

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json&) override;
	virtual void SaveSettings(json&) override;

	virtual void DrawSettings() override;

	virtual void Draw(TextureInfo&) override {}
};
