#pragma once

#include "ExtendedEffect.h"

class ENBDepthOfField : public EffectBase
{
public:
	virtual std::string GetName() const override { return "enbdepthoffield.fx"; }

	virtual bool Apply() override;
	virtual void Execute() override;
	virtual void UpdateEffectVariables() override;

	ID3D11ShaderResourceView* GetApertureSRV() const;

protected:
	void CreateEffectTextures() override;

private:
	uint32_t idApertureTime = 0xFFFFFFFF;
	uint32_t idFocusingTime = 0xFFFFFFFF;
	uint32_t idEnableAdaptation = 0xFFFFFFFF;
	bool idsCached = false;
	bool historyValid = false;
	std::string fallbackTechnique;

	ID3D11ShaderResourceView* apertureSRV = nullptr;
	uint32_t apertureFrame = UINT32_MAX;
};
