#pragma once

#include "Buffer.h"

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
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.effects11.description", "Effects 11 provides a framework for loading and executing ENBSeries-compatible FX effect files.\nThis allows for advanced post-processing effects and visual enhancements using DirectX 11 Effect (.fx) files."),
			{ T("feature.effects11.key_feature_1", "ENBSeries-compatible FX support"),
				T("feature.effects11.key_feature_2", "DirectX 11 Effect file loading"),
				T("feature.effects11.key_feature_3", "Advanced post-processing pipeline"),
				T("feature.effects11.key_feature_4", "Custom technique execution"),
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
		uint EnableRain;
		float RainMotionStretch;
		float RainMotionTransparency;

		float CloudsCurve;
		float CloudsDesaturation;
		float CloudsEdgeIntensity;
		float CloudsEdgeMoonMultiplier;

		uint UseProceduralGradientWeights;
		float ProceduralGradientWeightCurve;
		float LightSpriteCurve;
		float pad1;

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
		float SkyScatteringPad0;
		float SkyScatteringPad1;

		float3 VolumetricFogColorFilter;
		float VolumetricFogIntensity;

		float VolumetricFogCurve;
		float VolumetricFogOpacity;
		float VolumetricFogShadowAmount;
		uint VolumetricFogEnableLighting;

		float3 VolumetricRaysSkyColor;
		float VolumetricRaysPad0;
	};
	static_assert(sizeof(PerFrame) % 16 == 0);
	static_assert(offsetof(PerFrame, VolumetricFogColorFilter) % 16 == 0);
	static_assert(offsetof(PerFrame, VolumetricRaysSkyColor) % 16 == 0);
	static_assert(offsetof(PerFrame, EnableCloudsScattering) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringColor) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringDustTint) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringSunDirection) % 16 == 0);
	static_assert(offsetof(PerFrame, SkyScatteringSunIntensity) % 16 == 0);
	static_assert(offsetof(PerFrame, CloudsColorFilter) % 16 == 0);
	static_assert(offsetof(PerFrame, MasserBillboardTan) % 16 == 0);

	bool enableEffect = false;

	ID3D11PixelShader* raymarchVolumetricRaysPS = nullptr;
	ID3D11PixelShader* applyVolumetricRaysPS = nullptr;
	ID3D11ComputeShader* blurHCS = nullptr;
	ID3D11ComputeShader* blurVCS = nullptr;
	winrt::com_ptr<ID3D11BlendState> scatteringBlendState;
	winrt::com_ptr<ID3D11BlendState> alphaBlendState;

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

	winrt::com_ptr<ID3D11Texture2D> raindropTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> raindropSRV;
	std::string raindropStatus;
	void LoadRaindropTexture();

	PerFrame GetCommonBufferData();
	void UpdateSkyScattering(PerFrame& a_data);

	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void Prepass() override;
	virtual void ClearShaderCache() override;

	/** @brief Flips the "UseEffect" GLOBAL setting; bound to the Effects 11 toggle hotkey. */
	void ToggleEnabled();

	void DrawVolumetricRays();

	/** @brief Draws the ENB [RAYS] screen-space sun (or Masser) shafts additively onto the main target. */
	void DrawSunRays();

	void OnSkyUpdateColors(RE::Sky* a_sky);
	void OverrideWeather(RE::Sky* a_sky);
	void CheckCommonData();
	void OverridePointLightColor(float3& a_color);

	struct DirectionalAmbientColors
	{
		RE::NiColor directionalAmbientColors[3][2];
	};
	void OverrideAmbientLighting(DirectionalAmbientColors& DirectionalAmbientColors);

	__declspec(noinline) void ModifyParticle(RE::BSRenderPass* Pass);
	void ParticleShaderHacks();
	/** @brief True when the effect is on, the raindrop texture loaded, and RAIN "Enable" is set. */
	bool IsRainEnabled();

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

	/** @brief True when the effect chain replaced ISHDR this frame, leaving an SDR scene for HDR Display to expand. */
	bool ReplacedTonemapperThisFrame() const;

private:
	bool EnsureScatteringBlendState();
	bool EnsureSunRaysResources(uint32_t a_width, uint32_t a_height);

	uint tonemapReplacedFrame = UINT32_MAX;  ///< frameCount when the effect chain last wrote the tonemap output

	// The feature buffer is rebuilt several times per frame, so GetCommonBufferData's lookups are replayed from here
	PerFrame perFrameCache{};
	Util::FrameChecker perFrameCacheChecker;

	struct PointLightingParams
	{
		float curve = 1.0f;
		float desaturation = 0.0f;
		float intensity = 1.0f;
	} pointLighting;
};

#pragma warning(pop)
