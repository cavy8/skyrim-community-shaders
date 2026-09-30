#include "HairBacklighting.h"

#include <algorithm>

#include "I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.hair_backlighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HairBacklighting::Settings,
	Enable,
	Strength,
	ScatterWidth,
	EdgeFalloff,
	InteriorGlow,
	Absorption,
	DarkBoost,
	DarkThreshold,
	HeadOcclusion);

namespace
{
	constexpr float kMaxStrength = 10.0f;
	constexpr float kMinScatterWidth = 0.1f;
	constexpr float kMaxScatterWidth = 1.0f;
	constexpr float kMinEdgeFalloff = 0.5f;
	constexpr float kMaxEdgeFalloff = 8.0f;
	constexpr float kMinAbsorption = 0.25f;
	constexpr float kMaxAbsorption = 3.0f;
	constexpr float kMinDarkBoost = 1.0f;
	constexpr float kMaxDarkBoost = 16.0f;
}

void HairBacklighting::DrawSettings()
{
	bool enable = settings.Enable != 0;
	if (ImGui::Checkbox(T(TKEY("enable"), "Enable"), &enable)) {
		settings.Enable = enable;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tooltip"), "Allows bright light to shine through strands of hair."));
	}

	ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &settings.Strength, 0.0f, kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("strength_tooltip"), "Sets the glow brightness when the light is directly behind the hair."));
	}

	ImGui::SliderFloat(T(TKEY("scatter_width"), "Scatter Width"), &settings.ScatterWidth, kMinScatterWidth, kMaxScatterWidth, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("scatter_width_tooltip"), "Controls how far the view can move from the light before the glow fades.\nLower values limit the glow to views almost directly toward the light."));
	}

	ImGui::SliderFloat(T(TKEY("edge_falloff"), "Edge Falloff"), &settings.EdgeFalloff, kMinEdgeFalloff, kMaxEdgeFalloff, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("edge_falloff_tooltip"), "How tightly the glow hugs the outline of the hair.\nHigher values keep it to the thin edges."));
	}

	ImGui::SliderFloat(T(TKEY("interior_glow"), "Interior Glow"), &settings.InteriorGlow, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("interior_glow_tooltip"), "Controls how much glow appears through the body of the hair compared with its edges."));
	}

	ImGui::SliderFloat(T(TKEY("absorption"), "Color Depth"), &settings.Absorption, kMinAbsorption, kMaxAbsorption, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("absorption_tooltip"), "How strongly the hair color tints the glow. Higher values create a deeper,\nmore saturated glow."));
	}

	ImGui::SliderFloat(T(TKEY("head_occlusion"), "Head Occlusion"), &settings.HeadOcclusion, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("head_occlusion_tooltip"), "Reduces glow where the head blocks the light, such as on the scalp when a light\nis in front of and below the character. Set to 0 to turn this off."));
	}

	ImGui::SeparatorText(T(TKEY("dark_surroundings"), "Dark Surroundings"));

	ImGui::SliderFloat(T(TKEY("dark_boost"), "Dark Boost"), &settings.DarkBoost, kMinDarkBoost, kMaxDarkBoost, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dark_boost_tooltip"), "Sets how much brighter the glow can become in dark areas. Set to 1 to turn off\nthe boost."));
	}

	ImGui::SliderFloat(T(TKEY("dark_threshold"), "Dark Threshold"), &settings.DarkThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dark_threshold_tooltip"), "Sets the ambient brightness below which the glow boost begins. Keep it below\ndaylight levels so sunlight is unaffected."));
	}
}

void HairBacklighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void HairBacklighting::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.Strength = std::clamp(settings.Strength, 0.0f, kMaxStrength);
	settings.ScatterWidth = std::clamp(settings.ScatterWidth, kMinScatterWidth, kMaxScatterWidth);
	settings.EdgeFalloff = std::clamp(settings.EdgeFalloff, kMinEdgeFalloff, kMaxEdgeFalloff);
	settings.InteriorGlow = std::clamp(settings.InteriorGlow, 0.0f, 1.0f);
	settings.Absorption = std::clamp(settings.Absorption, kMinAbsorption, kMaxAbsorption);
	settings.DarkBoost = std::clamp(settings.DarkBoost, kMinDarkBoost, kMaxDarkBoost);
	settings.DarkThreshold = std::clamp(settings.DarkThreshold, 0.0f, 1.0f);
	settings.HeadOcclusion = std::clamp(settings.HeadOcclusion, 0.0f, 1.0f);
}

void HairBacklighting::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
