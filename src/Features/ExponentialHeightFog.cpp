#include "ExponentialHeightFog.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Globals.h"
#include "Features/IBL.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/Skylighting.h"
#include "Features/TerrainShadows.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "WeatherVariableRegistry.h"

#include <numbers>

#define I18N_KEY_PREFIX "feature.exp_height_fog."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ExponentialHeightFog::Settings,
	enabled,
	useDynamicCubemaps,
	startDistance,
	fogHeight,
	fogHeightFalloff,
	fogDensity,
	directionalInscatteringMultiplier,
	directionalInscatteringAnisotropy,
	inscatteringTint,
	cubemapMipLevel,
	sunlightAttenuationAmount,
	respectVanillaFogFade,
	disableVanillaFog,
	fogInscatteringColor,
	originalFogColorAmount,
	volumetricFogEnabled,
	volumetricGridPixelSize,
	volumetricGridSizeZ,
	volumetricFogDistance,
	volumetricFogStartDistance,
	volumetricFogNearFadeInDistance,
	volumetricFogExtinctionScale,
	volumetricFogScatteringDistribution,
	volumetricFogAlbedo,
	volumetricFogEmissive,
	volumetricDirectionalScatteringIntensity,
	volumetricShadowBias,
	volumetricDepthDistributionScale,
	volumetricSkyLightingIntensity,
	volumetricHistoryWeight,
	volumetricHistoryMissSampleCount,
	volumetricSampleJitterMultiplier,
	volumetricUpsampleJitterMultiplier,
	volumetricLocalLightScatteringIntensity,
	useVanillaFogSettings,
	vanillaFogStrength,
	fogLightingInfluence,
	distanceHazeMaxOpacity,
	distanceHazeStartDistance,
	distanceHazeFadeDistance)

namespace
{
	constexpr float kMinimumFogRange = 1.0f;
	constexpr float kMinimumFogPower = 0.01f;
	constexpr float kMinimumFogTransmittance = 0.0001f;
	constexpr float kReferenceOpacityFraction = 0.5f;
	constexpr float kMaximumWeatherHistoryChange = 0.1f;
	constexpr float kAnalyticalExtinctionScale = 0.001f * std::numbers::ln2_v<float> * std::numbers::ln2_v<float>;
	constexpr float4 kFallbackFogColor{ 0.85f, 0.88f, 0.92f, 1.0f };

	constexpr float kMinDistanceHazeFadeDistance = 1.0f;
	constexpr float kMaxDistanceHazeDistance = 200000.0f;

	float ClampFinite(float a_value, float a_min, float a_max, float a_default)
	{
		return std::isfinite(a_value) ? std::clamp(a_value, a_min, a_max) : a_default;
	}

	void ClampFollowAndHazeSettings(ExponentialHeightFog::Settings& a_settings)
	{
		const ExponentialHeightFog::Settings defaults{};
		a_settings.vanillaFogStrength = ClampFinite(a_settings.vanillaFogStrength, 0.0f, 4.0f, defaults.vanillaFogStrength);
		a_settings.fogLightingInfluence = ClampFinite(a_settings.fogLightingInfluence, 0.0f, 1.0f, defaults.fogLightingInfluence);
		a_settings.distanceHazeMaxOpacity = ClampFinite(a_settings.distanceHazeMaxOpacity, 0.0f, 1.0f, defaults.distanceHazeMaxOpacity);
		a_settings.distanceHazeStartDistance = ClampFinite(a_settings.distanceHazeStartDistance, 0.0f, kMaxDistanceHazeDistance, defaults.distanceHazeStartDistance);
		a_settings.distanceHazeFadeDistance = ClampFinite(a_settings.distanceHazeFadeDistance, kMinDistanceHazeFadeDistance, kMaxDistanceHazeDistance, defaults.distanceHazeFadeDistance);
	}

	bool CanReuseFogHistory(const ExponentialHeightFog::Settings& current, const ExponentialHeightFog::Settings& previous)
	{
		using Settings = ExponentialHeightFog::Settings;
		if (current.useVanillaFogSettings != previous.useVanillaFogSettings)
			return false;
		for (const auto field : {
				 &Settings::vanillaFogStrength, &Settings::fogLightingInfluence,
				 &Settings::fogDensity, &Settings::fogHeight, &Settings::fogHeightFalloff,
				 &Settings::volumetricFogDistance, &Settings::volumetricFogStartDistance,
				 &Settings::volumetricFogNearFadeInDistance, &Settings::volumetricFogExtinctionScale,
				 &Settings::volumetricDepthDistributionScale }) {
			if (current.*field != previous.*field)
				return false;
		}
		if (current.useVanillaFogSettings) {
			if (std::abs(current.vanillaFogDensity - previous.vanillaFogDensity) >
				kMaximumWeatherHistoryChange * std::max(current.vanillaFogDensity, previous.vanillaFogDensity))
				return false;
			auto weatherHistoryMatches = [](float value, float previousValue) {
				return std::abs(value - previousValue) <= kMaximumWeatherHistoryChange * std::max({ 1.0f, std::abs(value), std::abs(previousValue) });
			};
			for (const auto field : { &Settings::vanillaFogNear, &Settings::vanillaFogFar, &Settings::vanillaFogMaxOpacity, &Settings::vanillaFogPower }) {
				if (!weatherHistoryMatches(current.*field, previous.*field))
					return false;
			}
			for (const auto field : { &Settings::vanillaFogNearColor, &Settings::vanillaFogFarColor }) {
				const auto& color = current.*field;
				const auto& previousColor = previous.*field;
				if (!weatherHistoryMatches(color.x, previousColor.x) ||
					!weatherHistoryMatches(color.y, previousColor.y) ||
					!weatherHistoryMatches(color.z, previousColor.z))
					return false;
			}
		}
		return true;
	}

	float Halton(uint32_t a_index, uint32_t a_base)
	{
		float result = 0.0f;
		float invBase = 1.0f / static_cast<float>(a_base);
		float fraction = invBase;
		while (a_index > 0) {
			result += static_cast<float>(a_index % a_base) * fraction;
			a_index /= a_base;
			fraction *= invBase;
		}
		return result;
	}
}

void ExponentialHeightFog::RestoreDefaultSettings()
{
	settings = {};
}

void ExponentialHeightFog::LoadSettings(json& o_json)
{
	settings = o_json;
	ClampFollowAndHazeSettings(settings);
}

void ExponentialHeightFog::SaveSettings(json& o_json)
{
	o_json = settings;
}

ExponentialHeightFog::Settings ExponentialHeightFog::GetCommonBufferData() const
{
	Settings data = settings;
	ClampFollowAndHazeSettings(data);
	data.vanillaFogDensity = 0.0f;

	if (globals::state->isMapMenuOpen) {
		data.enabled = 0;
		data.disableVanillaFog = 0;
		return data;
	}

	const auto* sky = globals::game::sky;
	const bool hasUnboundedFogRange = sky && (sky->fogNear == std::numeric_limits<float>::infinity() || sky->fogFar == std::numeric_limits<float>::infinity());
	const float fogNear = sky ? std::max(std::isfinite(sky->fogNear) ? sky->fogNear : 0.0f, 0.0f) : 0.0f;
	const float fogFar = sky ? std::max(std::isfinite(sky->fogFar) ? sky->fogFar : fogNear + kMinimumFogRange, fogNear + kMinimumFogRange) : Settings{}.vanillaFogFar;
	const float fogPower = sky ? std::max(std::isfinite(sky->fogPower) ? sky->fogPower : 1.0f, kMinimumFogPower) : 1.0f;
	const float fogClamp = sky && !hasUnboundedFogRange ? std::clamp(std::isfinite(sky->fogClamp) ? sky->fogClamp : 0.0f, 0.0f, 1.0f - kMinimumFogTransmittance) : 0.0f;

	data.vanillaFogNear = fogNear;
	data.vanillaFogFar = fogFar;
	data.vanillaFogMaxOpacity = fogClamp;
	data.vanillaFogPower = fogPower;
	data.vanillaFogNearColor = kFallbackFogColor;
	data.vanillaFogFarColor = kFallbackFogColor;
	if (sky) {
		auto sanitizeColor = [](const RE::NiColor& color, const float4& fallback) {
			return float4{
				std::max(std::isfinite(color.red) ? color.red : fallback.x, 0.0f),
				std::max(std::isfinite(color.green) ? color.green : fallback.y, 0.0f),
				std::max(std::isfinite(color.blue) ? color.blue : fallback.z, 0.0f), 1.0f
			};
		};
		data.vanillaFogFarColor = sanitizeColor(sky->skyColor[static_cast<uint32_t>(RE::TESWeather::ColorTypes::kFogFar)], kFallbackFogColor);
		data.vanillaFogNearColor = sanitizeColor(sky->skyColor[static_cast<uint32_t>(RE::TESWeather::ColorTypes::kFogNear)], kFallbackFogColor);
	}
	if (!data.useVanillaFogSettings)
		return data;

	data.disableVanillaFog = 1;
	data.startDistance = fogNear;
	const float targetOpacity = data.vanillaFogMaxOpacity * kReferenceOpacityFraction;
	const float normalizedReference = std::pow(targetOpacity, 1.0f / fogPower);
	const float referenceDistance = std::max((fogFar - fogNear) * normalizedReference, kMinimumFogRange);
	const auto linearLightingData = globals::features::linearLighting.GetCommonBufferData();
	const float calibratedOpacity = linearLightingData.enableLinearLighting ?
	                                    std::pow(targetOpacity, linearLightingData.fogAlphaGamma) :
	                                    targetOpacity;
	data.vanillaFogDensity = -std::log(std::max(1.0f - calibratedOpacity, kMinimumFogTransmittance)) / (referenceDistance * kAnalyticalExtinctionScale);
	return data;
}

void ExponentialHeightFog::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable_exp_height_fog"), "Enable Exponential Height Fog"), (bool*)&settings.enabled);
	if (!ImGui::BeginTabBar("##ExponentialHeightFogTabs"))
		return;

	if (ImGui::BeginTabItem(T(TKEY("tab_general"), "General"))) {
		ImGui::BeginDisabled(settings.enabled == 0);
		DrawGeneralSettings();
		ImGui::EndDisabled();
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem(T(TKEY("volumetric_fog"), "Volumetric Fog"))) {
		ImGui::BeginDisabled(settings.enabled == 0);
		DrawVolumetricSettings();
		ImGui::EndDisabled();
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

void ExponentialHeightFog::DrawGeneralSettings()
{
	ImGui::Checkbox(T(TKEY("use_vanilla_fog_settings"), "Follow Vanilla Fog"), (bool*)&settings.useVanillaFogSettings);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_vanilla_fog_settings_tooltip"), "Derives fog density, start distance, and colors from the active weather while keeping exponential height falloff. Weather Fog Strength scales density. Replaces vanilla distance fog."));
	}
	ImGui::BeginDisabled(settings.useVanillaFogSettings == 0);
	Util::WeatherUI::SliderFloat(T(TKEY("vanilla_strength"), "Weather Fog Strength"), this, "vanillaFogStrength", &settings.vanillaFogStrength, 0.0f, 4.0f, "%.2f");
	Util::WeatherUI::SliderFloat(T(TKEY("lighting_influence"), "Weather Lighting Influence"), this, "fogLightingInfluence", &settings.fogLightingInfluence, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("density_height"), "Density and Height"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	Util::WeatherUI::SliderFloat(T(TKEY("fog_density"), "Fog Density"), this, "fogDensity", &settings.fogDensity, 0.0f, 1.0f, "%.3f");
	Util::WeatherUI::SliderFloat(T(TKEY("start_distance"), "Start Distance"), this, "startDistance", &settings.startDistance, 0.0f, 100000.0f, "%.1f");
	ImGui::EndDisabled();
	Util::WeatherUI::SliderFloat(T(TKEY("fog_height"), "Fog Height"), this, "fogHeight", &settings.fogHeight, -22000.0f, 22000.0f, "%.1f");
	Util::WeatherUI::SliderFloat(T(TKEY("fog_height_falloff"), "Fog Height Falloff"), this, "fogHeightFalloff", &settings.fogHeightFalloff, 0.001f, 2.0f, "%.3f");
	ImGui::BeginDisabled(settings.useVanillaFogSettings == 0 && settings.volumetricFogEnabled == 0);
	Util::WeatherUI::SliderFloat(T(TKEY("volumetric_extinction_scale"), "Extinction Scale"), this, "volumetricFogExtinctionScale", &settings.volumetricFogExtinctionScale, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_extinction_scale_tooltip"), "Scales fog density when following vanilla fog, including when volumetric fog is off. In manual mode, affects only volumetric fog."));
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("color_lighting"), "Color and Lighting"));
	Util::WeatherUI::ColorEdit4(T(TKEY("fog_inscattering_color"), "Fog Inscattering Color"), this, "fogInscatteringColor", (float*)&settings.fogInscatteringColor);
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	Util::WeatherUI::SliderFloat(T(TKEY("original_fog_color_amount"), "Original Fog Color Amount"), this, "originalFogColorAmount", &settings.originalFogColorAmount, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0 && settings.fogLightingInfluence <= 0.0f);
	Util::WeatherUI::SliderFloat(T(TKEY("dir_inscattering_mul"), "Directional Light Inscattering Multiplier"), this, "directionalInscatteringMultiplier", &settings.directionalInscatteringMultiplier, 0.0f, 10.0f, "%.2f");
	ImGui::BeginDisabled(settings.directionalInscatteringMultiplier <= 0.0f);
	Util::WeatherUI::SliderFloat(T(TKEY("dir_inscattering_anisotropy"), "Directional Light Inscattering Anisotropy"), this, "directionalInscatteringAnisotropy", &settings.directionalInscatteringAnisotropy, -0.99f, 0.99f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dir_inscattering_anisotropy_tooltip"),
							  "Controls the asymmetry of inscattering via the Henyey-Greenstein phase function.\n"
							  "Positive values produce forward scattering (glow around sun).\n"
							  "Zero is isotropic. Negative values produce back scattering."));
	}
	ImGui::EndDisabled();
	Util::WeatherUI::SliderFloat(T(TKEY("sunlight_attenuation"), "Sunlight Attenuation Amount"), this, "sunlightAttenuationAmount", &settings.sunlightAttenuationAmount, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("vanilla_fog"), "Vanilla Fog"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::Checkbox(T(TKEY("disable_vanilla_fog"), "Disable Vanilla Fog"), (bool*)&settings.disableVanillaFog);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("disable_vanilla_fog_tooltip"), "Disables the vanilla fog entirely. Only exponential height fog will be applied."));
	}
	ImGui::EndDisabled();
	Util::WeatherUI::Checkbox(T(TKEY("apply_vanilla_fade"), "Apply Vanilla Fade"), this, "respectVanillaFogFade", (bool*)&settings.respectVanillaFogFade);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("apply_vanilla_fade_tooltip"), "Applies vanilla fade brightness to exponential height fog."));
	}

	ImGui::SeparatorText(T("feature.dynamic_cubemaps.name", "Dynamic Cubemaps"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0 || !globals::features::dynamicCubemaps.loaded);
	ImGui::Checkbox(T(TKEY("use_dynamic_cubemaps"), "Use Dynamic Cubemaps for Inscattering"), (bool*)&settings.useDynamicCubemaps);
	ImGui::BeginDisabled(settings.useDynamicCubemaps == 0);
	Util::WeatherUI::ColorEdit4(T(TKEY("inscattering_cubemap_tint"), "Inscattering Cubemap Tint"), this, "inscatteringTint", (float*)&settings.inscatteringTint);
	ImGui::BeginDisabled(settings.inscatteringTint.w <= 0.0f);
	ImGui::SliderFloat(T(TKEY("cubemap_mip_level"), "Cubemap Mip Level"), &settings.cubemapMipLevel, 1.0f, 8.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("distance_haze"), "Distance Haze"));
	Util::WeatherUI::SliderFloat(T(TKEY("distance_haze_max_opacity"), "Haze Maximum Opacity"), this, "distanceHazeMaxOpacity", &settings.distanceHazeMaxOpacity, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_max_opacity_tooltip"), "Adds haze at all heights using the fog colors above, while preserving dense height fog. Zero disables distance haze."));
	}
	Util::WeatherUI::SliderFloat(T(TKEY("distance_haze_start_distance"), "Haze Start Distance"), this, "distanceHazeStartDistance", &settings.distanceHazeStartDistance, 0.0f, kMaxDistanceHazeDistance, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_start_distance_tooltip"), "Horizontal distance from the camera where haze begins, in game units. Independent of height fog Start Distance."));
	}
	Util::WeatherUI::SliderFloat(T(TKEY("distance_haze_fade_distance"), "Haze Fade Distance"), this, "distanceHazeFadeDistance", &settings.distanceHazeFadeDistance, kMinDistanceHazeFadeDistance, kMaxDistanceHazeDistance, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_fade_distance_tooltip"), "Horizontal distance beyond Haze Start Distance over which haze smoothly reaches its maximum opacity, in game units."));
	}
}

void ExponentialHeightFog::DrawVolumetricSettings()
{
	Util::WeatherUI::Checkbox(T(TKEY("enable_volumetric_fog"), "Enable Volumetric Fog"), this, "volumetricFogEnabled", (bool*)&settings.volumetricFogEnabled);
	ImGui::BeginDisabled(settings.volumetricFogEnabled == 0);
	Util::WeatherUI::SliderFloat(T(TKEY("volumetric_view_distance"), "Volumetric View Distance"), this, "volumetricFogDistance", &settings.volumetricFogDistance, 1000.0f, 200000.0f, "%.0f");
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	Util::WeatherUI::SliderFloat(T(TKEY("volumetric_start_distance"), "Volumetric Start Distance"), this, "volumetricFogStartDistance", &settings.volumetricFogStartDistance, 0.0f, 20000.0f, "%.0f");
	Util::WeatherUI::SliderFloat(T(TKEY("near_fade_in_distance"), "Near Fade In Distance"), this, "volumetricFogNearFadeInDistance", &settings.volumetricFogNearFadeInDistance, 0.0f, 20000.0f, "%.0f");
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("color_lighting"), "Color and Lighting"));
	Util::WeatherUI::ColorEdit4(T(TKEY("volumetric_albedo"), "Volumetric Albedo"), this, "volumetricFogAlbedo", (float*)&settings.volumetricFogAlbedo);
	Util::WeatherUI::ColorEdit4(T(TKEY("volumetric_emissive"), "Volumetric Emissive"), this, "volumetricFogEmissive", (float*)&settings.volumetricFogEmissive);
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0 && settings.fogLightingInfluence <= 0.0f);
	Util::WeatherUI::SliderFloat(T(TKEY("directional_scattering_intensity"), "Directional Scattering Intensity"), this, "volumetricDirectionalScatteringIntensity", &settings.volumetricDirectionalScatteringIntensity, 0.0f, 10.0f, "%.2f");
	ImGui::BeginDisabled(settings.volumetricDirectionalScatteringIntensity <= 0.0f);
	ImGui::SliderFloat(T(TKEY("directional_shadow_bias"), "Directional Shadow Bias"), &settings.volumetricShadowBias, 0.0f, 0.05f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	Util::WeatherUI::SliderFloat(T(TKEY("sky_lighting_scattering_intensity"), "Sky Lighting Scattering Intensity"), this, "volumetricSkyLightingIntensity", &settings.volumetricSkyLightingIntensity, 0.0f, 10.0f, "%.2f");
	ImGui::BeginDisabled(!globals::features::lightLimitFix.loaded);
	Util::WeatherUI::SliderFloat(T(TKEY("local_light_scattering_intensity"), "Local Light Scattering Intensity"), this, "volumetricLocalLightScatteringIntensity", &settings.volumetricLocalLightScatteringIntensity, 0.0f, 10.0f, "%.2f");
	ImGui::EndDisabled();
	const bool hasScattering = settings.volumetricDirectionalScatteringIntensity > 0.0f || settings.volumetricSkyLightingIntensity > 0.0f ||
	                           (globals::features::lightLimitFix.loaded && settings.volumetricLocalLightScatteringIntensity > 0.0f);
	ImGui::BeginDisabled(!hasScattering);
	Util::WeatherUI::SliderFloat(T(TKEY("volumetric_scattering_distribution"), "Volumetric Scattering Distribution"), this, "volumetricFogScatteringDistribution", &settings.volumetricFogScatteringDistribution, -0.9f, 0.9f, "%.2f");
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	if (ImGui::TreeNode(T(TKEY("debug"), "Quality and Temporal Filtering"))) {
		uint32_t minGridPixelSize = 4;
		uint32_t maxGridPixelSize = 64;
		uint32_t minGridSizeZ = 16;
		uint32_t maxGridSizeZ = 160;
		ImGui::SliderScalar(T(TKEY("grid_pixel_size"), "Grid Pixel Size"), ImGuiDataType_U32, &settings.volumetricGridPixelSize, &minGridPixelSize, &maxGridPixelSize, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderScalar(T(TKEY("grid_depth_slices"), "Grid Depth Slices"), ImGuiDataType_U32, &settings.volumetricGridSizeZ, &minGridSizeZ, &maxGridSizeZ, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("depth_distribution_scale"), "Depth Distribution Scale"), &settings.volumetricDepthDistributionScale, 1.0f, 128.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		const bool temporalReprojection = Util::GetTemporal();
		ImGui::BeginDisabled(!temporalReprojection);
		ImGui::SliderFloat(T(TKEY("temporal_history_weight"), "Temporal History Weight"), &settings.volumetricHistoryWeight, 0.0f, 0.99f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndDisabled();
		uint32_t minHistoryMissSampleCount = 1;
		uint32_t maxHistoryMissSampleCount = 16;
		ImGui::SliderScalar(T(TKEY("history_miss_samples"), "History Miss Samples"), ImGuiDataType_U32, &settings.volumetricHistoryMissSampleCount, &minHistoryMissSampleCount, &maxHistoryMissSampleCount, "%u", ImGuiSliderFlags_AlwaysClamp);
		ImGui::BeginDisabled(!temporalReprojection);
		ImGui::SliderFloat(T(TKEY("sample_jitter_multiplier"), "Sample Jitter Multiplier"), &settings.volumetricSampleJitterMultiplier, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("sample_jitter_multiplier_tooltip"),
								  "Matches UE's r.VolumetricFog.LightScatteringSampleJitterMultiplier.\n"
								  "Adds per-voxel random offset on top of the Halton sequence.\n"
								  "0 = UE default; nonzero values need stronger temporal filtering."));
		}
		ImGui::EndDisabled();
		ImGui::SliderFloat(T(TKEY("upsample_jitter_multiplier"), "Upsample Jitter Multiplier"), &settings.volumetricUpsampleJitterMultiplier, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("upsample_jitter_multiplier_tooltip"),
								  "Matches UE's r.VolumetricFog.UpsampleJitterMultiplier.\n"
								  "Jitters the final 3D fog lookup in screen space to hide\n"
								  "low-resolution froxel pixelization. 0 = UE default."));
		}
		ImGui::TreePop();
	}
	ImGui::EndDisabled();
}

void ExponentialHeightFog::SetupResources()
{
	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	Util::SetResourceName(linearSampler.get(), "ExponentialHeightFog::LinearSampler");

	samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, shadowSampler.put()));
	Util::SetResourceName(shadowSampler.get(), "ExponentialHeightFog::ShadowSampler");

	volumetricFogCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<VolumetricFogCB>(), "ExponentialHeightFog::VolumetricFogCB");
}

void ExponentialHeightFog::ClearShaderCache()
{
	if (materialSetupCS) {
		materialSetupCS->Release();
		materialSetupCS = nullptr;
	}
	if (conservativeDepthCS) {
		conservativeDepthCS->Release();
		conservativeDepthCS = nullptr;
	}
	if (lightScatteringCS) {
		lightScatteringCS->Release();
		lightScatteringCS = nullptr;
	}
	if (integrationCS) {
		integrationCS->Release();
		integrationCS = nullptr;
	}
}

void ExponentialHeightFog::CaptureDirectionalShadowMap()
{
	ID3D11ShaderResourceView* shadowMap = nullptr;
	globals::d3d::context->PSGetShaderResources(4, 1, &shadowMap);
	directionalShadowMap.copy_from(shadowMap);
	if (shadowMap)
		shadowMap->Release();
}

void ExponentialHeightFog::EnsureVolumetricResources()
{
	uint32_t pixelSize = std::clamp(settings.volumetricGridPixelSize, 4u, 64u);
	const uint32_t gridZ = std::clamp(settings.volumetricGridSizeZ, 16u, 160u);
	float2 screenSz{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	auto renderSize = Util::ConvertToDynamic(screenSz);

	auto getGridSize = [&renderSize, gridZ](uint32_t a_pixelSize) {
		return DirectX::XMUINT4{
			std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.x / static_cast<float>(a_pixelSize)))),
			std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.y / static_cast<float>(a_pixelSize)))),
			gridZ,
			0u
		};
	};
	DirectX::XMUINT4 gridSize = getGridSize(pixelSize);

	constexpr uint64_t maxVolumeVoxels = 16ull * 1024ull * 1024ull;
	while (pixelSize < 64u &&
		   static_cast<uint64_t>(gridSize.x) * gridSize.y * gridSize.z > maxVolumeVoxels) {
		pixelSize++;
		gridSize = getGridSize(pixelSize);
	}

	if (vBufferA && currentGridSize.x == gridSize.x && currentGridSize.y == gridSize.y && currentGridSize.z == gridSize.z)
		return;

	currentGridSize = gridSize;

	D3D11_TEXTURE3D_DESC texDesc{};
	texDesc.Width = gridSize.x;
	texDesc.Height = gridSize.y;
	texDesc.Depth = gridSize.z;
	texDesc.MipLevels = 1;
	texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
	srvDesc.Texture3D.MipLevels = 1;

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = texDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
	uavDesc.Texture3D.MipSlice = 0;
	uavDesc.Texture3D.FirstWSlice = 0;
	uavDesc.Texture3D.WSize = gridSize.z;

	vBufferA = std::make_unique<Texture3D>(texDesc, "ExponentialHeightFog::VBufferA");
	vBufferA->CreateSRV(srvDesc);
	vBufferA->CreateUAV(uavDesc);

	D3D11_TEXTURE2D_DESC conservativeDepthDesc{};
	conservativeDepthDesc.Width = gridSize.x;
	conservativeDepthDesc.Height = gridSize.y;
	conservativeDepthDesc.MipLevels = 1;
	conservativeDepthDesc.ArraySize = 1;
	conservativeDepthDesc.Format = DXGI_FORMAT_R32_FLOAT;
	conservativeDepthDesc.SampleDesc.Count = 1;
	conservativeDepthDesc.Usage = D3D11_USAGE_DEFAULT;
	conservativeDepthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	D3D11_SHADER_RESOURCE_VIEW_DESC conservativeDepthSrvDesc{};
	conservativeDepthSrvDesc.Format = conservativeDepthDesc.Format;
	conservativeDepthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	conservativeDepthSrvDesc.Texture2D.MipLevels = 1;

	D3D11_UNORDERED_ACCESS_VIEW_DESC conservativeDepthUavDesc{};
	conservativeDepthUavDesc.Format = conservativeDepthDesc.Format;
	conservativeDepthUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

	conservativeDepth = std::make_unique<Texture2D>(conservativeDepthDesc, "ExponentialHeightFog::ConservativeDepth");
	conservativeDepth->CreateSRV(conservativeDepthSrvDesc);
	conservativeDepth->CreateUAV(conservativeDepthUavDesc);

	conservativeDepthHistory = std::make_unique<Texture2D>(conservativeDepthDesc, "ExponentialHeightFog::ConservativeDepthHistory");
	conservativeDepthHistory->CreateSRV(conservativeDepthSrvDesc);

	lightScattering = std::make_unique<Texture3D>(texDesc, "ExponentialHeightFog::LightScattering");
	lightScattering->CreateSRV(srvDesc);
	lightScattering->CreateUAV(uavDesc);

	lightScatteringHistory = std::make_unique<Texture3D>(texDesc, "ExponentialHeightFog::LightScatteringHistory");
	lightScatteringHistory->CreateSRV(srvDesc);

	integratedLightScattering = std::make_unique<Texture3D>(texDesc, "ExponentialHeightFog::IntegratedLightScattering");
	integratedLightScattering->CreateSRV(srvDesc);
	integratedLightScattering->CreateUAV(uavDesc);

	hasLightScatteringHistory = false;
	hasConservativeDepthHistory = false;
	lastPrepassFrame = UINT32_MAX;
}

void ExponentialHeightFog::ReleaseVolumetricResources()
{
	vBufferA.reset();
	conservativeDepth.reset();
	conservativeDepthHistory.reset();
	lightScattering.reset();
	lightScatteringHistory.reset();
	integratedLightScattering.reset();
	currentGridSize = {};
	hasLightScatteringHistory = false;
	hasConservativeDepthHistory = false;
	lastPrepassFrame = UINT32_MAX;
	ID3D11ShaderResourceView* nullSRV = nullptr;
	globals::d3d::context->PSSetShaderResources(19, 1, &nullSRV);
}

void ExponentialHeightFog::BindIntegratedLightScattering()
{
	ID3D11ShaderResourceView* srv = integratedLightScattering ? integratedLightScattering->srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(19, 1, &srv);
}

ID3D11ComputeShader* ExponentialHeightFog::GetMaterialSetupCS()
{
	if (!materialSetupCS)
		materialSetupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", {}, "cs_5_0"));
	return materialSetupCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetConservativeDepthCS()
{
	if (!conservativeDepthCS)
		conservativeDepthCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", {}, "cs_5_0"));
	return conservativeDepthCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetLightScatteringCS()
{
	if (!lightScatteringCS) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::lightLimitFix.loaded) {
			defines.emplace_back("LIGHT_LIMIT_FIX", "");
		}
		if (globals::features::terrainShadows.loaded) {
			defines.emplace_back("TERRAIN_SHADOWS", "");
		}
		if (globals::features::cloudShadows.loaded) {
			defines.emplace_back("CLOUD_SHADOWS", "");
		}
		lightScatteringCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", defines, "cs_5_0"));
	}
	return lightScatteringCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetIntegrationCS()
{
	if (!integrationCS)
		integrationCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", {}, "cs_5_0"));
	return integrationCS;
}

void ExponentialHeightFog::Prepass()
{
	if (!settings.enabled || !settings.volumetricFogEnabled || settings.volumetricFogExtinctionScale <= 0.0f) {
		ReleaseVolumetricResources();
		return;
	}

	if (globals::state->isMapMenuOpen)
		return;

	const auto cameraData = Util::GetCameraData();
	const float volumeStart = settings.useVanillaFogSettings ? 0.0f : std::max(settings.volumetricFogStartDistance, 0.0f);
	if (settings.volumetricFogDistance <= std::max(cameraData.y, volumeStart) + 10.0f) {
		ReleaseVolumetricResources();
		return;
	}
	const Settings frameSettings = GetCommonBufferData();
	const float fogDensity = frameSettings.useVanillaFogSettings ? frameSettings.vanillaFogDensity * frameSettings.vanillaFogStrength : frameSettings.fogDensity;
	if (fogDensity <= 0.0f) {
		ReleaseVolumetricResources();
		return;
	}
	EnsureVolumetricResources();
	if (lastPrepassFrame == UINT32_MAX || !CanReuseFogHistory(frameSettings, previousFogSettings)) {
		hasLightScatteringHistory = false;
	}
	previousFogSettings = frameSettings;

	ID3D11ShaderResourceView* directionalShadowLightData = globals::deferred && globals::deferred->directionalShadowLights ? globals::deferred->directionalShadowLights->srv.get() : nullptr;
	auto& lightLimitFix = globals::features::lightLimitFix;
	const bool hasLocalLightData =
		lightLimitFix.loaded &&
		lightLimitFix.lights &&
		lightLimitFix.lightIndexList &&
		lightLimitFix.lightGrid;
	auto* depthSrv = Util::GetCurrentSceneDepthSRV(true);
	auto& ibl = globals::features::ibl;
	auto& skylighting = globals::features::skylighting;
	const bool hasIBL = ibl.loaded &&
	                    ibl.settings.EnableIBL != 0 &&
	                    !ibl.IsDisabledForCurrentScene() &&
	                    ibl.envIBLTexture &&
	                    ibl.skyIBLTexture;
	const bool hasSkylighting = skylighting.loaded && skylighting.texProbeArray;

	const bool temporalReprojection = Util::GetTemporal();
	const bool temporalHistoryValid =
		temporalReprojection &&
		hasLightScatteringHistory &&
		lastPrepassFrame != UINT32_MAX &&
		globals::state->frameCount == lastPrepassFrame + 1u;

	VolumetricFogCB cb{};
	cb.gridSizeAndFlags = {
		currentGridSize.x,
		currentGridSize.y,
		currentGridSize.z,
		(directionalShadowMap && directionalShadowLightData ? 1u : 0u) |
			(depthSrv ? 2u : 0u) |
			(hasIBL ? 4u : 0u) |
			(hasSkylighting ? 8u : 0u) |
			(depthSrv && temporalHistoryValid && hasConservativeDepthHistory ? 16u : 0u) |
			(hasLocalLightData ? 32u : 0u)
	};
	cb.invGridSizeAndNearFade = {
		1.0f / static_cast<float>(currentGridSize.x),
		1.0f / static_cast<float>(currentGridSize.y),
		1.0f / static_cast<float>(currentGridSize.z),
		!settings.useVanillaFogSettings && settings.volumetricFogNearFadeInDistance > 0.0f ? 1.0f / settings.volumetricFogNearFadeInDistance : 100000000.0f
	};

	const double nearPlane = std::max(static_cast<double>(cameraData.y), static_cast<double>(volumeStart));
	const double farPlane = std::max(nearPlane + 1.0, static_cast<double>(settings.volumetricFogDistance));
	const double nearWithOffset = nearPlane + 0.095 * 100.0;
	const double depthDistributionScale = std::max(
		static_cast<double>(settings.volumetricDepthDistributionScale),
		static_cast<double>(currentGridSize.z) / 120.0);
	const double farExp = std::exp2(std::min(static_cast<double>(currentGridSize.z) / depthDistributionScale, 120.0));
	const double gridZOffset = (farPlane - nearWithOffset * farExp) / (farPlane - nearWithOffset);
	const double gridZScale = (1.0 - gridZOffset) / nearWithOffset;
	cb.gridZParams = {
		static_cast<float>(gridZScale),
		static_cast<float>(gridZOffset),
		static_cast<float>(depthDistributionScale),
		0.0f
	};

	cb.clipToWorld = globals::game::frameBufferCached.GetCameraViewProjUnjittered().Invert();

	for (uint32_t i = 0; i < std::size(cb.frameJitterOffsets); i++) {
		const uint32_t temporalFrame = (globals::state->frameCount - i) & 1023u;
		cb.frameJitterOffsets[i] = {
			temporalReprojection ? Halton(temporalFrame, 2) : 0.5f,
			temporalReprojection ? Halton(temporalFrame, 3) : 0.5f,
			temporalReprojection ? Halton(temporalFrame, 5) : 0.5f,
			0.0f
		};
	}
	cb.historyParameters = {
		temporalHistoryValid ? std::clamp(settings.volumetricHistoryWeight, 0.0f, 0.99f) : 0.0f,
		static_cast<float>(std::clamp(settings.volumetricHistoryMissSampleCount, 1u, 16u)),
		0.0f,
		0.0f
	};
	cb.jitterParameters = {
		temporalReprojection ? std::max(settings.volumetricSampleJitterMultiplier, 0.0f) : 0.0f,
		static_cast<float>(globals::state->frameCount % 8u),
		0.0f,
		0.0f
	};
	volumetricFogCB->Update(cb);

	auto context = globals::d3d::context;
	ID3D11Buffer* cbuffers[1]{ volumetricFogCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbuffers);

	ID3D11Buffer* sharedBuffers[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);

	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	context->CSSetConstantBuffers(12, 1, frameBuffers);

	ID3D11SamplerState* samplers[2]{ linearSampler.get(), shadowSampler.get() };
	context->CSSetSamplers(0, 2, samplers);

	const uint32_t groupX = (currentGridSize.x + 7) / 8;
	const uint32_t groupY = (currentGridSize.y + 7) / 8;
	const uint32_t groupZ = (currentGridSize.z + 3) / 4;

	context->CSSetShaderResources(17, 1, &depthSrv);
	ID3D11ShaderResourceView* skylightingSrv = hasSkylighting ? skylighting.texProbeArray->srv.get() : nullptr;
	ID3D11ShaderResourceView* iblSrvs[2]{
		hasIBL ? ibl.envIBLTexture->srv.get() : nullptr,
		hasIBL ? ibl.skyIBLTexture->srv.get() : nullptr
	};
	context->CSSetShaderResources(50, 1, &skylightingSrv);
	context->CSSetShaderResources(76, 2, iblSrvs);

	if (depthSrv) {
		ID3D11UnorderedAccessView* uavs[1]{ conservativeDepth->uav.get() };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(GetConservativeDepthCS(), nullptr, 0);
		context->Dispatch(groupX, groupY, 1);
		uavs[0] = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	}

	{
		ID3D11UnorderedAccessView* uavs[1]{ vBufferA->uav.get() };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(GetMaterialSetupCS(), nullptr, 0);
		context->Dispatch(groupX, groupY, groupZ);
		uavs[0] = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	}

	{
		ID3D11ShaderResourceView* srvs[5]{
			vBufferA->srv.get(),
			directionalShadowMap.get(),
			temporalHistoryValid ? lightScatteringHistory->srv.get() : nullptr,
			conservativeDepth->srv.get(),
			temporalHistoryValid && hasConservativeDepthHistory ? conservativeDepthHistory->srv.get() : nullptr
		};
		ID3D11ShaderResourceView* localLightSrvs[3]{
			hasLocalLightData ? lightLimitFix.lights->srv.get() : nullptr,
			hasLocalLightData ? lightLimitFix.lightIndexList->srv.get() : nullptr,
			hasLocalLightData ? lightLimitFix.lightGrid->srv.get() : nullptr
		};
		ID3D11UnorderedAccessView* uavs[1]{ lightScattering->uav.get() };
		context->CSSetShaderResources(0, 5, srvs);
		context->CSSetShaderResources(35, 3, localLightSrvs);
		context->CSSetShaderResources(98, 1, &directionalShadowLightData);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(GetLightScatteringCS(), nullptr, 0);
		context->Dispatch(groupX, groupY, groupZ);
		uavs[0] = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	}

	{
		ID3D11ShaderResourceView* srvs[1]{ lightScattering->srv.get() };
		ID3D11UnorderedAccessView* uavs[1]{ integratedLightScattering->uav.get() };
		context->CSSetShaderResources(0, 1, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(GetIntegrationCS(), nullptr, 0);
		context->Dispatch(groupX, groupY, 1);
	}

	ID3D11ShaderResourceView* nullSrvs[5]{ nullptr, nullptr, nullptr, nullptr, nullptr };
	ID3D11ShaderResourceView* nullDepthSrv[1]{ nullptr };
	ID3D11UnorderedAccessView* nullUav[1]{ nullptr };
	ID3D11SamplerState* nullSamplers[2]{ nullptr, nullptr };
	ID3D11Buffer* nullCb[1]{ nullptr };
	context->CSSetShaderResources(0, 5, nullSrvs);
	context->CSSetShaderResources(17, 1, nullDepthSrv);
	context->CSSetShaderResources(35, 3, nullSrvs);
	context->CSSetShaderResources(50, 1, nullDepthSrv);
	context->CSSetShaderResources(76, 2, nullSrvs);
	context->CSSetShaderResources(98, 1, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
	context->CSSetSamplers(0, 2, nullSamplers);
	context->CSSetConstantBuffers(0, 1, nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	if (temporalReprojection) {
		context->CopyResource(lightScatteringHistory->resource.get(), lightScattering->resource.get());
		hasLightScatteringHistory = true;
		if (depthSrv) {
			context->CopyResource(conservativeDepthHistory->resource.get(), conservativeDepth->resource.get());
			hasConservativeDepthHistory = true;
		} else {
			hasConservativeDepthHistory = false;
		}
	} else {
		hasLightScatteringHistory = false;
		hasConservativeDepthHistory = false;
	}

	lastPrepassFrame = globals::state->frameCount;
	BindIntegratedLightScattering();
}

void ExponentialHeightFog::RegisterWeatherVariables()
{
	auto* registry = WeatherVariables::GlobalWeatherRegistry::GetSingleton()->GetOrCreateFeatureRegistry(GetShortName());
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Start Distance",
		"startDistance",
		"Start distance of the fog, from the camera",
		&settings.startDistance,
		0.0f,
		0.0f, 100000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Fog Height",
		"fogHeight",
		"Base height of the fog effect",
		&settings.fogHeight,
		0.0f,
		-22000.0f, 22000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Fog Height Falloff",
		"fogHeightFalloff",
		"Height density factor controls how the density increases as height decreases",
		&settings.fogHeightFalloff,
		0.2f,
		0.001f, 2.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::Float4Variable>(
		"Fog Inscattering Color",
		"fogInscatteringColor",
		"Color added to the fog inscattering contribution",
		&settings.fogInscatteringColor,
		float4{ 0.0f, 0.0f, 0.0f, 1.0f }));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Original Fog Color Amount",
		"originalFogColorAmount",
		"Amount of the original fog color added to fog inscattering",
		&settings.originalFogColorAmount,
		1.0f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Fog Density",
		"fogDensity",
		"Overall density of the fog",
		&settings.fogDensity,
		0.02f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Directional Inscattering Multiplier",
		"directionalInscatteringMultiplier",
		"Multiplier for directional light inscattering",
		&settings.directionalInscatteringMultiplier,
		1.0f,
		0.0f, 10.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Sunlight Attenuation Amount",
		"sunlightAttenuationAmount",
		"Amount of fog attenuation applied to direct sunlight",
		&settings.sunlightAttenuationAmount,
		1.0f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Directional Inscattering Anisotropy",
		"directionalInscatteringAnisotropy",
		"Henyey-Greenstein asymmetry parameter. Positive = forward scattering, 0 = isotropic, negative = back scattering.",
		&settings.directionalInscatteringAnisotropy,
		0.2f,
		-0.99f, 0.99f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::Float4Variable>(
		"Inscattering Cubemap Tint",
		"inscatteringTint",
		"RGB tint for the inscattering cubemap with alpha for intensity",
		&settings.inscatteringTint,
		float4{ 1.0f, 1.0f, 1.0f, 1.0f }));

	registry->RegisterVariable(std::make_shared<WeatherVariables::WeatherVariable<bool>>(
		"respectVanillaFogFade",
		"Apply Vanilla Fade",
		"Apply vanilla fade brightness to exponential height fog",
		(bool*)&settings.respectVanillaFogFade,
		false,
		[](const bool& from, const bool& to, float factor) {
			return factor > 0.5f ? to : from;
		}));

	registry->RegisterVariable(std::make_shared<WeatherVariables::WeatherVariable<bool>>(
		"disableVanillaFog",
		"Disable Vanilla Fog",
		"Disables vanilla fog entirely, only exponential height fog is applied",
		(bool*)&settings.disableVanillaFog,
		false,
		[](const bool& from, const bool& to, float factor) {
			return factor > 0.5f ? to : from;
		}));

	registry->RegisterVariable(std::make_shared<WeatherVariables::WeatherVariable<bool>>(
		"volumetricFogEnabled",
		"Enable Volumetric Fog",
		"Enables froxel-based volumetric fog for exponential height fog",
		(bool*)&settings.volumetricFogEnabled,
		false,
		[](const bool& from, const bool& to, float factor) {
			return factor > 0.5f ? to : from;
		}));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Volumetric View Distance",
		"volumetricFogDistance",
		"Maximum distance covered by exponential height volumetric fog",
		&settings.volumetricFogDistance,
		60000.0f,
		1000.0f, 200000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Volumetric Start Distance",
		"volumetricFogStartDistance",
		"Start distance of volumetric fog from the camera",
		&settings.volumetricFogStartDistance,
		0.0f,
		0.0f, 200000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Volumetric Near Fade In Distance",
		"volumetricFogNearFadeInDistance",
		"Distance over which volumetric fog fades in near the camera",
		&settings.volumetricFogNearFadeInDistance,
		1000.0f,
		0.0f, 20000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Volumetric Extinction Scale",
		"volumetricFogExtinctionScale",
		"Scale applied to volumetric fog extinction",
		&settings.volumetricFogExtinctionScale,
		1.0f,
		0.0f, 10.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Volumetric Scattering Distribution",
		"volumetricFogScatteringDistribution",
		"Henyey-Greenstein scattering distribution for volumetric fog",
		&settings.volumetricFogScatteringDistribution,
		0.2f,
		-0.9f, 0.9f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Volumetric Directional Scattering Intensity",
		"volumetricDirectionalScatteringIntensity",
		"Scale applied to volumetric fog directional light scattering",
		&settings.volumetricDirectionalScatteringIntensity,
		1.0f,
		0.0f, 10.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::Float4Variable>(
		"Volumetric Albedo",
		"volumetricFogAlbedo",
		"Volumetric fog albedo color",
		&settings.volumetricFogAlbedo,
		float4{ 1.0f, 1.0f, 1.0f, 1.0f }));

	registry->RegisterVariable(std::make_shared<WeatherVariables::Float4Variable>(
		"Volumetric Emissive",
		"volumetricFogEmissive",
		"Volumetric fog emissive color",
		&settings.volumetricFogEmissive,
		float4{ 0.0f, 0.0f, 0.0f, 0.0f }));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Volumetric Sky Lighting Intensity",
		"volumetricSkyLightingIntensity",
		"Scale applied to volumetric fog sky lighting",
		&settings.volumetricSkyLightingIntensity,
		1.0f,
		0.0f, 10.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Volumetric Local Light Scattering Intensity",
		"volumetricLocalLightScatteringIntensity",
		"Scale applied to volumetric fog local light scattering",
		&settings.volumetricLocalLightScatteringIntensity,
		1.0f,
		0.0f, 100.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"vanillaFogStrength",
		"Weather Fog Strength",
		"Scales the fog density derived from the active weather when following vanilla fog",
		&settings.vanillaFogStrength,
		1.25f,
		0.0f, 4.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"fogLightingInfluence",
		"Weather Lighting Influence",
		"How strongly sun, sky and local lights shape fog that follows vanilla fog",
		&settings.fogLightingInfluence,
		0.35f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"distanceHazeMaxOpacity",
		"Haze Maximum Opacity",
		"Maximum opacity of distance haze added at all heights",
		&settings.distanceHazeMaxOpacity,
		0.0f,
		0.0f, 1.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"distanceHazeStartDistance",
		"Haze Start Distance",
		"Horizontal distance from the camera where distance haze begins",
		&settings.distanceHazeStartDistance,
		15000.0f,
		0.0f, 200000.0f));

	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"distanceHazeFadeDistance",
		"Haze Fade Distance",
		"Distance over which distance haze reaches its maximum opacity",
		&settings.distanceHazeFadeDistance,
		60000.0f,
		1.0f, 200000.0f));
}
#undef I18N_KEY_PREFIX
