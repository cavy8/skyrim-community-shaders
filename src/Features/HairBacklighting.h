#pragma once

#include "Feature.h"
#include "FeatureCategories.h"

/** @brief Glowing halo on hair lit from behind, the hair counterpart of Foliage Lighting's tree transmission. */
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

	/** @brief Returns true only for the Lighting shader, the only one that shades hair. */
	virtual bool HasShaderDefine(RE::BSShader::Type shaderType) override { return shaderType == RE::BSShader::Type::Lighting; }

	/** @brief Returns a description and list of key features for the UI summary. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.hair_backlighting.description", "Makes hair glow when it is lit from behind, like foliage against the sun."),
			{ T("feature.hair_backlighting.key_feature_1", "Light transmission through hair facing away from a light"),
				T("feature.hair_backlighting.key_feature_2", "Brightest on thin edges and loose strands"),
				T("feature.hair_backlighting.key_feature_3", "Tinted by the hair color; works with any light, with or without Hair Specular"),
				T("feature.hair_backlighting.key_feature_4", "Boosted in dark surroundings so firelight at night glows like the sun by day"),
				T("feature.hair_backlighting.key_feature_5", "Held back where the head blocks the light, so the scalp does not glow through it") } };
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
