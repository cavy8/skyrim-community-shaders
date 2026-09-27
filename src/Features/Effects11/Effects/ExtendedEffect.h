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
	/**
	 * @brief Draws the parameters of several effects as one annotation-ordered tree.
	 * @param options Search and time-period filters; also reports whether anything changed. May be null.
	 */
	static void RenderMergedUI(std::span<Effect*> effects, UITree::FilterMode filter = UITree::FilterMode::All, UITree::ViewOptions* options = nullptr);

private:
	using WeatherValues = std::unordered_map<std::string, std::string>;
	std::unordered_map<uint32_t, WeatherValues> weatherData;

	// Per-frame blending reads these instead of re-parsing the weather-file strings.
	// A weather-separated variable owns one slot; parsedWeatherData holds one entry per slot for
	// every weather ID in weatherData.
	struct WeatherVarSlot
	{
		size_t index = 0;  ///< Index into uiVariables
		std::string iniKey;
		int components = 1;            ///< 1 for Float, 2-4 for vectors
		bool perComponent = false;     ///< Vector stored as KeyX/KeyY/... keys
		bool exteriorWeather = false;  ///< separation == "ExteriorWeather"
	};
	struct ParsedWeatherValue
	{
		float values[4] = {};
		uint8_t definedMask = 0;  ///< Bit c set when component c was present and parsed
	};
	std::vector<WeatherVarSlot> weatherVarSlots;
	std::vector<int> weatherSlotOfVariable;  ///< uiVariables index -> slot, or -1
	std::unordered_map<uint32_t, std::vector<ParsedWeatherValue>> parsedWeatherData;
	ID3DX11Effect* weatherCacheEffect = nullptr;
	size_t weatherCacheVariableCount = 0;

	void EnsureWeatherCaches();
	void RebuildWeatherCaches();
	void ParseWeatherValue(const WeatherValues& values, const WeatherVarSlot& slot, ParsedWeatherValue& out) const;

	// Time-of-day variables grouped by base variable, built once per compiled effect
	struct TimeOfDayEntry
	{
		size_t index = 0;  ///< Index into uiVariables
		int period = -1;   ///< Index into the period weight table, -1 for unknown periods
	};
	struct TimeOfDayGroup
	{
		ID3DX11EffectVariable* baseVariable = nullptr;
		int components = 1;  ///< 1 for Float, 2-4 for vectors
		bool exteriorWeather = false;
		std::vector<TimeOfDayEntry> entries;
	};
	std::vector<TimeOfDayGroup> timeOfDayGroups;
	ID3DX11Effect* timeOfDayCacheEffect = nullptr;
	size_t timeOfDayCacheVariableCount = 0;

	void EnsureTimeOfDayGroups();
	void RebuildTimeOfDayGroups();
	static int GetPeriodIndex(const std::string& period);

	struct DirtyWeatherFile
	{
		uint32_t weatherID = 0;
		std::unordered_set<std::string> keys;
	};
	std::unordered_map<std::string, DirtyWeatherFile> dirtyWeatherFiles;

	std::unordered_map<std::string, int> bindingCache;

	int ResolveTechniqueBinding(const std::string& variableName);
};

using EffectBase = ExtendedEffect;

#else

using EffectBase = Effect;

#endif
