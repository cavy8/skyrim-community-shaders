#pragma once

#include "Feature.h"
#include "FeatureCategories.h"
#include "Features/HairStrands/StrandRenderer.h"
#include "Features/HairStrands/StrandStyle.h"

/**
 * @brief Converts hair card meshes into rendered hair strands.
 *
 * Strands are generated from each hair mesh's texture flow (automatically, or from an
 * authored style), skinned on the GPU with the hair's own bones, and drawn inside the
 * hair's lighting pass with a strand variant of the hair's own Lighting shader, in place of
 * the hair's cards; the cards remain the fallback past the strand distance and budget.
 * See docs/development/hair-strands.md.
 */
struct HairStrands : Feature
{
	struct Settings
	{
		bool Enable = true;
		bool AutoConvert = true;
		bool PlayerOnly = false;
		uint MaxActors = 6;
		float DensityScale = 1.0f;
		float LodStart = 150.0f;
		float LodEnd = 600.0f;
		float MinStrandFraction = 0.15f;
		float MinPixelWidth = 0.8f;
		float MaxWidthScale = 4.0f;
		uint MaxSubdivisions = 4;
		uint MaxStrandsPerFrame = 200000;
		uint PhysicsMode = 2;  // 0 off, 1 simple (bone capsules for the body), 2 advanced (what the character wears)
		float PhysicsDistance = 400.0f;
		float SmpGuidance = 0.35f;
		float WindStrength = 1.0f;
		bool BodyCollision = true;
		bool CardCollision = true;
	};

	virtual inline std::string GetName() override { return "Hair Strands"; }
	virtual std::string GetDisplayName() override { return T("feature.hair_strands.name", "Hair Strands"); }
	virtual inline std::string GetShortName() override { return "HairStrands"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }

	/** @brief Returns a description and list of key features for the UI summary. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.hair_strands.description", "Turns hair card meshes into individually rendered, lit and animated hair strands."),
			{ T("feature.hair_strands.key_feature_1", "Automatic conversion that follows each hairstyle's texture flow"),
				T("feature.hair_strands.key_feature_2", "Straight, wavy, curly, coily and locs presets, tuned per hairstyle"),
				T("feature.hair_strands.key_feature_3", "In-game hairstyle editor; authored styles can replace automatic conversion"),
				T("feature.hair_strands.key_feature_4", "Follows the hair's bones and physics, and uses every hair lighting feature"),
				T("feature.hair_strands.key_feature_6", "TressFX 4.1 strand physics with gravity, inertia, wind and body collision, guided by SMP hair physics where present"),
				T("feature.hair_strands.key_feature_5", "Distance LOD, actor and strand budgets keep the cost bounded") } };
	}

	/** @brief Installs the Lighting and Utility shader SetupGeometry/RestoreGeometry hooks and loads the style files. */
	virtual void PostPostLoad() override;
	/** @brief Starts the frame: finishes generation jobs, evicts unused hair, assigns the budget. */
	virtual void Prepass() override;
	/** @brief Drops the compiled strand shaders so edited HLSL recompiles. */
	virtual void ClearShaderCache() override;

	/** @brief Draws the settings and the hairstyle editor. */
	virtual void DrawSettings() override;
	/** @brief Serializes Hair Strands settings. */
	virtual void SaveSettings(json& o_json) override;
	/** @brief Loads Hair Strands settings and clamps them to the UI ranges. */
	virtual void LoadSettings(json& o_json) override;
	/** @brief Restores default Hair Strands settings. */
	virtual void RestoreDefaultSettings() override;

	Settings settings;

private:
	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSLightingShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSUtilityShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSUtilityShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSEffectShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSEffectShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	Strands::RenderSettings MakeRenderSettings() const;
	void DrawPerformanceSettings();
	void DrawPhysicsSettings();
	void DrawStatistics();
	void DrawEditor();
	/** @brief Draws the style fields; returns true if a field changed, and whether it needs regeneration. */
	bool DrawStyleFields(Strands::StrandStyle& a_style, bool& o_regenerate);

	Strands::StyleLibrary library;
	std::unique_ptr<Strands::StrandRenderer> renderer;
	bool hooksInstalled = false;

	// Hairstyle editor.
	std::optional<Strands::HairKey> editorKey;
	Strands::StrandStyle editorStyle;
	bool editorReload = false;  // reload editorStyle once the hair re-resolves its style
};
