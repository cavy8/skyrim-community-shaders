#pragma once

#include "Buffer.h"

struct Rainbows : Feature
{
	virtual inline std::string GetName() override { return "Rainbows"; }
	virtual std::string GetDisplayName() override { return T("feature.rainbows.name", "Rainbows"); }
	virtual inline std::string GetShortName() override { return "Rainbows"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSkyAndWeather; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.rainbows.description", "Paints rainbows into passing showers when the sun breaks through, opposite the sun as in real life."),
			{ T("feature.rainbows.key_feature_1", "Primary and secondary bows with the dark band between them, colored from real light dispersion in water drops"),
				T("feature.rainbows.key_feature_2", "Appear as showers pass and linger while the rain moves off, tinted red at sunset and faint white moonbows at night"),
				T("feature.rainbows.key_feature_3", "Form in the rain in front of hills and mountains, hidden by nearby objects and dimmed by fog"),
				T("feature.rainbows.key_feature_4", "No GPU cost outside sunlit showers") } };
	}

	static constexpr uint32_t kLutSize = 1024;
	static constexpr float kLutMaxDegrees = 64.0f;

	struct Settings
	{
		bool Enabled = true;
		bool Moonbows = true;
		float Strength = 1.0f;
		float SecondaryBow = 0.43f;
		float InnerGlow = 0.35f;
		float DropSize = 500.0f;
		float Chance = 0.75f;
		float Linger = 30.0f;
		float FogAbsorption = 0.7f;
	};

	struct alignas(16) FrameData
	{
		float4 FogParams;
		float2 RenderSize;
		float Intensity;
		float FogAbsorption;
		float MoonIntensity;
		float AngleToLut;
		float RainHeight;
		float RainExtent;
		float RainDensity;
		float RainStart;
		float pad[2];
	};
	static_assert(sizeof(FrameData) == 64);

	Settings settings;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual bool HasDebugSection() const override { return true; }
	virtual void DrawDebugSection() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	void Render(RE::BSShaderAccumulator* a_accumulator);

private:
	struct LutKey
	{
		float dropSize = -1.0f;
		float secondaryBow = -1.0f;
		float innerGlow = -1.0f;
		bool operator==(const LutKey&) const = default;
	};

	struct DebugInfo
	{
		float rain = 0.0f;
		float sunshine = 0.0f;
		uint64_t renderedFrames = 0;
	};

	void UpdateWeather(float a_deltaTime);
	void EndShower();
	void BuildLut();
	void Draw();

	ID3D11VertexShader* GetVS();
	ID3D11PixelShader* GetPS();

	// Shower state: the rain that can hold a bow, decaying in game time once it stops falling.
	float showerRain = 0.0f;
	float rainStart = 0.0f;
	bool showerActive = false;
	bool showerHasBow = false;
	float lastGameHours = -1.0f;
	float visibility = 0.0f;
	bool preview = false;
	std::mt19937 rng{ std::random_device{}() };

	LutKey lutKey{};
	uint32_t lastRenderedFrame = UINT32_MAX;
	DebugInfo debug;

	eastl::unique_ptr<Texture2D> lut;
	eastl::unique_ptr<ConstantBuffer> frameConstants;

	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	winrt::com_ptr<ID3D11VertexShader> vertexShader;
	winrt::com_ptr<ID3D11PixelShader> pixelShader;
	bool shaderCompileFailed = false;
};
