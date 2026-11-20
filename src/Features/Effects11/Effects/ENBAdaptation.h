#pragma once

#include "ExtendedEffect.h"

class ENBAdaptation : public EffectBase
{
public:
	virtual std::string GetName() const override { return "enbadaptation.fx"; }

	virtual void Execute() override;
	virtual void UpdateEffectVariables() override;

	/** @brief The adaptation texture the latest Execute wrote, or nullptr if it did not write one. */
	ID3D11ShaderResourceView* GetOutputSRV() const;

	TextureManager::Texture textureCurrent;

protected:
	void CreateEffectTextures() override;

private:
	/** @brief Names of the adaptation ping-pong pair for this frame: (previous, output). */
	static std::pair<const char*, const char*> GetTextureNames();

	const char* outputTextureName = nullptr;

	uint32_t idForceMinMax = 0xFFFFFFFF;
	uint32_t idAdaptTime = 0xFFFFFFFF;
	uint32_t idAdaptMin = 0xFFFFFFFF;
	uint32_t idAdaptMax = 0xFFFFFFFF;
	uint32_t idAdaptSens = 0xFFFFFFFF;
	bool idsCached = false;
};
