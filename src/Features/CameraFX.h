#pragma once

#include "Buffer.h"
#include "CameraFX/CombatLens.h"
#include "CameraFX/Modifiers.h"
#include "CameraFX/WeatherLens.h"

struct CameraFX : Feature
{
	virtual inline std::string GetName() override { return "Camera FX"; }
	virtual std::string GetDisplayName() override { return T("feature.camera_fx.name", "Camera FX"); }
	virtual inline std::string GetShortName() override { return "CameraFX"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kPostProcessing; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.camera_fx.description", "Replaces the game's screen effects with lens and camera effects that stay correct with upscaling and frame generation."),
			{ T("feature.camera_fx.key_feature_1", "Takes over every imagespace modifier: damage, survival, poison, fades and the rest"),
				T("feature.camera_fx.key_feature_2", "Frost creeps over the lens as you freeze in Survival Mode"),
				T("feature.camera_fx.key_feature_3", "Low sun and full moons catch the lens, never through walls or terrain"),
				T("feature.camera_fx.key_feature_4", "Lens effects stay sharp on generated frames, under the HUD"),
				T("feature.camera_fx.key_feature_5", "Rain, snow, ash, fog and steam settle on the lens; blood, fire and frost from combat"),
				T("feature.camera_fx.key_feature_6", "No GPU cost while nothing is showing") } };
	}

	struct Settings
	{
		bool Enabled = false;
		bool ReplaceModifiers = true;
		bool ColdFrost = true;
		float FrostStrength = 1.0f;
		bool LowHealth = true;
		float LowHealthStrength = 1.0f;
		bool HeartbeatBlur = true;
		bool Exhaustion = true;
		float ExhaustionStrength = 1.0f;
		bool Hunger = true;
		float HungerStrength = 1.0f;
		bool Poison = true;
		float PoisonStrength = 1.0f;
		bool Transformation = true;
		float TransformationStrength = 1.0f;
		bool SunLens = true;
		float SunLensStrength = 1.0f;
		float LensDirt = 0.5f;
		float SunMaxHeight = 0.6f;
		bool MoonLens = true;
		float MoonLensStrength = 1.0f;
		bool RainDrops = true;
		float RainStrength = 1.0f;
		bool WaterSheet = true;
		bool WeatherFrost = true;
		float WeatherFrostStrength = 1.0f;
		bool ColdBreath = true;
		bool EntryFog = true;
		bool FogCondensation = true;
		bool Atmosphere = true;
		float AtmosphereStrength = 1.0f;
		bool Combat = true;
		bool Blood = true;
		float BloodStrength = 1.0f;
		bool BloodBlur = true;
		float BloodBlurStrength = 1.0f;
		bool ElementalSplats = true;
		bool ImpactFlash = true;
		float FlashStrength = 1.0f;
		bool SlashStreaks = true;
		bool HitShake = true;
		float HitShakeStrength = 1.0f;
		bool HitBlur = true;
		float HitBlurStrength = 1.0f;
		bool StaminaBreath = true;
		bool PhotosensitiveSafe = true;
	};

	struct FrameData
	{
		float4 Resolution;
		float4 Grade;
		float4 TintColor;
		float4 FadeColor;
		float4 Blur;
		float4 Radial;
		float4 Radial2;
		float4 DepthOfField;
		float4 DepthParams;
		float4 CameraData;
		float4 Themes;
		float4 ThemeParams;
		float4 Lens;
		float4 SunScreen;
		float4 SunColor;
		float4 MoonScreen[2];
		float4 Output;
		float4 Weather;
		float4 Weather2;
		float4 Weather3;
		float4 Wind;
		float4 Combat;
		float4 Combat2;
		float4 Combat3;
		float4 Combat4;
		float4 LensLook;
		float4 HitBlur;
		float4 Blurs;
		float4 Transform;
		float4 Splats[CameraFXLens::kMaxSplats * 2];
	};
	static_assert(sizeof(FrameData) % 16 == 0);

	struct VisibilityData
	{
		float4 Lights[3];
		float4 DepthParams;
		float4 DepthSize;
	};
	static_assert(sizeof(VisibilityData) % 16 == 0);

	Settings settings;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void Load() override;
	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;
	virtual void GameLoaded() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	bool TakesOverModifiers() const;
	void DrawBeforeInterface();
	void MarkWorldPostProcessed();

	struct SceneTarget
	{
		ID3D11Texture2D* texture = nullptr;
		ID3D11RenderTargetView* rtv = nullptr;
		bool hdr = false;
	};

	/** @brief The scene the interface draws over: HDR scene, frame generation buffer or framebuffer; empty if unusable. */
	SceneTarget GetSceneTarget() const;

private:
	struct LensPass
	{
		bool overlay = false;
		bool additive = false;
	};

	ID3D11RenderTargetView* GetLensTarget(const SceneTarget& a_scene, bool& o_uiLayer) const;
	bool EnsureSceneCopy(const D3D11_TEXTURE2D_DESC& a_desc);
	void EnsureLensTextures(bool a_needFrost, float a_deltaTime);
	void UpdateFrame(float a_deltaTime, bool a_hasDepth);
	void UpdateLights(RE::Sky* a_sky);
	void UpdateThemeClocks(float a_deltaTime);
	float UpdateLowHealth(float a_presence, float a_step);
	void ResetGameState();

	ID3D11VertexShader* GetVS();
	ID3D11PixelShader* GetScenePS();
	ID3D11PixelShader* GetLensPS();
	ID3D11PixelShader* GetCombatLensPS();
	ID3D11ComputeShader* GetVisibilityCS();
	ID3D11ComputeShader* GetBakeCS(bool a_frost);
	bool EnsurePipeline();
	float GetSceneEncoding(bool a_hdrTarget) const;

	std::array<bool, CameraFXModifiers::kThemeCount> GetEnabledThemes() const;
	static bool IsMenuSceneOpen();

	CameraFXLens::WeatherLens weather;
	CameraFXLens::CombatLens combat;
	CameraFXModifiers::Frame modifierFrame;
	CameraFXModifiers::Resolved resolved;

	FrameData frameData{};
	VisibilityData visibilityData{};
	bool sceneActive = false;
	bool lensActive = false;
	bool lightsActive = false;
	LensPass weatherPass;
	LensPass combatPass;
	bool sceneNeedsMips = false;
	bool lensNeedsMips = false;
	bool visibilityCleared = true;

	float time = 0.0f;
	float frost = 0.0f;
	float heartPhase = 0.0f;
	float lowHealthHoldSeconds = 0.0f;
	float lowHealthSeverity = 0.0f;
	RE::Setting* lowHealthIntervalSetting = nullptr;
	RE::Setting* heartbeatSlowSetting = nullptr;
	RE::Setting* heartbeatFastSetting = nullptr;
	float blinkTimer = 0.0f;
	float blinkAge = 99.0f;
	float hungerPhase = 0.0f;
	float beastPhase = 0.0f;
	float beastAge = 99.0f;
	float beastLevel = 0.0f;
	float beastAbsentSeconds = 99.0f;
	bool beastVampire = false;
	float sunAmount = 0.0f;
	std::array<float, 2> moonAmount{};
	std::array<float, CameraFXModifiers::kThemeCount> themeLevels{};
	float idleSeconds = 0.0f;
	float frostIdleSeconds = 0.0f;
	float frostAspect = 0.0f;
	float windOffset = 0.0f;
	uint32_t lastDrawnFrame = UINT32_MAX;
	uint32_t worldPostProcessedFrame = UINT32_MAX;
	bool worldPostProcessSeen = false;

	eastl::unique_ptr<Texture2D> sceneCopy;
	eastl::unique_ptr<Texture2D> visibility;
	eastl::unique_ptr<Texture2D> lensDirt;
	eastl::unique_ptr<Texture2D> frostMask;
	eastl::unique_ptr<ConstantBuffer> frameConstants;
	eastl::unique_ptr<ConstantBuffer> visibilityConstants;

	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11SamplerState> wrapSampler;
	winrt::com_ptr<ID3D11BlendState> opaqueBlend;
	winrt::com_ptr<ID3D11BlendState> lensSceneBlend;
	winrt::com_ptr<ID3D11BlendState> lensUIBlend;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	winrt::com_ptr<ID3D11VertexShader> vertexShader;
	winrt::com_ptr<ID3D11PixelShader> scenePixelShader;
	winrt::com_ptr<ID3D11PixelShader> lensPixelShader;
	winrt::com_ptr<ID3D11PixelShader> combatLensPixelShader;
	winrt::com_ptr<ID3D11ComputeShader> visibilityShader;
	winrt::com_ptr<ID3D11ComputeShader> dirtBakeShader;
	winrt::com_ptr<ID3D11ComputeShader> frostBakeShader;
	bool vertexShaderFailed = false;
	bool sceneShaderFailed = false;
	bool lensShaderFailed = false;
	bool combatLensShaderFailed = false;
	bool visibilityShaderFailed = false;
	bool dirtBakeShaderFailed = false;
	bool frostBakeShaderFailed = false;
	bool dirtBakeFailed = false;
	bool frostBakeFailed = false;
	bool pipelineReady = false;
	bool pipelineFailureLogged = false;
	bool sceneCopyFailed = false;
	UINT failedCopyWidth = 0;
	UINT failedCopyHeight = 0;
	DXGI_FORMAT failedCopyFormat = DXGI_FORMAT_UNKNOWN;
	bool sceneTargetAvailable = true;
	bool clearedAtMainMenu = false;
};
