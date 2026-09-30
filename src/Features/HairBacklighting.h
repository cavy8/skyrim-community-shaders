#pragma once

#include "Feature.h"
#include "FeatureCategories.h"

/** @brief Adds light transmission through hair strands. */
struct HairBacklighting : Feature
{
	struct alignas(16) Settings
	{
		uint Enable = 1;
		float Strength = 2.0f;
		float ScatterWidth = 0.5f;
		float EdgeFalloff = 3.2f;
		float InteriorGlow = 0.0f;
		float Absorption = 0.9f;
		float DarkBoost = 4.0f;
		float DarkThreshold = 0.4f;
		float HeadOcclusion = 1.0f;
		uint pad[3]{};
	};
	STATIC_ASSERT_ALIGNAS_16(Settings);
	static_assert(sizeof(Settings) == 48);

	virtual inline std::string GetName() override { return "Hair Backlighting"; }
	virtual std::string GetDisplayName() override { return T("feature.hair_backlighting.name", "Hair Backlighting"); }
	virtual inline std::string GetShortName() override { return "HairBacklighting"; }
	virtual inline std::string_view GetShaderDefineName() override { return "HAIR_BACKLIGHTING"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }

	/** @brief Returns true for the shader that renders hair. */
	virtual bool HasShaderDefine(RE::BSShader::Type shaderType) override { return shaderType == RE::BSShader::Type::Lighting; }

	/** @brief Returns the feature description and highlights shown in the UI. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.hair_backlighting.description", "Allows bright light to shine through strands of hair"),
			{ T("feature.hair_backlighting.key_feature_1", "Bright light shines through hair strands"),
				T("feature.hair_backlighting.key_feature_2", "Glow is strongest along fine edges and loose strands"),
				T("feature.hair_backlighting.key_feature_3", "Glow takes on the hair color and works with any light source"),
				T("feature.hair_backlighting.key_feature_4", "Glow is brighter in dark surroundings"),
				T("feature.hair_backlighting.key_feature_5", "Glow is reduced where the head blocks the light") } };
	}

	/** @brief Draws the hair backlighting controls. */
	virtual void DrawSettings() override;
	/** @brief Serializes hair backlighting settings. */
	virtual void SaveSettings(json& o_json) override;
	/** @brief Loads hair backlighting settings and clamps them to the UI ranges. */
	virtual void LoadSettings(json& o_json) override;
	/** @brief Restores default hair backlighting settings. */
	virtual void RestoreDefaultSettings() override;

	Settings settings;
};
