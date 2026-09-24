#pragma once

#include "Feature.h"

struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;
struct ID3D11DepthStencilView;

namespace globals
{
	struct FrameBuffer;
}

struct ReverseZ : Feature
{
	virtual inline std::string GetName() override { return "Reverse Z-Buffer"; }
	virtual inline std::string GetShortName() override { return "ReverseZ"; }
	virtual inline std::string_view GetShaderDefineName() override { return "REVERSE_Z"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }
	virtual bool IsCore() const override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;

	struct Settings
	{
		bool EnableReverseZ = true;
	};

	Settings settings;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const override;
	virtual const void* GetSettingsBlob() const override { return &settings; }
	virtual size_t GetSettingsBlobSize() const override { return sizeof(settings); }

	virtual bool HasShaderDefine(RE::BSShader::Type) override;
	virtual bool ValidateCache(CSimpleIniA& a_ini) override;
	virtual void WriteDiskCacheInfo(CSimpleIniA& a_ini) override;

	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;

	void LatchBootState();
	void SetupDepthTargets();
	void InstallRuntimeHooks();
	void ApplyReverseProjection(void* a_cameraStateEntry, const RE::NiCamera* a_camera);
	void ReversePublishedProjection();
	void FixupMappedFrameBuffer(globals::FrameBuffer& a_frameBuffer);
	void TracePublishedCamera(const RE::NiCamera* a_camera);
	[[nodiscard]] bool IsConvertedDepthTarget(uint32_t a_target) const;
	[[nodiscard]] bool ExpectPublishedReversal(const RE::NiCamera* a_camera, bool a_renderingCubemap) const;

	[[nodiscard]] bool IsActive() const { return activeThisBoot; }

	static constexpr long kUtilityDefinesRevision = 1;

	[[nodiscard]] bool IsReverseDepthView(ID3D11DepthStencilView* a_view) const;
	[[nodiscard]] ID3D11DepthStencilView* ResolveCubemapFaceDepthView(ID3D11RenderTargetView* a_renderTarget, ID3D11DepthStencilView* a_depthView) const;
	[[nodiscard]] ID3D11DepthStencilState* GetReversedState(ID3D11DepthStencilState* a_state);
	[[nodiscard]] ID3D11RasterizerState* GetReversedRasterizerState(ID3D11RasterizerState* a_state);

private:
	bool bootLatched = false;
	bool activeThisBoot = false;
	bool depthTargetsConverted = false;
	bool runtimeHooksInstalled = false;
	uint32_t convertedTargetMask = 0;
};
