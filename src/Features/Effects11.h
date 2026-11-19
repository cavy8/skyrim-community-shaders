#pragma once

#include "Buffer.h"
#include "Effects11/SceneHandoff.h"

#include <array>
#include <memory>
#include <winrt/base.h>

// C4324: the aligned PerFrame cache member pads the struct
#pragma warning(push)
#pragma warning(disable: 4324)

struct Effects11 : Feature
{
public:
	virtual inline std::string GetName() override { return "Effects11"; }
	virtual inline std::string GetShortName() override { return "Effects11"; }
	virtual inline std::string GetDisplayName() override { return "Effects 11"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kPostProcessing; }
	virtual inline std::string_view GetShaderDefineName() override { return "EFFECTS11"; }
	bool HasShaderDefine(RE::BSShader::Type) override;

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.effects11.description", "Effects 11 loads ENBSeries-compatible FX post-processing. Install multiple presets under Data/SKSE/Plugins/CommunityShaders/Effects11/Presets/<Name>/ (each with enbseries.ini + enbseries/), use a classic root/Data enbseries install as Legacy, or ship effects11/ inside a unified Presets pack. Hotswap does not modify root files."),
			{ T("feature.effects11.key_feature_1", "ENBSeries-compatible FX support"),
				T("feature.effects11.key_feature_2", "Multi-preset library with in-game hotswap"),
				T("feature.effects11.key_feature_3", "Legacy root/Data enbseries support"),
				T("feature.effects11.key_feature_4", "Advanced post-processing pipeline"),
				T("feature.effects11.key_feature_5", "Dynamic UI variable system") }
		};
	}

	struct alignas(16) PerFrame
	{
		uint Enable;
		float ColorPow;
		float LightSpriteIntensity;
		float FireIntensity;

		float FireCurve;
		float PBRColorPow;
		float pad0[2];

		float CloudsCurve;
		float CloudsDesaturation;
		float CloudsEdgeIntensity;
		float CloudsEdgeMoonMultiplier;

		uint UseProceduralGradientWeights;
		float ProceduralGradientWeightCurve;
		float LightSpriteCurve;
		uint EnableParticle;

		float ParticleIntensity;
		float ParticleLightingInfluence;
		float ParticleAmbientInfluence;
		float ParticlePointLightingInfluence;

		uint EnableVolumetricRays;
		float VolumetricRaysIntensity;
		float VolumetricRaysDensity;
		float VolumetricRaysSkyColorAmount;

		float VolumetricRaysDesaturation;
		float3 VolumetricRaysColorFilter;

		uint EnableWater;
		float WaterWavesAmplitude;
		float WaterMuddiness;
		float WaterSunLightingMultiplier;

		float WaterSunSpecularMultiplier;
		float WaterFresnelMin;
		float WaterFresnelMax;
		float WaterFresnelMultiplier;

		float WaterReflectionAmount;
		float WaterPad0;
		float WaterPad1;
		float WaterPad2;

		uint EnableCloudsScattering;
		float SkyScatteringIntensity;
		float SkyScatteringShadowAmount;
		float SkyScatteringAmount;

		float3 SkyScatteringColor;
		float SkyScatteringDustDarkening;

		float3 SkyScatteringDustTint;
		float SkyScatteringDustVolume;

		float3 SkyScatteringSunDirection;
		float SkyScatteringSunVisibility;

		float SkyScatteringHorizonRange;
		float SkyScatteringAtmosphereThickness;
		float SkyScatteringAirGlowIntensity;
		float SkyScatteringAirGlowRange;

		float SkyScatteringSunGlowIntensity;
		float SkyScatteringSunGlowRange;
		float SkyScatteringMoonGlowAmount;
		float SkyScatteringMoonGlowRange;

		float SkyScatteringSunIntensity;
		float CloudsLightingSunIntensity;
		float CloudsLightingMoonIntensity;
		uint EnableCloudsLightingFromMoon;

		uint CalculateCloudsEdgeFromScattering;
		float CloudsLightingDesaturation;
		float CloudsLightingForwardScattering;
		float CloudsLightingDensity;

		float3 CloudsColorFilter;
		float CloudsIntensity;

		float CloudsVertexAlphaBoost;
		float CloudsEdgeClamp;
		float CloudsEdgeFadePower;
		float SunBillboardTan;

		float MasserBillboardTan;
		float SecundaBillboardTan;
		float SkyScaleMultiplier = 1.0f;
		float SkyScatteringPad1;

		float3 VolumetricFogColorFilter;
		float VolumetricFogIntensity;

		float VolumetricFogCurve;
		float VolumetricFogOpacity;
		float VolumetricFogShadowAmount;
		uint VolumetricFogEnableLighting;

		float3 VolumetricRaysSkyColor;
		uint VolumetricFogEnableShadows;

		float StarsCurve;
		float StarsIntensity;
		float MoonCurve;
		uint EnableAnimatedStars;

		float StarsAnimationTime;
		float StarsAnimationDensity;
		float StarsAnimationIntensity;
		uint FixBlackCrush;

		uint VolumetricRaysSampleCount;
		uint GameVolumetricRaysDisableDithering;
		float2 GameVolumetricRaysPad0;

		float3 GameVolumetricRaysSunColor;
		float GameVolumetricRaysCustomContribution;

		float3 GameVolumetricRaysCustomColor;
		float GameVolumetricRaysPad1;
	};
	static_assert(sizeof(PerFrame) % 16 == 0);
	static_assert(offsetof(PerFrame, StarsCurve) % 16 == 0);
	static_assert(offsetof(PerFrame, VolumetricFogColorFilter) % 16 == 0);
	static_assert(offsetof(PerFrame, VolumetricRaysSkyColor) % 16 == 0);
	static_assert(offsetof(PerFrame, VolumetricRaysSampleCount) % 16 == 0);
	static_assert(offsetof(PerFrame, GameVolumetricRaysSunColor) % 16 == 0);
	static_assert(offsetof(PerFrame, GameVolumetricRaysCustomColor) % 16 == 0);
	static_assert(offsetof(PerFrame, EnableCloudsScattering) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringColor) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringDustTint) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringSunDirection) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringSunIntensity) % 16 == 0);
	static_assert(offsetof(PerFrame, CloudsColorFilter) % 16 == 0);
	static_assert(offsetof(PerFrame, MasserBillboardTan) % 16 == 0);

	struct Settings
	{
		bool IgnorePresetParticles = true;
		float PBRColorPowStrength = 0.0f;
	} settings;

	bool enableEffect = false;

	/** @brief Whether the active preset hands this feature back to CS through its manifest sceneControl. */
	bool IsHandedOff(E11Handoff::Feature feature) const { return handedOff[static_cast<size_t>(feature)]; }
	/** @brief Sets the runtime handoff flag; UnifiedPresetCatalog owns persisting it to the manifest. */
	void SetHandedOff(E11Handoff::Feature feature, bool value) { handedOff[static_cast<size_t>(feature)] = value; }
	/** @brief Whether Effects 11 drives a feature it can hand to CS. */
	bool OwnsFeature(E11Handoff::Feature feature) const { return loaded && enableEffect && !IsHandedOff(feature); }

	ID3D11PixelShader* raymarchVolumetricRaysPS = nullptr;
	ID3D11PixelShader* applyVolumetricRaysPS = nullptr;
	ID3D11ComputeShader* blurHCS = nullptr;
	ID3D11ComputeShader* blurVCS = nullptr;
	winrt::com_ptr<ID3D11BlendState> scatteringBlendState;

	std::unique_ptr<Texture2D> vlTexA;
	std::unique_ptr<Texture2D> vlTexB;
	std::unique_ptr<Texture2D> vlDepthHalf;
	std::unique_ptr<ConstantBuffer> vlBlurCB;

	ID3D11PixelShader* sunRaysMaskPS = nullptr;
	ID3D11PixelShader* sunRaysBlurPS = nullptr;
	ID3D11PixelShader* sunRaysCompositePS = nullptr;
	std::unique_ptr<Texture2D> sunRaysTexA;
	std::unique_ptr<Texture2D> sunRaysTexB;
	std::unique_ptr<ConstantBuffer> sunRaysCB;

	float3 scatteringSunColor = { 1.0f, 1.0f, 1.0f };
	float3 scatteringSunDirection = { 0.0f, 0.0f, 1.0f };
	float3 ungradedSunlight = { 0.0f, 0.0f, 0.0f };
	float3 gradedSunlight = { 0.0f, 0.0f, 0.0f };
	bool sunlightGraded = false;
	float3 volumetricSunColor = { 0.0f, 0.0f, 0.0f };

	bool gameVolumetricRaysIntensityApplied = false;
	float gameVolumetricRaysIntensityLast = 1.0f;
	float gameVolumetricRaysIntensityCurrent = 1.0f;
	float nightLightLast = 1.0f;
	float nightLightCurrent = 1.0f;

	PerFrame GetCommonBufferData();
	void UpdateSkyScattering(PerFrame& a_data);

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void Load() override;
	virtual void SetupResources() override;
	virtual void PostSetupResources() override;
	virtual void Reset() override;
	virtual void ClearShaderCache() override;

	/** @brief Switches between Effects 11 and the pipeline that ran before it; bound to the Effects 11 toggle hotkey. */
	void ToggleEnabled();
	/** @brief Writes the "UseEffect" GLOBAL setting; go through PostProcessingMode::Set to keep the pipelines exclusive. */
	void SetUseEffect(bool enabled);
	/** @brief UseEffect is on in enbseries.ini; does not require a compiled preset. */
	bool IsUseEffectEnabled() const;
	/** @brief UseEffect on and a compiled preset is ready (Effects 11 actually drives the frame). */
	bool IsPresetEnabled() const;
	bool IsActive() const { return presetActive; }

	void DrawVolumetricRays();

	/** @brief Draws the ENB [RAYS] screen-space shafts from the sun, or from a moon at night, additively onto the main target. */
	void DrawSunRays();

	void OnSkyUpdateColors(RE::Sky* a_sky);
	/** @brief Records the sunlight the game volumetric lighting should use: ungraded, but with the dims applied after grading. Runs after Sky Sync's dim. */
	void FinishSkyUpdateColors(RE::Sky* a_sky);
	/**
	 * @brief The [GAMEVOLUMETRICRAYS] Intensity of the last and current weather, if this tick multiplied it into the game volumetric lighting.
	 * @return False when Effects 11 left the intensity alone this tick.
	 */
	bool GetGameVolumetricRaysIntensityEndpoints(float& a_last, float& a_current) const;
	std::pair<float, float> GetNightLightEndpoints() const { return { nightLightLast, nightLightCurrent }; }
	void OverrideWeather(RE::Sky* a_sky);
	void CheckCommonData();
	void OverridePointLightColor(float3& a_color);

	struct DirectionalAmbientColors
	{
		RE::NiColor directionalAmbientColors[3][2];
	};
	void OverrideAmbientLighting(DirectionalAmbientColors& DirectionalAmbientColors);
	/**
	 * @brief During a transition between weathers with different presets, rescales the graded ambient cube towards
	 * the blend of each weather graded by its own preset values.
	 * @param a_sky The sky whose directional ambient cube a_graded was graded from.
	 * @param a_graded Ambient colours already graded at the blended preset values; corrected in place.
	 */
	void BlendGradedAmbient(RE::Sky* a_sky, DirectionalAmbientColors& a_graded);

	/**
	 * @brief Whether Effects11 wants to replace the vanilla tonemap this frame.
	 *
	 * Queried by State::GetTonemapOwner() to arbitrate against Post Processing. Does not
	 * render anything; refreshes per-frame common data as a side effect.
	 */
	bool WantsTonemapOwnership();

	/**
	 * @brief Runs the ENB effect chain in place of the vanilla tonemap pass.
	 * @param a_input Render target holding the scene color to tonemap.
	 * @param a_output Render target receiving the tonemapped result.
	 * @return True only if the chain wrote the output; false means the caller must fall
	 *         back to the vanilla pass.
	 */
	bool RenderTonemap(RE::RENDER_TARGET a_input, RE::RENDER_TARGET a_output);

	/** @brief True when the effect chain replaced ISHDR for the frame being presented, leaving an SDR scene for HDR Display to expand. */
	bool ReplacedTonemapperThisFrame() const;

private:
	bool EnsureScatteringBlendState();
	/**
	 * @brief Grades the weather colours in place with the current preset values.
	 * @return The sun scattering tint (scatteringSunColor unchanged when the effect is off).
	 */
	float3 GradeSkyColors(std::span<RE::NiColor, RE::TESWeather::ColorTypes::kTotal> a_colors) const;
	/** @brief Per-weather counterpart of BlendGradedAmbient for the sky colours graded by GradeSkyColors. */
	void BlendGradedWeathers(RE::Sky* a_sky, std::span<RE::NiColor, RE::TESWeather::ColorTypes::kTotal> a_graded);
	/** @brief Matches presetActive to IsPresetEnabled(), reloading the shader cache on a change when a_reloadShaders is set. */
	void SyncPresetActive(bool a_reloadShaders);
	bool presetActive = false;
	bool resourcesReady = false;
	/** @brief Compiles the sun ray shaders and (re)creates their half-res targets; false if anything failed. */
	bool EnsureSunRaysResources(uint32_t a_width, uint32_t a_height);

	uint tonemapReplacedFrame = UINT32_MAX;  ///< frameCount when the effect chain last wrote the tonemap output
	std::array<bool, static_cast<size_t>(E11Handoff::Feature::Count)> handedOff{};

	/** @brief Point light settings, resolved once per frame in CheckCommonData since OverridePointLightColor runs per light. */
	struct PointLightingParams
	{
		float curve = 1.0f;
		float desaturation = 0.0f;
		float intensity = 1.0f;
	} pointLighting;

	// The feature buffer is rebuilt several times per frame, so GetCommonBufferData's lookups are replayed from here
	PerFrame perFrameCache{};
	Util::FrameChecker perFrameCacheChecker;
};

#pragma warning(pop)
