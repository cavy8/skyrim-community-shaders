// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Truly Night Eye 1.0 by Aswordmaster.
#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "FeatureCategories.h"

/** @brief Adjustable Night Eye vision with eyeshine and actor highlighting, ported from Truly Night Eye. */
struct NightEye : Feature
{
	struct Settings
	{
		bool Enabled = true;
		float Exposure = 1.0f;
		float Gamma = 0.8f;
		float ShadowLift = 0.01f;
		std::array<float, 3> TintColor{ 0.55f, 0.75f, 1.0f };
		float TintAmount = 0.0f;
		float Glow = 0.0f;
		float Vignette = 0.0f;
		float Grain = 0.0f;
		float TransitionSeconds = 0.2f;
		float EyeshineAmount = 0.0f;
		std::array<float, 3> EyeshineColor{ 0.65f, 1.0f, 0.8f };
		bool CustomVFX = false;
		float CustomAmount = 1.0f;
		bool AnaglyphEnabled = false;
		float AnaglyphAmount = 1.0f;
		float AnaglyphHorizontal = 4.0f;
		float AnaglyphVertical = 0.0f;
		float AnaglyphDesaturation = 0.8f;
		std::array<float, 3> AnaglyphLeftColor{ 1.0f, 0.0f, 0.0f };
		std::array<float, 3> AnaglyphRightColor{ 0.0f, 1.0f, 1.0f };
		bool AnaglyphSwap = false;
		int VisionMode = 0;
		float ActorVisionAmount = 1.0f;
		float ActorVisionBrightness = 1.0f;
		std::array<float, 3> EnemyColor{ 1.0f, 0.03f, 0.01f };
		std::array<float, 3> AllyColor{ 0.02f, 1.0f, 0.05f };
		std::array<float, 3> NeutralColor{ 0.7f, 0.7f, 0.7f };
	};

	/** @brief Per-frame eyeshine and actor vision values read by the Lighting and Effect shaders. */
	struct alignas(16) GeometrySettings
	{
		float4 Eye;
		float4 Actor;
		float4 Enemy;
		float4 Ally;
		float4 Neutral;
	};
	STATIC_ASSERT_ALIGNAS_16(GeometrySettings);
	static_assert(sizeof(GeometrySettings) == 80);

	virtual inline std::string GetName() override { return "Night Eye"; }
	virtual std::string GetDisplayName() override { return T("feature.night_eye.name", "Night Eye"); }
	virtual inline std::string GetShortName() override { return "NightEye"; }
	virtual inline std::string_view GetShaderDefineName() override { return "NIGHT_EYE"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kPostProcessing; }

	/** @brief Returns true for the Lighting and Effect shaders, which draw eyeshine and actor vision. */
	virtual bool HasShaderDefine(RE::BSShader::Type shaderType) override
	{
		return shaderType == RE::BSShader::Type::Lighting || shaderType == RE::BSShader::Type::Effect;
	}

	/** @brief Returns the feature description and highlights shown in the UI. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.night_eye.description", "Replaces the Night Eye look with an adjustable vision effect"),
			{ T("feature.night_eye.key_feature_1", "Exposure, shadow gamma, shadow lift and transition speed"),
				T("feature.night_eye.key_feature_2", "Tint, glow, vignette and grain"),
				T("feature.night_eye.key_feature_3", "Eyeshine for human and creature eyes"),
				T("feature.night_eye.key_feature_4", "Anaglyph colour split and editable custom VFX"),
				T("feature.night_eye.key_feature_5", "Infrared and Predator actor vision") } };
	}

	/** @brief Creates the post-process states and compiles the Night Eye shaders. */
	virtual void SetupResources() override;
	/** @brief Resolves the Night Eye magic effects and reads the eyeshine mask table. */
	virtual void DataLoaded() override;
	/** @brief Clears the active state and actor relationships after a save loads. */
	virtual void GameLoaded() override;
	/** @brief Advances the Night Eye transition and schedules the active-effect query. */
	virtual void Reset() override;
	/** @brief Draws the Night Eye controls. */
	virtual void DrawSettings() override;
	/** @brief Serializes Night Eye settings. */
	virtual void SaveSettings(json& o_json) override;
	/** @brief Loads Night Eye settings and clamps them to their supported ranges. */
	virtual void LoadSettings(json& o_json) override;
	/** @brief Restores default Night Eye settings. */
	virtual void RestoreDefaultSettings() override;
	/** @brief Recompiles the Night Eye post-process shader. */
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;

	/** @brief Grades the finished scene in place, just before the interface draws, while a Night Eye effect is active. */
	void DrawBeforeInterface();
	/** @brief Notes that the world was post-processed this frame, so freeze-frame menus are not graded repeatedly. */
	void MarkWorldPostProcessed();
	/** @brief Tags a Lighting or Effect draw for eyeshine and actor vision, and binds its eyeshine mask. */
	void SetupGeometry(RE::BSRenderPass* a_pass);
	/** @brief Returns the per-frame eyeshine and actor vision values for the feature buffer. */
	GeometrySettings GetCommonBufferData() const;

	/** @brief Returns true while a Night Eye effect is active on the player. */
	bool IsEffectActive() const { return active.load(std::memory_order_acquire); }
	/** @brief Returns the current Night Eye blend, from 0 when inactive to 1 when fully faded in. */
	float GetActivation() const { return activation; }
	bool ReplacesImageSpaceModifier(const RE::TESImageSpaceModifier* a_modifier) const;
	bool IsVisionModifier(const RE::TESImageSpaceModifier* a_modifier) const;

	Settings settings;

private:
	struct EyeshineMask
	{
		std::string filename;
		std::vector<RE::BSFixedString> geometryNames;
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		bool matchLogged = false;
	};

	using Affiliations = std::unordered_map<RE::FormID, std::uint32_t>;

	void QueryGame(bool a_actorVision);
	bool PrepareShaders();
	bool PrepareInput(ID3D11Texture2D* a_output, ID3D11RenderTargetView* a_view);
	void RenderPostProcess(ID3D11Texture2D* a_texture, ID3D11RenderTargetView* a_view);
	void LoadEyeshineMaskTable();
	void LoadEyeshineMaskTextures();
	bool IsEyeGeometry(RE::BSRenderPass* a_pass);
	ID3D11ShaderResourceView* GetEyeshineMask(RE::BSRenderPass* a_pass);

	std::vector<const RE::EffectSetting*> effects;
	std::vector<const RE::TESImageSpaceModifier*> imageSpaceModifiers;
	std::vector<const RE::TESImageSpaceModifier*> visionModifiers;
	std::atomic<bool> postProcessReady{ false };
	std::atomic<bool> active{ false };
	std::atomic<bool> queryPending{ false };
	std::atomic<std::shared_ptr<const Affiliations>> affiliations;
	std::shared_ptr<const Affiliations> frameAffiliations;
	float activation = 0.0f;
	float elapsed = 0.0f;
	float queryElapsed = 0.0f;
	std::uint32_t lastDrawnFrame = UINT32_MAX;
	std::uint32_t worldPostProcessedFrame = UINT32_MAX;
	bool worldPostProcessSeen = false;

	winrt::com_ptr<ID3D11VertexShader> vertexShader;
	winrt::com_ptr<ID3D11PixelShader> pixelShader;
	winrt::com_ptr<ID3D11Texture2D> inputTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> inputView;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	eastl::unique_ptr<ConstantBuffer> parameters;
	D3D11_TEXTURE2D_DESC inputDescription{};
	DXGI_FORMAT inputFormat = DXGI_FORMAT_UNKNOWN;
	bool reloadShaders = true;
	bool shaderCompileFailed = false;

	std::vector<EyeshineMask> eyeshineMasks;
	std::unordered_map<std::string, std::size_t> eyeshineMaskAliases;
	std::unordered_map<RE::BSFixedString, std::size_t, RE::BSCRC32<RE::BSFixedString>> eyeshineMaskCache;
	std::unordered_map<RE::BSFixedString, bool, RE::BSCRC32<RE::BSFixedString>> eyeNames;
	bool eyeshineMasksLoaded = false;
	ID3D11ShaderResourceView* boundMask = nullptr;
	bool boundMaskValid = false;
};
