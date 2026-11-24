#include "ExponentialHeightFog.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/IBL.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/PhysicalSky.h"
#include "Features/Skylighting.h"
#include "Features/TerrainShadows.h"
#include "Features/ViewAtmosphere.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

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
	fogHeight2,
	fogHeightFalloff2,
	fogDensity2,
	directionalInscatteringMultiplier,
	directionalInscatteringAnisotropy,
	useSkyIBL,
	inscatteringTint,
	cubemapMipLevel,
	sunlightAttenuationAmount,
	respectVanillaFogFade,
	disableVanillaFog,
	fogInscatteringColor,
	originalFogColorAmount,
	originalFogColorOverride,
	volumetricFogEnabled,
	volumetricFogStartDistance,
	volumetricFogNearFadeInDistance,
	volumetricFogExtinctionScale,
	volumetricFogScatteringDistribution,
	volumetricFogAlbedo,
	volumetricFogEmissive,
	volumetricDirectionalScatteringIntensity,
	volumetricSkyLightingIntensity,
	volumetricFogNoiseScale,
	volumetricFogNoiseThreshold,
	volumetricFogNoiseVelocity,
	volumetricLocalLightScatteringIntensity,
	useVanillaFogSettings,
	vanillaFogStrength,
	fogLightingInfluence,
	distanceHazeMaxOpacity,
	distanceHazeStartDistance,
	distanceHazeFadeDistance,
	interiorFogStrength,
	interiorFogBrightness,
	interiorFogTint)

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
		a_settings.interiorFogStrength = ClampFinite(a_settings.interiorFogStrength, 0.0f, 4.0f, defaults.interiorFogStrength);
		a_settings.interiorFogBrightness = ClampFinite(a_settings.interiorFogBrightness, 0.0f, 4.0f, defaults.interiorFogBrightness);
		for (int i = 0; i < 4; ++i)
			(&a_settings.interiorFogTint.x)[i] = ClampFinite((&a_settings.interiorFogTint.x)[i], 0.0f, 1.0f, (&defaults.interiorFogTint.x)[i]);
	}

	void ApplyInteriorFog(ExponentialHeightFog::Settings& a_data)
	{
		if (!Util::IsInterior())
			return;
		a_data.vanillaFogStrength *= a_data.interiorFogStrength;
		a_data.fogDensity *= a_data.interiorFogStrength;
		a_data.fogDensity2 *= a_data.interiorFogStrength;

		const float tintAmount = a_data.interiorFogTint.w;
		float4 color{
			a_data.interiorFogBrightness * std::lerp(1.0f, a_data.interiorFogTint.x, tintAmount),
			a_data.interiorFogBrightness * std::lerp(1.0f, a_data.interiorFogTint.y, tintAmount),
			a_data.interiorFogBrightness * std::lerp(1.0f, a_data.interiorFogTint.z, tintAmount), 1.0f
		};
		for (auto* target : { &a_data.vanillaFogNearColor, &a_data.vanillaFogFarColor }) {
			target->x *= color.x;
			target->y *= color.y;
			target->z *= color.z;
		}
		globals::features::linearLighting.SRGBToWorking(&color.x);
		for (auto* target : { &a_data.inscatteringTint, &a_data.fogInscatteringColor }) {
			target->x *= color.x;
			target->y *= color.y;
			target->z *= color.z;
		}
	}

	bool CanReuseFogHistory(const ExponentialHeightFog::Settings& current, const ExponentialHeightFog::Settings& previous)
	{
		using Settings = ExponentialHeightFog::Settings;
		if (current.useVanillaFogSettings != previous.useVanillaFogSettings)
			return false;
		for (const auto field : {
				 &Settings::vanillaFogStrength, &Settings::fogLightingInfluence,
				 &Settings::fogDensity, &Settings::fogHeight, &Settings::fogHeightFalloff,
				 &Settings::fogDensity2, &Settings::fogHeight2, &Settings::fogHeightFalloff2,
				 &Settings::volumetricFogStartDistance,
				 &Settings::volumetricFogNearFadeInDistance, &Settings::volumetricFogExtinctionScale }) {
			const float value = current.*field;
			const float previousValue = previous.*field;
			if (std::abs(value - previousValue) > kMaximumWeatherHistoryChange * std::max(std::abs(value), std::abs(previousValue)))
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
	auto& linearLighting = globals::features::linearLighting;
	linearLighting.SRGBToWorking(&data.inscatteringTint.x);
	linearLighting.SRGBToWorking(&data.fogInscatteringColor.x);
	linearLighting.SRGBToWorking(&data.originalFogColorOverride.x);
	linearLighting.SRGBToWorking(&data.volumetricFogAlbedo.x);
	linearLighting.SRGBToWorking(&data.volumetricFogEmissive.x);
	data.vanillaFogDensity = 0.0f;

	if (IsSuppressed()) {
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
		const auto& tint = settings.originalFogColorOverride;
		const float tintLuminance = 0.2126f * tint.x + 0.7152f * tint.y + 0.0722f * tint.z;
		if (data.useVanillaFogSettings && !Util::IsInterior() && tint.w > 0.0f && tintLuminance > 0.0f) {
			for (auto* target : { &data.vanillaFogNearColor, &data.vanillaFogFarColor }) {
				const float scale = (0.2126f * target->x + 0.7152f * target->y + 0.0722f * target->z) / tintLuminance;
				target->x = std::lerp(target->x, tint.x * scale, tint.w);
				target->y = std::lerp(target->y, tint.y * scale, tint.w);
				target->z = std::lerp(target->z, tint.z * scale, tint.w);
			}
		}
	}
	ApplyInteriorFog(data);
	if (!data.useVanillaFogSettings)
		return data;

	data.disableVanillaFog = 1;
	data.startDistance = fogNear;
	const float targetOpacity = data.vanillaFogMaxOpacity * kReferenceOpacityFraction;
	const float normalizedReference = std::pow(targetOpacity, 1.0f / fogPower);
	const float referenceDistance = std::max((fogFar - fogNear) * normalizedReference, kMinimumFogRange);
	data.vanillaFogDensity = -std::log(std::max(1.0f - targetOpacity, kMinimumFogTransmittance)) / (referenceDistance * kAnalyticalExtinctionScale);
	return data;
}

bool ExponentialHeightFog::IsSuppressed() const
{
	// The world/local map keeps its vanilla fog; height fog tuned for eye level washes it out
	return globals::state->isMapMenuOpen;
}

void ExponentialHeightFog::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable_exp_height_fog"), "Enable Exponential Height Fog"), (bool*)&settings.enabled);
	if (!ImGui::BeginTabBar("##ExponentialHeightFogTabs"))
		return;

	if (ImGui::BeginTabItem(T(TKEY("tab_general"), "General"))) {
		ImGui::BeginDisabled(settings.enabled == 0);
		DrawHeightFogSettings();
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

void ExponentialHeightFog::DrawHeightFogSettings()
{
	ImGui::Checkbox(T(TKEY("use_vanilla_fog_settings"), "Follow Vanilla Fog"), (bool*)&settings.useVanillaFogSettings);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_vanilla_fog_settings_tooltip"), "Derives fog density, start distance, and colors from the active weather while keeping exponential height falloff. Weather Fog Strength scales density. Replaces vanilla distance fog."));
	}
	ImGui::BeginDisabled(settings.useVanillaFogSettings == 0);
	ImGui::SliderFloat(T(TKEY("vanilla_strength"), "Weather Fog Strength"), &settings.vanillaFogStrength, 0.0f, 4.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("lighting_influence"), "Weather Lighting Influence"), &settings.fogLightingInfluence, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("interiors"), "Interiors"));
	ImGui::SliderFloat(T(TKEY("interior_fog_strength"), "Interior Fog Strength"), &settings.interiorFogStrength, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("interior_fog_strength_tooltip"), "Scales fog density inside interiors only. Below 1 thins the fog a cell's lighting template asks for."));
	}
	ImGui::SliderFloat(T(TKEY("interior_fog_brightness"), "Interior Fog Brightness"), &settings.interiorFogBrightness, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("interior_fog_brightness_tooltip"), "Scales the fog color inside interiors only. Below 1 darkens fog that washes out dim rooms."));
	}
	ImGui::ColorEdit4(T(TKEY("interior_fog_tint"), "Interior Fog Tint"), (float*)&settings.interiorFogTint);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("interior_fog_tint_tooltip"), "Tints the fog color inside interiors. Alpha is how much of the tint applies."));
	}

	ImGui::SeparatorText(T(TKEY("density_height"), "Density and Height"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::SliderFloat(T(TKEY("fog_density"), "Fog Density"), &settings.fogDensity, 0.0f, 1.0f, "%.3f");
	ImGui::SliderFloat(T(TKEY("start_distance"), "Start Distance"), &settings.startDistance, 0.0f, 100000.0f, "%.1f");
	ImGui::EndDisabled();
	ImGui::SliderFloat(T(TKEY("fog_height"), "Fog Height"), &settings.fogHeight, -22000.0f, 22000.0f, "%.1f");
	ImGui::SliderFloat(T(TKEY("fog_height_falloff"), "Fog Height Falloff"), &settings.fogHeightFalloff, 0.001f, 2.0f, "%.3f");
	if (ImGui::TreeNode(T(TKEY("second_fog_layer"), "Second Fog Layer"))) {
		ImGui::SliderFloat(T(TKEY("fog_height_2"), "Fog Height 2"), &settings.fogHeight2, -22000.0f, 22000.0f, "%.1f");
		ImGui::SliderFloat(T(TKEY("fog_height_falloff_2"), "Fog Height Falloff 2"), &settings.fogHeightFalloff2, 0.001f, 2.0f, "%.3f");
		ImGui::SliderFloat(T(TKEY("fog_density_2"), "Fog Density 2"), &settings.fogDensity2, 0.0f, 1.0f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("second_fog_layer_tooltip"),
								  "Adds a second stacked exponential height fog layer with its own base height, density and height falloff.\n"
								  "The two line integrals are summed.\n"
								  "Use it for high-altitude haze above the ground layer or a distinct low-lying ground fog."));
		}
		ImGui::TreePop();
	}
	ImGui::BeginDisabled(settings.useVanillaFogSettings == 0 && settings.volumetricFogEnabled == 0);
	ImGui::SliderFloat(T(TKEY("volumetric_extinction_scale"), "Extinction Scale"), &settings.volumetricFogExtinctionScale, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_extinction_scale_tooltip"), "Scales fog density when following vanilla fog, including when volumetric fog is off. In manual mode, affects only volumetric fog."));
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("color_lighting"), "Color and Lighting"));
	ImGui::ColorEdit4(T(TKEY("fog_inscattering_color"), "Fog Inscattering Color"), (float*)&settings.fogInscatteringColor);
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::SliderFloat(T(TKEY("original_fog_color_amount"), "Original Fog Color Amount"), &settings.originalFogColorAmount, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();
	ImGui::ColorEdit4(T(TKEY("original_fog_color_override"), "Original Fog Color Override"), (float*)&settings.originalFogColorOverride);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("original_fog_color_override_tooltip"),
							  "Replaces the weather's fog colour. Alpha blends from the weather colour (0) to this colour (1).\n"
							  "With weather fog settings, outdoors, only the hue changes: the weather fog keeps its brightness."));
	}
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0 && settings.fogLightingInfluence <= 0.0f);
	ImGui::SliderFloat(T(TKEY("dir_inscattering_mul"), "Directional Light Inscattering Multiplier"), &settings.directionalInscatteringMultiplier, 0.0f, 10.0f, "%.2f");
	ImGui::BeginDisabled(settings.directionalInscatteringMultiplier <= 0.0f);
	ImGui::SliderFloat(T(TKEY("dir_inscattering_anisotropy"), "Directional Light Inscattering Anisotropy"), &settings.directionalInscatteringAnisotropy, -0.99f, 0.99f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dir_inscattering_anisotropy_tooltip"),
							  "Controls the asymmetry of inscattering via the Henyey-Greenstein phase function.\n"
							  "Positive values produce forward scattering (glow around sun).\n"
							  "Zero is isotropic. Negative values produce back scattering."));
	}
	ImGui::EndDisabled();
	ImGui::SliderFloat(T(TKEY("sunlight_attenuation"), "Sunlight Attenuation Amount"), &settings.sunlightAttenuationAmount, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("vanilla_fog"), "Vanilla Fog"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::Checkbox(T(TKEY("disable_vanilla_fog"), "Disable Vanilla Fog"), (bool*)&settings.disableVanillaFog);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("disable_vanilla_fog_tooltip"), "Disables the vanilla fog entirely. Only exponential height fog will be applied."));
	}
	ImGui::EndDisabled();
	ImGui::Checkbox(T(TKEY("apply_vanilla_fade"), "Apply Vanilla Fade"), (bool*)&settings.respectVanillaFogFade);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("apply_vanilla_fade_tooltip"), "Applies vanilla fade brightness to exponential height fog."));
	}

	ImGui::SeparatorText(T(TKEY("inscattering_cubemap"), "Inscattering Cubemap"));
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::BeginDisabled(!globals::features::ibl.loaded);
	ImGui::Checkbox(T(TKEY("use_sky_ibl"), "Use Sky IBL for Exterior Inscattering"), (bool*)&settings.useSkyIBL);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_sky_ibl_tooltip"), "Adds the sky IBL color to the fog inscattering in exteriors. Requires the Image Based Lighting feature."));
	}
	ImGui::EndDisabled();
	ImGui::BeginDisabled(!globals::features::dynamicCubemaps.loaded);
	ImGui::Checkbox(T(TKEY("use_dynamic_cubemaps"), "Use Dynamic Cubemaps for Interior Inscattering"), (bool*)&settings.useDynamicCubemaps);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("use_dynamic_cubemaps_tooltip"), "Adds the dynamic environment cubemap color to the fog inscattering in interiors."));
	}
	ImGui::EndDisabled();
	const bool hasCubemapInscattering = (settings.useSkyIBL != 0 && globals::features::ibl.loaded) ||
	                                    (settings.useDynamicCubemaps != 0 && globals::features::dynamicCubemaps.loaded);
	ImGui::BeginDisabled(!hasCubemapInscattering);
	ImGui::ColorEdit4(T(TKEY("inscattering_cubemap_tint"), "Inscattering Cubemap Tint"), (float*)&settings.inscatteringTint);
	ImGui::BeginDisabled(settings.inscatteringTint.w <= 0.0f);
	ImGui::SliderFloat(T(TKEY("cubemap_mip_level"), "Cubemap Mip Level"), &settings.cubemapMipLevel, 1.0f, 8.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	ImGui::SeparatorText(T(TKEY("distance_haze"), "Distance Haze"));
	ImGui::SliderFloat(T(TKEY("distance_haze_max_opacity"), "Haze Maximum Opacity"), &settings.distanceHazeMaxOpacity, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_max_opacity_tooltip"), "Adds haze at all heights using the fog colors above, while preserving dense height fog. Zero disables distance haze."));
	}
	ImGui::SliderFloat(T(TKEY("distance_haze_start_distance"), "Haze Start Distance"), &settings.distanceHazeStartDistance, 0.0f, kMaxDistanceHazeDistance, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_start_distance_tooltip"), "Horizontal distance from the camera where haze begins, in game units. Independent of height fog Start Distance."));
	}
	ImGui::SliderFloat(T(TKEY("distance_haze_fade_distance"), "Haze Fade Distance"), &settings.distanceHazeFadeDistance, kMinDistanceHazeFadeDistance, kMaxDistanceHazeDistance, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_haze_fade_distance_tooltip"), "Horizontal distance beyond Haze Start Distance over which haze smoothly reaches its maximum opacity, in game units."));
	}
}

void ExponentialHeightFog::DrawVolumetricSettings()
{
	ImGui::Checkbox(T(TKEY("enable_volumetric_fog"), "Enable Volumetric Fog"), (bool*)&settings.volumetricFogEnabled);
	ImGui::BeginDisabled(settings.volumetricFogEnabled == 0);
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0);
	ImGui::SliderFloat(T(TKEY("volumetric_start_distance"), "Volumetric Start Distance"), &settings.volumetricFogStartDistance, 0.0f, 20000.0f, "%.0f");
	ImGui::SliderFloat(T(TKEY("near_fade_in_distance"), "Near Fade In Distance"), &settings.volumetricFogNearFadeInDistance, 0.0f, 20000.0f, "%.0f");
	ImGui::EndDisabled();
	if (ImGui::TreeNode(T(TKEY("volumetric_noise"), "Volumetric Noise"))) {
		ImGui::SliderFloat(T(TKEY("volumetric_noise_scale"), "Noise Scale"), &settings.volumetricFogNoiseScale, 0.0f, 0.01f, "%.6f");
		ImGui::SliderFloat(T(TKEY("volumetric_noise_threshold"), "Noise Threshold"), &settings.volumetricFogNoiseThreshold, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat3(T(TKEY("volumetric_noise_velocity"), "Noise Velocity"), &settings.volumetricFogNoiseVelocity.x, -1.0f, 1.0f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("volumetric_noise_tooltip"),
								  "Modulates the volumetric fog density with a 3D value noise field.\n"
								  "Noise Scale: spatial frequency of the fog clumps (0 = disabled).\n"
								  "Noise Threshold: soft cutoff that carves clumps out of the noise.\n"
								  "Noise Velocity: animation drift of the noise field, scaled by time."));
		}
		ImGui::TreePop();
	}

	ImGui::SeparatorText(T(TKEY("color_lighting"), "Color and Lighting"));
	ImGui::ColorEdit4(T(TKEY("volumetric_albedo"), "Volumetric Albedo"), (float*)&settings.volumetricFogAlbedo);
	ImGui::ColorEdit4(T(TKEY("volumetric_emissive"), "Volumetric Emissive"), (float*)&settings.volumetricFogEmissive);
	ImGui::BeginDisabled(settings.useVanillaFogSettings != 0 && settings.fogLightingInfluence <= 0.0f);
	ImGui::SliderFloat(T(TKEY("directional_scattering_intensity"), "Directional Scattering Intensity"), &settings.volumetricDirectionalScatteringIntensity, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("sky_lighting_scattering_intensity"), "Sky Lighting Scattering Intensity"), &settings.volumetricSkyLightingIntensity, 0.0f, 10.0f, "%.2f");
	ImGui::BeginDisabled(!globals::features::lightLimitFix.loaded);
	ImGui::SliderFloat(T(TKEY("local_light_scattering_intensity"), "Local Light Scattering Intensity"), &settings.volumetricLocalLightScatteringIntensity, 0.0f, 10.0f, "%.2f");
	ImGui::EndDisabled();
	const bool hasScattering = settings.volumetricDirectionalScatteringIntensity > 0.0f || settings.volumetricSkyLightingIntensity > 0.0f ||
	                           (globals::features::lightLimitFix.loaded && settings.volumetricLocalLightScatteringIntensity > 0.0f);
	ImGui::BeginDisabled(!hasScattering);
	ImGui::SliderFloat(T(TKEY("volumetric_scattering_distribution"), "Volumetric Scattering Distribution"), &settings.volumetricFogScatteringDistribution, -0.9f, 0.9f, "%.2f");
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	ImGui::TextWrapped("%s", T("feature.view_atmosphere.quality_location", "Grid quality and temporal sampling are configured in Atmosphere and Fog."));
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
	if (farMaterialSetupCS) {
		farMaterialSetupCS->Release();
		farMaterialSetupCS = nullptr;
	}
	if (conservativeDepthCS) {
		conservativeDepthCS->Release();
		conservativeDepthCS = nullptr;
	}
	if (farConservativeDepthCS) {
		farConservativeDepthCS->Release();
		farConservativeDepthCS = nullptr;
	}
	if (conservativeDepthGroupCS) {
		conservativeDepthGroupCS->Release();
		conservativeDepthGroupCS = nullptr;
	}
	if (farConservativeDepthGroupCS) {
		farConservativeDepthGroupCS->Release();
		farConservativeDepthGroupCS = nullptr;
	}
	if (lightScatteringCS) {
		lightScatteringCS->Release();
		lightScatteringCS = nullptr;
	}
	if (farLightScatteringCS) {
		farLightScatteringCS->Release();
		farLightScatteringCS = nullptr;
	}
	if (integrationCS) {
		integrationCS->Release();
		integrationCS = nullptr;
	}
	if (farIntegrationCS) {
		farIntegrationCS->Release();
		farIntegrationCS = nullptr;
	}
	if (fogIBLColorCS) {
		fogIBLColorCS->Release();
		fogIBLColorCS = nullptr;
	}
}

void ExponentialHeightFog::EnsureVolumetricResources()
{
	const auto& quality = globals::features::viewAtmosphere.settings;
	uint32_t pixelSize = std::clamp(quality.nearPixelSize, 4u, 64u);
	const uint32_t gridZ = std::clamp(quality.nearSlices, 16u, 160u);
	uint32_t farPixelSize = std::clamp(quality.middlePixelSize, 4u, 64u);
	const uint32_t farGridZ = std::clamp(quality.middleSlices, 16u, 160u);
	float2 screenSz{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	auto renderSize = Util::ConvertToDynamic(screenSz);

	auto getGridSize = [&renderSize](uint32_t a_pixelSize, uint32_t a_gridZ) {
		return DirectX::XMUINT4{
			std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.x / static_cast<float>(a_pixelSize)))),
			std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.y / static_cast<float>(a_pixelSize)))),
			a_gridZ,
			0u
		};
	};
	DirectX::XMUINT4 gridSize = getGridSize(pixelSize, gridZ);

	constexpr uint64_t maxVolumeVoxels = 16ull * 1024ull * 1024ull;
	while (pixelSize < 64u &&
		   static_cast<uint64_t>(gridSize.x) * gridSize.y * gridSize.z > maxVolumeVoxels) {
		pixelSize++;
		gridSize = getGridSize(pixelSize, gridZ);
	}

	// The far volume must be coarser than the near volume.
	farPixelSize = std::max(farPixelSize, pixelSize);
	DirectX::XMUINT4 farGridSize = getGridSize(farPixelSize, farGridZ);
	while (farPixelSize < 64u &&
		   static_cast<uint64_t>(farGridSize.x) * farGridSize.y * farGridSize.z > maxVolumeVoxels / 4ull) {
		farPixelSize++;
		farGridSize = getGridSize(farPixelSize, farGridZ);
	}

	if (vBufferA &&
		currentGridSize.x == gridSize.x && currentGridSize.y == gridSize.y && currentGridSize.z == gridSize.z &&
		currentFarGridSize.x == farGridSize.x && currentFarGridSize.y == farGridSize.y && currentFarGridSize.z == farGridSize.z)
		return;

	currentGridSize = gridSize;
	currentFarGridSize = farGridSize;

	auto make3D = [this](const DirectX::XMUINT4& a_size, const char* a_name, DXGI_FORMAT a_format = DXGI_FORMAT_R16G16B16A16_FLOAT) {
		D3D11_TEXTURE3D_DESC texDesc{};
		texDesc.Width = a_size.x;
		texDesc.Height = a_size.y;
		texDesc.Depth = a_size.z;
		texDesc.MipLevels = 1;
		texDesc.Format = a_format;
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
		uavDesc.Texture3D.WSize = a_size.z;

		auto tex = std::make_unique<Texture3D>(texDesc, a_name);
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
		return tex;
	};

	auto make2D = [this](const DirectX::XMUINT4& a_size, const char* a_name) {
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = a_size.x;
		texDesc.Height = a_size.y;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		auto tex = std::make_unique<Texture2D>(texDesc, a_name);
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
		return tex;
	};

	vBufferA = make3D(gridSize, "ExponentialHeightFog::VBufferA");
	conservativeDepth = make2D(gridSize, "ExponentialHeightFog::ConservativeDepth");
	lightScattering = make3D(gridSize, "ExponentialHeightFog::LightScattering", DXGI_FORMAT_R32G32B32A32_FLOAT);
	integratedLightScattering = make3D(gridSize, "ExponentialHeightFog::IntegratedLightScattering");

	conservativeDepthHistory = make2D(gridSize, "ExponentialHeightFog::ConservativeDepthHistory");
	lightScatteringHistory = make3D(gridSize, "ExponentialHeightFog::LightScatteringHistory", DXGI_FORMAT_R32G32B32A32_FLOAT);

	vBufferAFar = make3D(farGridSize, "ExponentialHeightFog::VBufferAFar");
	conservativeDepthFar = make2D(farGridSize, "ExponentialHeightFog::ConservativeDepthFar");
	lightScatteringFar = make3D(farGridSize, "ExponentialHeightFog::LightScatteringFar", DXGI_FORMAT_R32G32B32A32_FLOAT);
	integratedLightScatteringFar = make3D(farGridSize, "ExponentialHeightFog::IntegratedLightScatteringFar");

	conservativeDepthFarHistory = make2D(farGridSize, "ExponentialHeightFog::ConservativeDepthFarHistory");
	lightScatteringFarHistory = make3D(farGridSize, "ExponentialHeightFog::LightScatteringFarHistory", DXGI_FORMAT_R32G32B32A32_FLOAT);

	hasLightScatteringHistory = false;
	hasConservativeDepthHistory = false;
	hasLightScatteringFarHistory = false;
	hasConservativeDepthFarHistory = false;
	lastPrepassFrame = UINT32_MAX;
}

void ExponentialHeightFog::ReleaseVolumetricResources()
{
	vBufferA.reset();
	vBufferAFar.reset();
	conservativeDepth.reset();
	conservativeDepthHistory.reset();
	conservativeDepthFar.reset();
	conservativeDepthFarHistory.reset();
	lightScattering.reset();
	lightScatteringHistory.reset();
	lightScatteringFar.reset();
	lightScatteringFarHistory.reset();
	integratedLightScattering.reset();
	integratedLightScatteringFar.reset();
	currentGridSize = {};
	currentFarGridSize = {};
	hasLightScatteringHistory = false;
	hasConservativeDepthHistory = false;
	hasLightScatteringFarHistory = false;
	hasConservativeDepthFarHistory = false;
	lastPrepassFrame = UINT32_MAX;
	ID3D11ShaderResourceView* nullSRV = nullptr;
	globals::d3d::context->PSSetShaderResources(19, 1, &nullSRV);
	globals::d3d::context->PSSetShaderResources(22, 1, &nullSRV);
}

void ExponentialHeightFog::BindIntegratedLightScattering()
{
	ID3D11ShaderResourceView* srv = integratedLightScattering ? integratedLightScattering->srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(19, 1, &srv);
	ID3D11ShaderResourceView* farSrv = integratedLightScatteringFar ? integratedLightScatteringFar->srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(22, 1, &farSrv);
}

ID3D11ComputeShader* ExponentialHeightFog::GetMaterialSetupCS()
{
	if (!materialSetupCS)
		materialSetupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", {}, "cs_5_0"));
	return materialSetupCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetConservativeDepthCS(bool a_groupReduce)
{
	if (a_groupReduce) {
		if (!conservativeDepthGroupCS)
			conservativeDepthGroupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", { { "GROUP_REDUCE", "" } }, "cs_5_0"));
		return conservativeDepthGroupCS;
	}
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
		if (globals::features::physicalSky.loaded) {
			defines.emplace_back("PHYSICAL_SKY", "");
		}
		lightScatteringCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", defines, "cs_5_0"));
	}
	return lightScatteringCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarLightScatteringCS()
{
	if (!farLightScatteringCS) {
		std::vector<std::pair<const char*, const char*>> defines;
		defines.emplace_back("VOLUMETRIC_FOG_FAR_GRID", "");
		if (globals::features::lightLimitFix.loaded) {
			defines.emplace_back("LIGHT_LIMIT_FIX", "");
		}
		if (globals::features::terrainShadows.loaded) {
			defines.emplace_back("TERRAIN_SHADOWS", "");
		}
		if (globals::features::cloudShadows.loaded) {
			defines.emplace_back("CLOUD_SHADOWS", "");
		}
		if (globals::features::physicalSky.loaded) {
			defines.emplace_back("PHYSICAL_SKY", "");
		}
		farLightScatteringCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", defines, "cs_5_0"));
	}
	return farLightScatteringCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarMaterialSetupCS()
{
	if (!farMaterialSetupCS)
		farMaterialSetupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0"));
	return farMaterialSetupCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarConservativeDepthCS(bool a_groupReduce)
{
	if (a_groupReduce) {
		if (!farConservativeDepthGroupCS)
			farConservativeDepthGroupCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" }, { "GROUP_REDUCE", "" } }, "cs_5_0"));
		return farConservativeDepthGroupCS;
	}
	if (!farConservativeDepthCS)
		farConservativeDepthCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0"));
	return farConservativeDepthCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetIntegrationCS()
{
	if (!integrationCS)
		integrationCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", {}, "cs_5_0"));
	return integrationCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFarIntegrationCS()
{
	if (!farIntegrationCS)
		farIntegrationCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", { { "VOLUMETRIC_FOG_FAR_GRID", "" } }, "cs_5_0"));
	return farIntegrationCS;
}

ID3D11ComputeShader* ExponentialHeightFog::GetFogIBLColorCS()
{
	if (!fogIBLColorCS)
		fogIBLColorCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\ExponentialHeightFog\\FogIBLColorCS.hlsl", {}, "cs_5_0"));
	return fogIBLColorCS;
}

void ExponentialHeightFog::UpdateFogIBLColor()
{
	auto* shader = GetFogIBLColorCS();
	if (!shader)
		return;

	auto context = globals::d3d::context;
	if (!fogIBLColor) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = 1;
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		fogIBLColor = std::make_unique<Texture2D>(desc, "ExponentialHeightFog::FogIBLColor");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		fogIBLColor->CreateSRV(srvDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		fogIBLColor->CreateUAV(uavDesc);
	}

	ID3D11ShaderResourceView* nullView = nullptr;
	context->PSSetShaderResources(21, 1, &nullView);

	ID3D11ShaderResourceView* iblViews[2]{};
	context->PSGetShaderResources(76, 2, iblViews);
	context->CSSetShaderResources(76, 2, iblViews);
	for (auto* view : iblViews) {
		if (view)
			view->Release();
	}

	ID3D11Buffer* sharedBuffers[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);
	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	context->CSSetConstantBuffers(12, 1, frameBuffers);

	ID3D11UnorderedAccessView* uav = fogIBLColor->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch(1, 1, 1);

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	ID3D11ShaderResourceView* nullViews[2]{};
	context->CSSetShaderResources(76, 2, nullViews);
	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11ShaderResourceView* view = fogIBLColor->srv.get();
	context->PSSetShaderResources(21, 1, &view);
}

void ExponentialHeightFog::Prepass()
{
	if (settings.enabled && !settings.useVanillaFogSettings && settings.originalFogColorAmount > 0.0f && globals::features::ibl.loaded && !IsSuppressed())
		UpdateFogIBLColor();

	const auto& quality = globals::features::viewAtmosphere.settings;
	if (globals::features::viewAtmosphere.IsPrepared()) {
		if (vBufferA || vBufferAFar)
			ReleaseVolumetricResources();
		return;
	}
	if (!settings.enabled || !settings.volumetricFogEnabled || settings.volumetricFogExtinctionScale <= 0.0f) {
		ReleaseVolumetricResources();
		return;
	}

	if (IsSuppressed())
		return;

	const auto cameraData = Util::GetCameraData();
	const float volumeStart = settings.useVanillaFogSettings ? 0.0f : std::max(settings.volumetricFogStartDistance, 0.0f);
	if (quality.localRange <= std::max(cameraData.y, volumeStart) + 10.0f) {
		ReleaseVolumetricResources();
		return;
	}
	const Settings frameSettings = GetCommonBufferData();
	const float fogDensity = frameSettings.useVanillaFogSettings ? frameSettings.vanillaFogDensity * frameSettings.vanillaFogStrength : frameSettings.fogDensity;
	if (fogDensity <= 0.0f && frameSettings.fogDensity2 <= 0.0f) {
		hasLightScatteringHistory = false;
		hasConservativeDepthHistory = false;
		hasLightScatteringFarHistory = false;
		hasConservativeDepthFarHistory = false;
		lastPrepassFrame = UINT32_MAX;
		ID3D11ShaderResourceView* nullViews[1] = { nullptr };
		globals::d3d::context->PSSetShaderResources(19, 1, nullViews);
		globals::d3d::context->PSSetShaderResources(22, 1, nullViews);
		return;
	}

	const double nearPlane = std::max(static_cast<double>(cameraData.y), static_cast<double>(volumeStart));
	const double totalFarPlane = std::max(nearPlane + 1.0, static_cast<double>(quality.localRange));
	const double nearEndDepth = std::min(
		std::max(static_cast<double>(std::max(quality.nearRange, 0.0f)), nearPlane + 1.0),
		totalFarPlane);
	const bool farGridEnabled = nearEndDepth + 1.0 < totalFarPlane;

	// From 8-pixel cells up, one thread group per cell reduces the cell's depth footprint in parallel.
	const bool nearGroupReduce = std::clamp(quality.nearPixelSize, 4u, 64u) >= 8;
	const bool farGroupReduce = std::clamp(quality.middlePixelSize, 4u, 64u) >= 8;
	auto* conservativeDepthShader = GetConservativeDepthCS(nearGroupReduce);
	auto* materialSetupShader = GetMaterialSetupCS();
	auto* lightScatteringShader = GetLightScatteringCS();
	auto* integrationShader = GetIntegrationCS();
	if (!conservativeDepthShader || !materialSetupShader || !lightScatteringShader || !integrationShader) {
		ReleaseVolumetricResources();
		return;
	}
	if (farGridEnabled && (!GetFarConservativeDepthCS(farGroupReduce) || !GetFarMaterialSetupCS() || !GetFarLightScatteringCS() || !GetFarIntegrationCS())) {
		ReleaseVolumetricResources();
		return;
	}
	EnsureVolumetricResources();
	if (lastPrepassFrame == UINT32_MAX || !CanReuseFogHistory(frameSettings, previousFogSettings)) {
		hasLightScatteringHistory = false;
		hasLightScatteringFarHistory = false;
	}
	previousFogSettings = frameSettings;

	ID3D11ShaderResourceView* directionalShadowLightData = globals::deferred && globals::deferred->directionalShadowLights ? globals::deferred->directionalShadowLights->srv.get() : nullptr;
	auto* directionalShadowMap = globals::deferred ? globals::deferred->GetDirectionalShadowMap() : nullptr;
	auto& lightLimitFix = globals::features::lightLimitFix;
	const bool hasLocalLightData =
		lightLimitFix.loaded &&
		lightLimitFix.lights &&
		lightLimitFix.lightIndexList &&
		lightLimitFix.lightGrid;
	const bool hasLocalShadowData = hasLocalLightData && lightLimitFix.IsLocalShadowCacheActive() && lightLimitFix.localShadowBuffer;
	auto [engineLocalShadowMap, localShadowBuffer] = globals::features::viewAtmosphere.PrepareLocalShadows(hasLocalLightData);
	auto* depthSrv = Util::GetCurrentSceneDepthSRV(true);
	auto& ibl = globals::features::ibl;
	auto& physicalSky = globals::features::physicalSky;
	auto& skylighting = globals::features::skylighting;
	const bool hasIBL = ibl.loaded &&
	                    ibl.settings.EnableIBL != 0 &&
	                    !ibl.IsDisabledForCurrentScene() &&
	                    ibl.envIBLTexture &&
	                    ibl.skyIBLTexture;
	const bool hasPhysicalSky = physicalSky.loaded && physicalSky.texTrLut;
	const bool hasSkylighting = skylighting.loaded && skylighting.texProbeArray;

	const bool temporalReprojection = Util::GetTemporal();
	const bool temporalHistoryValid =
		temporalReprojection &&
		hasLightScatteringHistory &&
		lastPrepassFrame != UINT32_MAX &&
		globals::state->frameCount == lastPrepassFrame + 1u;
	const bool temporalHistoryValidFar =
		temporalReprojection &&
		hasLightScatteringFarHistory &&
		lastPrepassFrame != UINT32_MAX &&
		globals::state->frameCount == lastPrepassFrame + 1u;

	auto computeGridZParams = [](double a_nearPlane, double a_farPlane, uint32_t a_gridZ, float a_distributionScale) {
		constexpr double kNearOffset = 0.095 * 100.0;
		// The offset must stay inside the slab: a fog start at or past Near Grid End leaves a 1-unit near slab.
		const double nearWithOffset = a_nearPlane + std::min(kNearOffset, 0.5 * (a_farPlane - a_nearPlane));
		const double depthDistributionScale = std::max(static_cast<double>(a_distributionScale), static_cast<double>(a_gridZ) / 120.0);
		const double farExp = std::exp2(std::min(static_cast<double>(a_gridZ) / depthDistributionScale, 120.0));
		const double gridZOffset = (a_farPlane - nearWithOffset * farExp) / (a_farPlane - nearWithOffset);
		const double gridZScale = (1.0 - gridZOffset) / nearWithOffset;
		return DirectX::XMFLOAT4{
			static_cast<float>(gridZScale),
			static_cast<float>(gridZOffset),
			static_cast<float>(depthDistributionScale),
			0.0f
		};
	};

	const float nearFadeInDistanceInv = !settings.useVanillaFogSettings && settings.volumetricFogNearFadeInDistance > 0.0f ? 1.0f / settings.volumetricFogNearFadeInDistance : 100000000.0f;

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
			(hasLocalLightData ? 32u : 0u) |
			(hasLocalShadowData ? 64u : 0u) |
			(engineLocalShadowMap ? 128u : 0u)
	};
	cb.invGridSizeAndNearFade = {
		1.0f / static_cast<float>(currentGridSize.x),
		1.0f / static_cast<float>(currentGridSize.y),
		1.0f / static_cast<float>(currentGridSize.z),
		nearFadeInDistanceInv
	};
	cb.gridZParams = computeGridZParams(nearPlane, nearEndDepth, currentGridSize.z, quality.depthDistribution);

	cb.farGridSizeAndFlags = {
		currentFarGridSize.x,
		currentFarGridSize.y,
		currentFarGridSize.z,
		(directionalShadowMap && directionalShadowLightData ? 1u : 0u) |
			(depthSrv ? 2u : 0u) |
			(hasIBL ? 4u : 0u) |
			(hasSkylighting ? 8u : 0u) |
			(depthSrv && temporalHistoryValidFar && hasConservativeDepthFarHistory ? 16u : 0u)
	};
	cb.farInvGridSizeAndNearFade = {
		1.0f / static_cast<float>(currentFarGridSize.x),
		1.0f / static_cast<float>(currentFarGridSize.y),
		1.0f / static_cast<float>(currentFarGridSize.z),
		nearFadeInDistanceInv
	};
	cb.farGridZParams = computeGridZParams(nearEndDepth, totalFarPlane, currentFarGridSize.z, quality.depthDistribution);
	cb.farRange = { static_cast<float>(nearEndDepth), static_cast<float>(totalFarPlane), 0.0f, 0.0f };

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
		temporalHistoryValid ? std::clamp(quality.historyWeight, 0.0f, 0.99f) : 0.0f,
		static_cast<float>(std::clamp(quality.historyMissSamples, 1u, 16u)),
		temporalHistoryValidFar ? std::clamp(quality.historyWeight, 0.0f, 0.99f) : 0.0f,
		0.0f
	};
	cb.jitterParameters = {
		temporalReprojection ? std::max(quality.sampleJitter, 0.0f) : 0.0f,
		static_cast<float>(globals::state->frameCount % 8u),
		0.0f,
		0.0f
	};
	volumetricFogCB->Update(cb);

	auto context = globals::d3d::context;
	ID3D11Buffer* cbuffers[1]{ volumetricFogCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbuffers);
	context->CSSetConstantBuffers(1, 1, &localShadowBuffer);

	ID3D11Buffer* sharedBuffers[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);

	ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
	context->CSSetConstantBuffers(12, 1, frameBuffers);

	ID3D11SamplerState* samplers[2]{ linearSampler.get(), shadowSampler.get() };
	context->CSSetSamplers(0, 2, samplers);

	context->CSSetShaderResources(17, 1, &depthSrv);
	ID3D11ShaderResourceView* skylightingSrv = hasSkylighting ? skylighting.texProbeArray->srv.get() : nullptr;
	ID3D11ShaderResourceView* iblSrvs[2]{
		hasIBL ? ibl.envIBLTexture->srv.get() : nullptr,
		hasIBL ? ibl.skyIBLTexture->srv.get() : nullptr
	};
	context->CSSetShaderResources(50, 1, &skylightingSrv);
	ID3D11ShaderResourceView* physicalSkySrvs[4]{
		hasPhysicalSky ? physicalSky.texTrLut->srv.get() : nullptr,
		physicalSky.loaded && physicalSky.texSvLut ? physicalSky.texSvLut->srv.get() : nullptr,
		physicalSky.loaded && physicalSky.texApLut ? physicalSky.texApLut->srv.get() : nullptr,
		physicalSky.loaded && physicalSky.texApShadow ? physicalSky.texApShadow->srv.get() : nullptr
	};
	ID3D11ShaderResourceView* physicalSkyShadowVolumeSrv = physicalSky.loaded && physicalSky.texShadowVolume ? physicalSky.texShadowVolume->srv.get() : nullptr;
	context->CSSetShaderResources(61, 4, physicalSkySrvs);
	context->CSSetShaderResources(76, 2, iblSrvs);
	context->CSSetShaderResources(112, 1, &physicalSkyShadowVolumeSrv);
	ID3D11ShaderResourceView* physicalSkyShadowMeanSrv = physicalSky.loaded && physicalSky.texCloudShadowMean ? physicalSky.texCloudShadowMean->srv.get() : nullptr;
	context->CSSetShaderResources(69, 1, &physicalSkyShadowMeanSrv);
	ID3D11ShaderResourceView* localShadowSrvs[2]{
		hasLocalShadowData ? lightLimitFix.localShadowBuffer->srv.get() : nullptr,
		hasLocalShadowData ? lightLimitFix.localShadowCache->srv.get() : nullptr
	};
	context->CSSetShaderResources(5, 1, &engineLocalShadowMap);
	context->CSSetShaderResources(102, 2, localShadowSrvs);

	struct VolumetricPassDesc
	{
		DirectX::XMUINT4 gridSize;
		Texture3D* vBuffer;
		Texture2D* conservativeDepth;
		Texture2D* conservativeDepthHistory;  // may be null
		Texture3D* scattering;
		Texture3D* scatteringHistory;  // may be null
		Texture3D* integrated;
		ID3D11ComputeShader* materialSetupCS;
		ID3D11ComputeShader* conservativeDepthCS;
		bool groupReduce;
		ID3D11ComputeShader* lightScatteringCS;
		ID3D11ComputeShader* integrationCS;
		bool hasPrevConservativeDepth;
		const char* name;
	};

	auto runVolumetricPass = [&](const VolumetricPassDesc& p) {
		const uint32_t groupX = (p.gridSize.x + 7) / 8;
		const uint32_t groupY = (p.gridSize.y + 7) / 8;
		const uint32_t groupZ = (p.gridSize.z + 3) / 4;

		if (depthSrv) {
			ID3D11UnorderedAccessView* uavs[1]{ p.conservativeDepth->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.conservativeDepthCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			if (p.groupReduce)
				context->Dispatch(p.gridSize.x, p.gridSize.y, 1);
			else
				context->Dispatch(groupX, groupY, 1);
			globals::profiler->EndPass();
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11UnorderedAccessView* uavs[1]{ p.vBuffer->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.materialSetupCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			context->Dispatch(groupX, groupY, groupZ);
			globals::profiler->EndPass();
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11ShaderResourceView* srvs[5]{
				p.vBuffer->srv.get(),
				directionalShadowMap,
				p.scatteringHistory ? p.scatteringHistory->srv.get() : nullptr,
				p.conservativeDepth->srv.get(),
				p.hasPrevConservativeDepth && p.conservativeDepthHistory ? p.conservativeDepthHistory->srv.get() : nullptr
			};
			ID3D11ShaderResourceView* localLightSrvs[3]{
				hasLocalLightData ? lightLimitFix.lights->srv.get() : nullptr,
				hasLocalLightData ? lightLimitFix.lightIndexList->srv.get() : nullptr,
				hasLocalLightData ? lightLimitFix.lightGrid->srv.get() : nullptr
			};
			ID3D11UnorderedAccessView* uavs[1]{ p.scattering->uav.get() };
			context->CSSetShaderResources(0, 5, srvs);
			context->CSSetShaderResources(35, 3, localLightSrvs);
			context->CSSetShaderResources(98, 1, &directionalShadowLightData);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.lightScatteringCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			context->Dispatch(groupX, groupY, groupZ);
			globals::profiler->EndPass();
			uavs[0] = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		}

		{
			ID3D11ShaderResourceView* srvs[1]{ p.scattering->srv.get() };
			ID3D11UnorderedAccessView* uavs[1]{ p.integrated->uav.get() };
			context->CSSetShaderResources(0, 1, srvs);
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->CSSetShader(p.integrationCS, nullptr, 0);
			globals::profiler->BeginPass(p.name);
			context->Dispatch(groupX, groupY, 1);
			globals::profiler->EndPass();
		}
	};

	runVolumetricPass({ currentGridSize,
		vBufferA.get(),
		conservativeDepth.get(),
		temporalHistoryValid && hasConservativeDepthHistory ? conservativeDepthHistory.get() : nullptr,
		lightScattering.get(),
		temporalHistoryValid ? lightScatteringHistory.get() : nullptr,
		integratedLightScattering.get(),
		materialSetupShader,
		conservativeDepthShader,
		nearGroupReduce,
		lightScatteringShader,
		integrationShader,
		temporalHistoryValid && hasConservativeDepthHistory,
		"ExponentialHeightFog::NearVolume" });

	if (farGridEnabled) {
		runVolumetricPass({ currentFarGridSize,
			vBufferAFar.get(),
			conservativeDepthFar.get(),
			temporalHistoryValidFar && hasConservativeDepthFarHistory ? conservativeDepthFarHistory.get() : nullptr,
			lightScatteringFar.get(),
			temporalHistoryValidFar ? lightScatteringFarHistory.get() : nullptr,
			integratedLightScatteringFar.get(),
			GetFarMaterialSetupCS(),
			GetFarConservativeDepthCS(farGroupReduce),
			farGroupReduce,
			GetFarLightScatteringCS(),
			GetFarIntegrationCS(),
			temporalHistoryValidFar && hasConservativeDepthFarHistory,
			"ExponentialHeightFog::FarVolume" });
	} else {
		const float clearValue[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
		context->ClearUnorderedAccessViewFloat(integratedLightScatteringFar->uav.get(), clearValue);
		hasLightScatteringFarHistory = false;
		hasConservativeDepthFarHistory = false;
	}

	ID3D11ShaderResourceView* nullSrvs[5]{ nullptr, nullptr, nullptr, nullptr, nullptr };
	ID3D11ShaderResourceView* nullDepthSrv[1]{ nullptr };
	ID3D11UnorderedAccessView* nullUav[1]{ nullptr };
	ID3D11SamplerState* nullSamplers[2]{ nullptr, nullptr };
	ID3D11Buffer* nullCb[1]{ nullptr };
	context->CSSetShaderResources(0, 5, nullSrvs);
	context->CSSetShaderResources(5, 1, nullSrvs);
	context->CSSetShaderResources(17, 1, nullDepthSrv);
	context->CSSetShaderResources(35, 3, nullSrvs);
	context->CSSetShaderResources(50, 1, nullDepthSrv);
	context->CSSetShaderResources(76, 2, nullSrvs);
	context->CSSetShaderResources(98, 1, nullSrvs);
	context->CSSetShaderResources(102, 2, nullSrvs);
	context->CSSetShaderResources(112, 1, nullSrvs);
	context->CSSetShaderResources(69, 1, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
	context->CSSetSamplers(0, 2, nullSamplers);
	context->CSSetConstantBuffers(0, 1, nullCb);
	context->CSSetConstantBuffers(1, 1, nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	if (temporalReprojection) {
		std::swap(lightScattering, lightScatteringHistory);
		hasLightScatteringHistory = true;
		if (depthSrv) {
			std::swap(conservativeDepth, conservativeDepthHistory);
			hasConservativeDepthHistory = true;
		} else {
			hasConservativeDepthHistory = false;
		}
		if (farGridEnabled) {
			std::swap(lightScatteringFar, lightScatteringFarHistory);
			hasLightScatteringFarHistory = true;
			if (depthSrv) {
				std::swap(conservativeDepthFar, conservativeDepthFarHistory);
				hasConservativeDepthFarHistory = true;
			} else {
				hasConservativeDepthFarHistory = false;
			}
		} else {
			hasLightScatteringFarHistory = false;
			hasConservativeDepthFarHistory = false;
		}
	} else {
		hasLightScatteringHistory = false;
		hasConservativeDepthHistory = false;
		hasLightScatteringFarHistory = false;
		hasConservativeDepthFarHistory = false;
	}

	lastPrepassFrame = globals::state->frameCount;
	BindIntegratedLightScattering();
}
#undef I18N_KEY_PREFIX
