#include "NativeMenu/NativeMenu.h"

#include "Features/DynamicCubemaps.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceReflections.h"
#include "Globals.h"
#include "I18n/I18n.h"

#include <algorithm>
#include <array>

#define I18N_KEY_PREFIX "feature.screen_space_gi."

namespace
{
	using Settings = ScreenSpaceGI::Settings;

	struct SSGIRoot
	{
		static Settings& Live() { return globals::features::screenSpaceGI.settings; }
		static Settings Defaults() { return Settings{}; }
	};

	struct SSGIPreset
	{
		uint32_t numSteps;
		bool halfRes;
		bool quarterRes;
		bool enableGI;
		bool enableSH;
	};

	constexpr std::array<SSGIPreset, 5> kSSGIPresets{ {
		{ 6, true, false, false, false },
		{ 6, false, true, true, false },
		{ 8, true, false, true, true },
		{ 12, false, false, true, true },
		{ 16, false, false, true, true },
	} };
	constexpr size_t kSSGICustomIndex = kSSGIPresets.size();

	size_t FindSSGIPresetIndex(const Settings& s)
	{
		for (size_t i = 0; i < kSSGIPresets.size(); ++i) {
			const auto& p = kSSGIPresets[i];
			if (s.NumSteps == p.numSteps && s.HalfRes == p.halfRes && s.QuarterRes == p.quarterRes &&
				s.EnableGI == p.enableGI && (!p.enableGI || s.EnableSH == p.enableSH))
				return i;
		}
		return kSSGICustomIndex;
	}

	std::vector<std::string> SSGIQualityOptions()
	{
		return {
			T("menu.features.disabled", "Disabled"),
			T(TKEY("ao_only"), "AO only"),
			T(TKEY("low"), "Low"),
			T(TKEY("standard"), "Standard"),
			T(TKEY("extreme"), "Extreme"),
			T(TKEY("reference"), "Reference"),
			T(TKEY("custom"), "Custom"),
		};
	}

	// Option 0 is Disabled, so the presets and Custom sit one past their index.
	float SSGIOption(const Settings& s)
	{
		return s.Enabled ? static_cast<float>(FindSSGIPresetIndex(s) + 1) : 0.0f;
	}

	float __stdcall GetSSGIQuality()
	{
		return SSGIOption(SSGIRoot::Live());
	}

	void __stdcall SetSSGIQuality(float v)
	{
		const auto option = static_cast<size_t>(std::clamp<float>(v, 0.0f, static_cast<float>(kSSGICustomIndex + 1)));
		auto& s = SSGIRoot::Live();
		s.Enabled = option != 0;
		// SaveSettings writes userSettings, so Disabled and Custom must reach it too or the choice is lost on restart.
		globals::features::screenSpaceGI.userSettings.Enabled = s.Enabled;

		// Custom keeps the current values, so it re-enables without losing tuning.
		if (option == 0 || option - 1 >= kSSGIPresets.size())
			return;

		const auto& preset = kSSGIPresets[option - 1];
		const bool needsReblur = preset.halfRes || preset.quarterRes;
		s.NumSteps = preset.numSteps;
		s.EnableREBLUR = s.EnableREBLUR || needsReblur;
		s.HalfRes = preset.halfRes;
		s.QuarterRes = preset.quarterRes;
		s.EnableGI = preset.enableGI;
		if (preset.enableGI)
			s.EnableSH = preset.enableSH;
		auto& ssgi = globals::features::screenSpaceGI;
		ssgi.userSettings = ssgi.settings;
		ssgi.recompileFlag = true;
		ssgi.queuedResetHistory = true;
	}

	bool __stdcall IsSSGIEditable()
	{
		auto& ssgi = globals::features::screenSpaceGI;
		return !ssgi.appliedE11Overlay.active && NativeMenu::IsFeatureEditable(ssgi.GetShortName());
	}

	float __stdcall GetSSREnabled()
	{
		return globals::features::screenSpaceReflections.settings.Enabled ? 1.0f : 0.0f;
	}

	void __stdcall SetSSREnabled(float v)
	{
		auto& ssr = globals::features::screenSpaceReflections;
		const bool enabled = v != 0.0f;
		if (ssr.settings.Enabled == enabled)
			return;
		ssr.settings.Enabled = enabled;
		ssr.userSettings.Enabled = enabled;
		ssr.queuedEnabledChange = true;
	}

	bool __stdcall IsSSREditable()
	{
		auto& ssr = globals::features::screenSpaceReflections;
		return !ssr.appliedE11Overlay.active && NativeMenu::IsFeatureEditable(ssr.GetShortName());
	}
}

namespace NativeMenu
{
	std::vector<Row> SSGIRows()
	{
		std::vector<Row> rows;
		if (globals::features::screenSpaceGI.loaded) {
			rows.push_back(Dropdown(T(TKEY("name"), "Screen Space GI"), SSGIQualityOptions(), &GetSSGIQuality, &SetSSGIQuality,
				SSGIOption(Settings{}),
				T(TKEY("description"),
					"Screen Space Global Illumination adds realistic indirect lighting and ambient occlusion to the game. This technique simulates how light bounces off surfaces to illuminate other objects naturally."),
				&IsSSGIEditable));
		}

		if (globals::features::screenSpaceReflections.loaded && globals::features::dynamicCubemaps.loaded) {
			rows.push_back(Checkbox(T("feature.screen_space_reflections.name", "Screen Space Reflections"), &GetSSREnabled, &SetSSREnabled,
				ScreenSpaceReflections::Settings{}.Enabled ? 1.0f : 0.0f,
				T("feature.screen_space_reflections.native_menu_description",
					"Reflects nearby objects on screen in glossy surfaces, falling back to the dynamic cubemaps where rays leave the screen."),
				&IsSSREditable));
		}
		return rows;
	}
}

#undef I18N_KEY_PREFIX
