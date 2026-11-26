#pragma once

struct ProceduralNightSky : Feature
{
	struct Settings
	{
		bool enabled = true;
		float starIntensity = 1.0f;
		float starTwinkle = 0.25f;
		float riftIntensity = 1.0f;
		float auroraIntensity = 1.0f;
		float auroraSpeed = 1.0f;
		float auroraLighting = 1.0f;
		float auroraDirectLight = 1.0f;
		int auroraPalette = 0;
		float shootingStarRate = 2.0f;
		float shootingStarIntensity = 1.0f;
	};

	struct alignas(16) PerFrameData
	{
		uint enabled;
		float starIntensity;
		float starTwinkle;
		float radianceLimit;

		float3 auroraLowColor;
		float auroraIntensity;

		float3 auroraHighColor;
		float auroraTime;

		float auroraLighting;
		float riftIntensity;
		float2 pad = { 0.0f, 0.0f };

		float3 shootingStarHead;
		float shootingStarIntensity;

		float3 shootingStarTail;
		float shootingStarWidth;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);
	static_assert(sizeof(PerFrameData) == 96);

	Settings settings;

	virtual inline std::string GetName() override { return "Procedural Night Sky"; }
	virtual std::string GetDisplayName() override { return T("feature.procedural_night_sky.name", "Procedural Night Sky"); }
	virtual inline std::string GetShortName() override { return "ProceduralNightSky"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSkyAndWeather; }
	virtual inline std::string_view GetShaderDefineName() override { return "PROCEDURAL_NIGHT_SKY"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type shaderType) override { return shaderType == RE::BSShader::Type::Sky; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.procedural_night_sky.description", "Replaces the vanilla stars and aurora with procedural stars and animated aurora curtains."),
			{ T("feature.procedural_night_sky.key_feature_1", "Stars with natural brightness and color that stay sharp at any resolution"),
				T("feature.procedural_night_sky.key_feature_2", "Animated aurora curtains across the whole sky, following each weather's aurora colors and timing"),
				T("feature.procedural_night_sky.key_feature_3", "Follows vanilla star fading, moons, clouds and reflections"),
				T("feature.procedural_night_sky.key_feature_4", "Occasional shooting stars across clear night skies"),
				T("feature.procedural_night_sky.key_feature_5", "A lore-friendly Aetherius Rift: a band of light bleeding through the holes in the sky, torn by lanes of the void") }
		};
	}

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void Reset() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	PerFrameData GetCommonBufferData() const;

	/** @brief Draws the aurora from the transparency pass, the slot the vanilla mesh uses, so clouds never cover it. */
	void Render(RE::BSShaderAccumulator* a_accumulator);

	/** @brief Returns true while the aurora should add to the sky IBL. */
	bool HasAuroraLight() const;

	/**
	 * @brief Adds the aurora's glow to the weather sunlight colour.
	 * @param a_undimmed The weather's sunlight before the caster's phase and height dimmed it: the scale the glow is sized against.
	 */
	void ApplyAuroraSunlight(RE::Sky* a_sky, const RE::NiColor& a_undimmed) const;

	/**
	 * @brief The aurora's glow for a directional light that is set in working space; black when there is none.
	 * @param a_casterLight The light the caster gives right now.
	 * @param a_moonlight The brightest moon's light at full phase in this weather: the scale the glow is sized against.
	 */
	RE::NiColor GetAuroraWorkingLight(const RE::NiColor& a_casterLight, const RE::NiColor& a_moonlight) const;

private:
	struct ShootingStar
	{
		float3 start;
		float3 direction;
		float speed = 0.0f;
		float life = 0.0f;
		float age = 0.0f;
		float trail = 0.0f;
		float width = 0.0f;
		float brightness = 0.0f;
	};

	static float GetAuroraVisibility(const RE::Sky* a_sky);

	/** @brief The aurora's light colour at luminance 1, scaled by visibility and the direct light setting; black when there is none. */
	float3 GetAuroraLightColor(bool a_linear) const;

	void UpdateAuroraClock();
	void UpdateShootingStar();
	void SpawnShootingStar();
	float NextShootingStarRandom();

	ID3D11VertexShader* GetAuroraVS();
	ID3D11PixelShader* GetAuroraPS();
	void DrawAurora();

	bool shaderCompileFailed = false;
	uint32_t lastRenderedFrame = UINT32_MAX;
	double auroraClock = 0.0;

	ShootingStar shootingStar;
	float shootingStarTimer = 20.0f;
	uint32_t shootingStarSeed = 0x6D2B79F5u;

	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11VertexShader> auroraVS;
	winrt::com_ptr<ID3D11PixelShader> auroraPS;
};
