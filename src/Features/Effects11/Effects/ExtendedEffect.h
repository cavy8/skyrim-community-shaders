#pragma once

#include "Effect.h"
#include "../UITree.h"

#include <span>

#ifdef ENABLE_ENB_EXTENDER

class ExtendedEffect : public Effect
{
public:
	void LoadWeatherData();
	void ApplyWeatherBlending(float blendFactor, uint32_t currentWeatherID, uint32_t lastWeatherID);
	void SyncWeatherVarFromUI(size_t index, uint32_t weatherID);
	void ApplyTimeOfDayInterpolation();
	void SaveWeatherOverrides() override;

	void Unload() override;
	bool IsTechniqueEnabled(TechniqueInfo& info) override;

	// Rendering
	void RenderImGui() override;
	static void RenderMergedUI(std::span<Effect*> effects, UITree::FilterMode filter = UITree::FilterMode::All);

private:
	using WeatherValues = std::unordered_map<std::string, std::string>;
	std::unordered_map<uint32_t, WeatherValues> weatherData;

	struct DirtyWeatherFile
	{
		uint32_t weatherID = 0;
		std::unordered_set<std::string> keys;
	};
	std::unordered_map<std::string, DirtyWeatherFile> dirtyWeatherFiles;

	std::unordered_map<std::string, int> bindingCache;

	int ResolveTechniqueBinding(const std::string& variableName);
	static float GetPeriodWeight(const std::string& period);
};

using EffectBase = ExtendedEffect;

#else

using EffectBase = Effect;

#endif
