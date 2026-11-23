#include "VolumetricLighting.h"

#include "CSEditor/SceneManager/WeatherTransition.h"
#include "Effects11.h"
#include "I18n/I18n.h"
#include "InteriorSun.h"
#include "ShaderCache.h"
#include "SkySync.h"
#include "State.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.volumetric_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VolumetricLighting::TextureSize,
	Width,
	Height,
	Depth);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VolumetricLighting::Settings,
	ExteriorEnabled,
	ExteriorQuality,
	ExteriorCustomSize,
	ExteriorStrength,
	ExteriorSunFocus,
	ExteriorShaftDefinition,
	InteriorEnabled,
	InteriorQuality,
	InteriorCustomSize,
	InteriorStrength,
	InteriorSunFocus,
	InteriorShaftDefinition,
	Effects11Priority);

void VolumetricLighting::DrawSettings()
{
	if (ImGui::Checkbox(T(TKEY("enable_exteriors"), "Enable Volumetric Lighting in Exteriors"), &settings.ExteriorEnabled))
		SetupVL();

	if (settings.ExteriorEnabled) {
		DrawVolumetricLightingSettings(settings.ExteriorQuality, settings.ExteriorCustomSize, false, !inInterior);
		DrawGodRaySettings(settings.ExteriorStrength, settings.ExteriorSunFocus, settings.ExteriorShaftDefinition, false);
	}

	if (ImGui::Checkbox(T(TKEY("enable_interiors"), "Enable Volumetric Lighting in Interiors"), &settings.InteriorEnabled))
		SetupVL();

	if (settings.InteriorEnabled) {
		DrawVolumetricLightingSettings(settings.InteriorQuality, settings.InteriorCustomSize, true, inInterior);
		DrawGodRaySettings(settings.InteriorStrength, settings.InteriorSunFocus, settings.InteriorShaftDefinition, true);
	}

	DrawEffects11PrioritySetting();
}

void VolumetricLighting::DrawGodRaySettings(float& strength, float& sunFocus, float& shaftDefinition, const bool isInterior)
{
	ImGui::SliderFloat(isInterior ? T(TKEY("interior_strength"), "Interior God Ray Strength") : T(TKEY("exterior_strength"), "Exterior God Ray Strength"),
		&strength, 0.0f, MaxGodRayStrength, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("strength_tooltip"), "Brightness of the rays looking straight at the sun or moon, relative to the weather's own Volumetric Lighting record.\nA weather with no volumetric lighting stays dark at any strength."));

	ImGui::SliderFloat(isInterior ? T(TKEY("interior_sun_focus"), "Interior Sun Focus") : T(TKEY("exterior_sun_focus"), "Exterior Sun Focus"),
		&sunFocus, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("sun_focus_tooltip"), "Where God Ray Strength is spent.\n0.00 scales the whole scene evenly, 1.00 scales only the rays around the sun or moon and leaves the rest of the sky at the weather's own level."));

	ImGui::SliderFloat(isInterior ? T(TKEY("interior_shaft_definition"), "Interior Shaft Definition") : T(TKEY("exterior_shaft_definition"), "Exterior Shaft Definition"),
		&shaftDefinition, -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("shaft_definition_tooltip"), "How tightly the focused rays hug the sun or moon, plus a bias on the weather's scattering phase function.\nPositive values tighten them into defined shafts, negative values spread them into a wide glow."));
}

void VolumetricLighting::DrawEffects11PrioritySetting()
{
	if (!globals::features::effects11.loaded)
		return;

	const char* priorityNames[] = {
		T(TKEY("priority_effects11"), "Effects 11 Preset"),
		T(TKEY("priority_community_shaders"), "Strength Sliders"),
		T(TKEY("priority_combined"), "Both Combined")
	};

	settings.Effects11Priority = std::clamp(settings.Effects11Priority, 0, static_cast<int32_t>(GodRayPriority::Count) - 1);
	ImGui::Combo(T(TKEY("priority"), "God Ray Intensity Source"), &settings.Effects11Priority, priorityNames, IM_ARRAYSIZE(priorityNames));
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("priority_tooltip"), "Effects 11 presets set god ray intensity through the ENB [GAMEVOLUMETRICRAYS] Intensity key.\nThis chooses which one applies when both are active. The sliders take over on their own whenever the Effects 11 preset is switched off.\nShaft Definition is always applied, as Effects 11 does not control it."));
}

void VolumetricLighting::DrawVolumetricLightingSettings(int32_t& quality, TextureSize& customSize, const bool isInterior, const bool inLocationType)
{
	auto& [Width, Height, Depth] = FetchCurrentSizeInUnits(isInterior);
	const char* qualityNames[] = {
		T(TKEY("quality_low"), "Low"),
		T(TKEY("quality_medium"), "Medium"),
		T(TKEY("quality_high"), "High"),
		T(TKEY("quality_custom"), "Custom")
	};

	if (ImGui::SliderInt(isInterior ? T(TKEY("interior_quality"), "Interior Quality") : T(TKEY("exterior_quality"), "Exterior Quality"), &quality, 0, static_cast<uint8_t>(Quality::Count) - 1, qualityNames[std::clamp(quality, 0, static_cast<int32_t>(Quality::Count) - 1)], ImGuiSliderFlags_AlwaysClamp)) {
		if (inLocationType)
			SetupVL();
	}

	const bool isCustomQuality = static_cast<Quality>(quality) == Quality::Custom;
	if (!isCustomQuality)
		ImGui::BeginDisabled();

	if (ImGui::SliderInt(isInterior ? T(TKEY("interior_width"), "Interior Width") : T(TKEY("exterior_width"), "Exterior Width"), &Width, 1, 20, FromUnits(Width, 32), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		customSize.Width = Width * 32;
		if (inLocationType)
			SetupVL();
	}

	if (ImGui::SliderInt(isInterior ? T(TKEY("interior_height"), "Interior Height") : T(TKEY("exterior_height"), "Exterior Height"), &Height, 1, 20, FromUnits(Height, 32), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		customSize.Height = Height * 32;
		if (inLocationType)
			SetupVL();
	}

	if (ImGui::SliderInt(isInterior ? T(TKEY("interior_depth"), "Interior Depth") : T(TKEY("exterior_depth"), "Exterior Depth"), &Depth, 1, 64, FromUnits(Depth, 10), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		customSize.Depth = Depth * 10;
		if (inLocationType)
			SetupVL();
	}

	if (!isCustomQuality)
		ImGui::EndDisabled();
}

inline const char* VolumetricLighting::FromUnits(const int32_t value, const int32_t unitScale)
{
	static std::string s;
	s = std::to_string(value * unitScale);
	return s.c_str();
}

VolumetricLighting::TextureSize& VolumetricLighting::FetchCurrentSizeInUnits(const bool interior)
{
	auto& size = interior ? interiorSizeInUnits : exteriorSizeInUnits;
	if (interior) {
		switch (static_cast<Quality>(settings.InteriorQuality)) {
		case Quality::Low:
			size = *gVolumetricLightingSizeLow;
			break;
		case Quality::Medium:
			size = *gVolumetricLightingSizeMedium;
			break;
		case Quality::High:
			size = defaultSizeHigh;
			break;
		case Quality::Custom:
			size = settings.InteriorCustomSize;
			break;
		default:
			break;
		}
	} else {
		switch (static_cast<Quality>(settings.ExteriorQuality)) {
		case Quality::Low:
			size = *gVolumetricLightingSizeLow;
			break;
		case Quality::Medium:
			size = *gVolumetricLightingSizeMedium;
			break;
		case Quality::High:
			size = defaultSizeHigh;
			break;
		case Quality::Custom:
			size = settings.ExteriorCustomSize;
			break;
		default:
			break;
		}
	}

	size.Height /= 32;
	size.Width /= 32;
	size.Depth /= 10;

	return size;
}

void VolumetricLighting::ApplyIfChanged(const Settings& previous)
{
	// Before the first EarlyPrepass the game addresses aren't ready; that pass applies the settings itself
	if (initialised && settings != previous)
		SetupVL();
}

void VolumetricLighting::LoadSettings(json& o_json)
{
	const Settings previous = settings;
	settings = o_json;
	settings.ExteriorQuality = std::clamp(settings.ExteriorQuality, 0, static_cast<int32_t>(Quality::Count) - 1);
	settings.InteriorQuality = std::clamp(settings.InteriorQuality, 0, static_cast<int32_t>(Quality::Count) - 1);
	settings.ExteriorStrength = std::clamp(settings.ExteriorStrength, 0.0f, MaxGodRayStrength);
	settings.InteriorStrength = std::clamp(settings.InteriorStrength, 0.0f, MaxGodRayStrength);
	settings.ExteriorSunFocus = std::clamp(settings.ExteriorSunFocus, 0.0f, 1.0f);
	settings.InteriorSunFocus = std::clamp(settings.InteriorSunFocus, 0.0f, 1.0f);
	settings.ExteriorShaftDefinition = std::clamp(settings.ExteriorShaftDefinition, -1.0f, 1.0f);
	settings.InteriorShaftDefinition = std::clamp(settings.InteriorShaftDefinition, -1.0f, 1.0f);
	settings.Effects11Priority = std::clamp(settings.Effects11Priority, 0, static_cast<int32_t>(GodRayPriority::Count) - 1);
	// Custom sizes are stored in texels; the sliders only offer whole 32/32/10 texel steps
	for (auto* size : { &settings.ExteriorCustomSize, &settings.InteriorCustomSize }) {
		size->Width = std::clamp(size->Width, 32, 640);
		size->Height = std::clamp(size->Height, 32, 640);
		size->Depth = std::clamp(size->Depth, 10, 640);
	}
	ApplyIfChanged(previous);
}

void VolumetricLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void VolumetricLighting::RestoreDefaultSettings()
{
	const Settings previous = settings;
	settings = {};
	ApplyIfChanged(previous);
}

void VolumetricLighting::DataLoaded()
{
}

void VolumetricLighting::PostPostLoad()
{
	gVolumetricLightingSizeLow = reinterpret_cast<TextureSize*>(REL::RelocationID(527970, 414916).address());
	gVolumetricLightingSizeMedium = reinterpret_cast<TextureSize*>(REL::RelocationID(527973, 414919).address());
	gVolumetricLightingSizeHigh = reinterpret_cast<TextureSize*>(REL::RelocationID(527976, 414922).address());
	defaultSizeHigh = *gVolumetricLightingSizeHigh;

	// Ensure the VL raymarch compute shader is only dispatched once, rather than once for every level of depth
	// The updated raymarch shader iterates through the depth now instead
	// Skip the first call, the second call has read/write texture setup in the correct order
	REL::safe_fill(REL::RelocationID(100309, 107023).address() + REL::Relocate(0xA4, 0x406), REL::NOP, 3);
	// Exit the loop after the first iteration
	REL::safe_fill(REL::RelocationID(100309, 107023).address() + REL::Relocate(0x147, 0x4A9), REL::NOP, 6);
}

void VolumetricLighting::SetupResources()
{
	vlDataCB = new ConstantBuffer(ConstantBufferDesc<VLData>(), "VolumetricLighting::VLDataCB");
}

void VolumetricLighting::EarlyPrepass()
{
	int32_t width = static_cast<int32_t>(globals::game::graphicsState->screenWidth);
	int32_t height = static_cast<int32_t>(globals::game::graphicsState->screenHeight);

	fullScreenX = width;
	fullScreenY = height;

	// The blur targets are allocated at the full screen size, but under dynamic resolution only
	// the top-left render area holds valid data. Clamping and dispatching against the full size
	// makes the blur taps read stale pixels from outside that area along the right/bottom edges.
	const float2 renderSize = Util::ConvertToDynamic(float2{ (float)width, (float)height });

	vlData.screenX = std::max(1, static_cast<int32_t>(renderSize.x));
	vlData.screenY = std::max(1, static_cast<int32_t>(renderSize.y));
	vlData.screenXMin1 = vlData.screenX - 1;
	vlData.screenYMin1 = vlData.screenY - 1;
	vlDataCB->Update(vlData);

	const auto interiorCell = RE::TES::GetSingleton()->interiorCell;
	const bool currentlyInInterior = interiorCell != nullptr;

	const bool currentlyInInteriorWithSun = InteriorSun::IsInteriorWithSun(interiorCell);

	if (initialised && currentlyInInterior == inInterior && currentlyInInteriorWithSun == inInteriorWithSun)
		return;

	initialised = true;
	inInterior = currentlyInInterior;
	inInteriorWithSun = currentlyInInteriorWithSun;
	SetupVL();
}

void VolumetricLighting::SetupVL()
{
	if (inInterior) {
		*globals::game::bEnableVolumetricLighting = settings.InteriorEnabled && inInteriorWithSun;
		*gVolumetricLightingSizeHigh = static_cast<Quality>(settings.InteriorQuality) == Quality::Custom ? settings.InteriorCustomSize : defaultSizeHigh;
		SetVLQuality(GetVLDescriptor(), settings.InteriorQuality);
	} else {
		*globals::game::bEnableVolumetricLighting = settings.ExteriorEnabled;
		*gVolumetricLightingSizeHigh = static_cast<Quality>(settings.ExteriorQuality) == Quality::Custom ? settings.ExteriorCustomSize : defaultSizeHigh;
		SetVLQuality(GetVLDescriptor(), settings.ExteriorQuality);
	}
}

RE::BSVolumetricLightingRenderData& VolumetricLighting::GetRenderData()
{
	static auto& renderData = *reinterpret_cast<RE::BSVolumetricLightingRenderData*>(
		REL::RelocationID(527719, 414629).address() - offsetof(RE::BSVolumetricLightingRenderData, color));
	return renderData;
}

bool VolumetricLighting::ClaimEffects11Intensity()
{
	if (!loaded)
		return true;

	effects11DroveIntensity = static_cast<GodRayPriority>(settings.Effects11Priority) != GodRayPriority::CommunityShaders;
	return effects11DroveIntensity;
}

void VolumetricLighting::ApplyGodRaySettings()
{
	const bool effects11Claimed = std::exchange(effects11DroveIntensity, false);
	const auto priority = static_cast<GodRayPriority>(settings.Effects11Priority);

	const float strength = inInterior ? settings.InteriorStrength : settings.ExteriorStrength;
	const float sunFocus = inInterior ? settings.InteriorSunFocus : settings.ExteriorSunFocus;
	const float shaftDefinition = inInterior ? settings.InteriorShaftDefinition : settings.ExteriorShaftDefinition;

	auto& renderData = GetRenderData();

	godRayBufferData.GodRayGain = 0.0f;
	godRayBufferData.GodRayExponent = std::exp2(std::lerp(MinGodRayLobeExponentLog2, MaxGodRayLobeExponentLog2, (shaftDefinition + 1.0f) * 0.5f));

	// Under GodRayPriority::Effects11 the slider still applies whenever the preset is switched
	// off, since nothing claimed the intensity that tick.
	if (!(effects11Claimed && priority == GodRayPriority::Effects11)) {
		const float uniformStrength = std::lerp(strength, 1.0f, sunFocus);
		renderData.intensity *= uniformStrength;
		if (uniformStrength > 1e-4f)
			godRayBufferData.GodRayGain = strength / uniformStrength - 1.0f;
	}

	if (shaftDefinition != 0.0f) {
		const float target = shaftDefinition > 0.0f ? MaxForwardScattering : 0.0f;
		const float biased = std::lerp(renderData.phaseFunction.scattering, target, std::abs(shaftDefinition));
		renderData.phaseFunction.scattering = std::clamp(biased, -ScatteringLimit, ScatteringLimit);
	}
}

namespace
{
	constexpr size_t RenderDataFloatCount = sizeof(RE::BSVolumetricLightingRenderData) / sizeof(float);
	static_assert(RenderDataFloatCount * sizeof(float) == sizeof(RE::BSVolumetricLightingRenderData));

	constexpr float MeanDensityFactor = 0.5f;
	constexpr float MaxTransitionCorrection = 8.0f;

	void AccumulateRenderData(RE::BSVolumetricLightingRenderData& a_target, const RE::BSVolumetricLightingRenderData& a_source, float a_weight)
	{
		const auto source = reinterpret_cast<const float*>(&a_source);
		const auto target = reinterpret_cast<float*>(&a_target);
		for (size_t i = 0; i < RenderDataFloatCount; ++i)
			target[i] += source[i] * a_weight;
	}

	struct WeatherTimeBlend
	{
		RE::TESWeather::ColorTime first = RE::TESWeather::ColorTime::kDay;
		RE::TESWeather::ColorTime second = RE::TESWeather::ColorTime::kDay;
		float firstWeight = 1.0f;
	};

	WeatherTimeBlend GetWeatherTimeBlend(RE::Sky* a_sky)
	{
		WeatherTimeBlend result;
		RE::Sky::COLOR_BLEND blend{};
		a_sky->FillColorBlend(blend, a_sky->currentWeather, a_sky->currentWeatherPct, result.first, result.second);
		result.firstWeight = std::clamp(blend.blend[0] + blend.blend[2], 0.0f, 1.0f);
		return result;
	}

	RE::BSVolumetricLightingRenderData GetDefaultRenderData()
	{
		static const auto intensity = RE::GetINISetting("fVolumetricLightingIntensity:Display");
		static const auto customColorContribution = RE::GetINISetting("fVolumetricLightingCustomColorContribution:Display");
		static const auto densityContribution = RE::GetINISetting("fVolumetricLightingDensityContribution:Display");
		static const auto densityScale = RE::GetINISetting("fVolumetricLightingDensityScale:Display");
		static const auto windSpeedScale = RE::GetINISetting("fVolumetricLightingWindSpeedScale:Display");
		static const auto windFallingSpeed = RE::GetINISetting("fVolumetricLightingWindFallingSpeed:Display");
		static const auto phaseContribution = RE::GetINISetting("fVolumetricLightingPhaseContribution:Display");
		static const auto phaseScattering = RE::GetINISetting("fVolumetricLightingPhaseScattering:Display");
		static const auto rangeFactor = RE::GetINISetting("fVolumetricLightingRangeFactor:Display");

		auto value = [](const RE::Setting* setting) { return setting ? setting->GetFloat() : 0.0f; };
		return {
			value(intensity),
			{ value(customColorContribution) },
			{ 1.0f, 1.0f, 1.0f },
			{ value(densityContribution), value(densityScale), value(windSpeedScale), value(windFallingSpeed) },
			{ value(phaseContribution), value(phaseScattering) },
			{ value(rangeFactor) }
		};
	}

	RE::BSVolumetricLightingRenderData GetWeatherVolumetricLighting(const RE::TESWeather* a_weather, const WeatherTimeBlend& a_time, bool a_night)
	{
		auto record = [a_weather](RE::TESWeather::ColorTime a_colorTime) {
			const auto form = a_weather ? a_weather->volumetricLighting[a_colorTime] : nullptr;
			return form ? static_cast<const RE::BSVolumetricLightingRenderData&>(*form) : GetDefaultRenderData();
		};

		if (a_night)
			return record(RE::TESWeather::ColorTime::kNight);

		RE::BSVolumetricLightingRenderData result{};
		AccumulateRenderData(result, record(a_time.first), a_time.firstWeight);
		if (a_time.firstWeight < 1.0f)
			AccumulateRenderData(result, record(a_time.second), 1.0f - a_time.firstWeight);
		return result;
	}

	RE::NiColor GetWeatherSunlight(RE::Sky* a_sky, const RE::TESWeather* a_weather, const WeatherTimeBlend& a_time, bool a_night)
	{
		RE::NiColor result{};
		if (!a_weather)
			return result;

		constexpr auto sunlight = RE::TESWeather::ColorTypes::kSunlight;
		RE::Sky::COLOR_BLEND blend{};
		if (a_night) {
			blend.RGBVal[0] = a_weather->colorData[sunlight][RE::TESWeather::ColorTime::kNight];
			blend.blend[0] = 1.0f;
		} else {
			blend.RGBVal[0] = a_weather->colorData[sunlight][a_time.first];
			blend.RGBVal[1] = a_weather->colorData[sunlight][a_time.second];
			blend.blend[0] = a_time.firstWeight;
			blend.blend[1] = 1.0f - a_time.firstWeight;
		}
		a_sky->SetColor(result, &blend, 0.0f);
		return result;
	}

	float GetVolumetricBrightness(const RE::BSVolumetricLightingRenderData& a_data, const RE::NiColor& a_sunlight, float a_intensityScale)
	{
		const float customContribution = std::clamp(a_data.customColor.contribution, 0.0f, 1.0f);
		const RE::NiColor color = a_sunlight + (a_data.color - a_sunlight) * customContribution;
		const float colorMean = std::max((color.red + color.green + color.blue) / 3.0f, 0.0f);

		const float scattering = std::clamp(a_data.phaseFunction.scattering, -0.999f, 0.999f);
		const float meanPhase = std::abs(scattering) > 1e-3f ?
		                            (1.0f - scattering * scattering) / (8.0f * RE::NI_PI * scattering) * std::log((1.0f + scattering) / (1.0f - scattering)) :
		                            1.0f / (4.0f * RE::NI_PI);
		const float phase = std::max(std::lerp(1.0f, meanPhase, a_data.phaseFunction.contribution), 0.0f);
		const float density = std::max(std::lerp(1.0f, MeanDensityFactor, a_data.density.contribution), 0.0f);

		return std::max(a_data.intensity * a_intensityScale, 0.0f) * phase * density * colorMean;
	}
}

void VolumetricLighting::BeginSkyUpdate()
{
	auto& renderData = GetRenderData();
	if (hasWrittenRenderData && std::memcmp(&renderData, &writtenRenderData, sizeof(renderData)) == 0)
		renderData = engineRenderData;
	else
		engineRenderData = renderData;
}

void VolumetricLighting::DetectNightRenderData()
{
	nightRenderData = std::memcmp(&GetRenderData(), &engineRenderData, sizeof(RE::BSVolumetricLightingRenderData)) != 0;
}

void VolumetricLighting::FinishSkyUpdate(RE::Sky* a_sky)
{
	auto& renderData = GetRenderData();

	lightFade = 1.0f;
	const float weatherPct = a_sky ? a_sky->currentWeatherPct : 1.0f;
	if (a_sky && a_sky->mode.get() == RE::Sky::Mode::kFull && a_sky->currentClimate && a_sky->currentWeather && a_sky->lastWeather &&
		weatherPct > 0.0f && weatherPct < 1.0f) {
		const bool nightVolumetricLighting = nightRenderData;
		const bool nightSunlight = globals::features::skySync.GetLightNightWeight() >= 1.0f;
		const auto time = GetWeatherTimeBlend(a_sky);

		float intensityLast = 1.0f;
		float intensityCurrent = 1.0f;
		float nightLightLast = 1.0f;
		float nightLightCurrent = 1.0f;
		if (globals::features::effects11.loaded) {
			globals::features::effects11.GetGameVolumetricRaysIntensityEndpoints(intensityLast, intensityCurrent);
			std::tie(nightLightLast, nightLightCurrent) = globals::features::effects11.GetNightLightEndpoints();
		}

		const auto lastData = GetWeatherVolumetricLighting(a_sky->lastWeather, time, nightVolumetricLighting);
		const auto currentData = GetWeatherVolumetricLighting(a_sky->currentWeather, time, nightVolumetricLighting);
		const auto lastSunlight = GetWeatherSunlight(a_sky, a_sky->lastWeather, time, nightSunlight);
		const auto currentSunlight = GetWeatherSunlight(a_sky, a_sky->currentWeather, time, nightSunlight);

		const float volumetricPct = nightVolumetricLighting ? weatherPct : WeatherTransition::GetRawPct();
		RE::BSVolumetricLightingRenderData blendedData{};
		AccumulateRenderData(blendedData, lastData, 1.0f - volumetricPct);
		AccumulateRenderData(blendedData, currentData, volumetricPct);
		const RE::NiColor blendedSunlight = lastSunlight * (1.0f - weatherPct) + currentSunlight * weatherPct;

		const float blendedBrightness = GetVolumetricBrightness(blendedData, blendedSunlight, std::lerp(intensityLast, intensityCurrent, weatherPct));
		if (blendedBrightness > 1e-6f) {
			const float lastBrightness = GetVolumetricBrightness(lastData, lastSunlight, intensityLast);
			const float currentBrightness = GetVolumetricBrightness(currentData, currentSunlight, intensityCurrent);
			const float targetBrightness = std::lerp(lastBrightness, currentBrightness, weatherPct);
			renderData.intensity *= std::clamp(targetBrightness / blendedBrightness, 0.0f, MaxTransitionCorrection);
			if (targetBrightness > 1e-6f)
				lightFade = std::lerp(lastBrightness * nightLightLast, currentBrightness * nightLightCurrent, weatherPct) / targetBrightness;
		}
	}

	writtenRenderData = renderData;
	hasWrittenRenderData = true;
}

VolumetricLighting::VolumetricLightingDescriptor& VolumetricLighting::GetVLDescriptor()
{
	using func_t = decltype(&VolumetricLighting::GetVLDescriptor);
	static REL::Relocation<func_t> func{ REL::RelocationID(100297, 107014) };
	return func();
}

void VolumetricLighting::SetVLQuality(VolumetricLightingDescriptor& descriptor, const uint32_t quality)
{
	using func_t = decltype(&VolumetricLighting::SetVLQuality);
	static REL::Relocation<func_t> func{ REL::RelocationID(100299, 107016).address() };
	func(descriptor, std::clamp<uint32_t>(quality, 0, 2));
}

RE::BSImagespaceShader* VolumetricLighting::CreateShader(const std::string_view& name, const std::string_view& fileName, RE::BSComputeShader* computeShader)
{
	auto shader = RE::BSImagespaceShader::Create();
	shader->shaderType = RE::BSShader::Type::ImageSpace;
	shader->fxpFilename = fileName.data();
	shader->name = name.data();
	shader->originalShaderName = fileName.data();
	shader->computeShader = computeShader;
	shader->isComputeShader = true;
	return shader;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateGenerateCS(RE::BSComputeShader* computeShader)
{
	if (generateCS == nullptr)
		generateCS = CreateShader("BSImagespaceShaderVolumetricLightingGenerateCS", "ISVolumetricLightingGenerateCS", computeShader);
	return generateCS;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateRaymarchCS(RE::BSComputeShader* computeShader)
{
	if (raymarchCS == nullptr)
		raymarchCS = CreateShader("BSImagespaceShaderVolumetricLightingRaymarchCS", "ISVolumetricLightingRaymarchCS", computeShader);
	return raymarchCS;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateBlurHCS(RE::BSComputeShader* computeShader)
{
	if (blurHCS == nullptr)
		blurHCS = CreateShader("BSImagespaceShaderVolumetricLightingBlurHCS", "ISVolumetricLightingBlurHCS", computeShader);
	return blurHCS;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateBlurVCS(RE::BSComputeShader* computeShader)
{
	if (blurVCS == nullptr)
		blurVCS = CreateShader("BSImagespaceShaderVolumetricLightingBlurVCS", "ISVolumetricLightingBlurVCS", computeShader);
	return blurVCS;
}

void VolumetricLighting::SetDimensionsCB() const
{
	auto cb = vlDataCB->CB();
	globals::d3d::context->CSSetConstantBuffers(1, 1, &cb);
}

void VolumetricLighting::SetGroupCountsHCS(uint32_t& threadGroupCountX, uint32_t& threadGroupCountY) const
{
	threadGroupCountX = (vlData.screenX + BlurThreadGroupSizeX - BlurWindow * 2u - 1u) / (BlurThreadGroupSizeX - BlurWindow * 2u);
	// One group per row, so the game's full-height count would blur rows outside the dynamic resolution area.
	threadGroupCountY = static_cast<uint32_t>(vlData.screenY);
}

void VolumetricLighting::SetGroupCountsVCS(uint32_t& threadGroupCountX, uint32_t& threadGroupCountY) const
{
	// One group per column, so the game's full-width count would blur columns outside the dynamic resolution area.
	threadGroupCountX = static_cast<uint32_t>(vlData.screenX);
	threadGroupCountY = (vlData.screenY + BlurThreadGroupSizeY - BlurWindow * 2u - 1u) / (BlurThreadGroupSizeY - BlurWindow * 2u);
}

#undef I18N_KEY_PREFIX
