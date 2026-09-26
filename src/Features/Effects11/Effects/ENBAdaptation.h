#pragma once

#include "ExtendedEffect.h"

class ENBAdaptation : public EffectBase
{
public:
	virtual std::string GetName() const override { return "enbadaptation.fx"; }

	virtual void Execute() override;
	virtual void UpdateEffectVariables() override;

	void ClearShaderCache();

	TextureManager::Texture textureCurrent;
	TextureManager::Texture textureSunMasked;

protected:
	void CreateEffectTextures() override;

private:
	struct alignas(16) SunMaskCB
	{
		Matrix CameraViewProj;
		Matrix CameraViewProjInverse;
		float4 SunDirection;
		float4 DynamicResolution;
		float4 MaskParams;
	};

	bool EnsureSunMaskResources();
	ID3D11ShaderResourceView* MaskProceduralSun(ID3D11ShaderResourceView* a_source);

	ID3D11PixelShader* sunMaskPS = nullptr;
	bool sunMaskPSFailed = false;
	winrt::com_ptr<ID3D11Buffer> sunMaskCB;
	winrt::com_ptr<ID3D11SamplerState> sunMaskSampler;

	uint32_t idForceMinMax = 0xFFFFFFFF;
	uint32_t idAdaptTime = 0xFFFFFFFF;
	uint32_t idAdaptMin = 0xFFFFFFFF;
	uint32_t idAdaptMax = 0xFFFFFFFF;
	uint32_t idAdaptSens = 0xFFFFFFFF;
	bool idsCached = false;
};
