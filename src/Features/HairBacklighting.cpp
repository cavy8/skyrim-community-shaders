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
	DarkThreshold);

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
		ImGui::Text("%s", T(TKEY("enable_tooltip"), "Lets light shine through hair that is between the camera and a light."));
	}

	ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &settings.Strength, 0.0f, kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("strength_tooltip"), "Brightness of the glow when the light is directly behind the hair,\nas a multiple of the light's color."));
	}

	ImGui::SliderFloat(T(TKEY("scatter_width"), "Scatter Width"), &settings.ScatterWidth, kMinScatterWidth, kMaxScatterWidth, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("scatter_width_tooltip"), "How far the camera can move off the line to the light before the glow fades.\nLow values glow only when looking almost straight into the light."));
	}

	ImGui::SliderFloat(T(TKEY("edge_falloff"), "Edge Falloff"), &settings.EdgeFalloff, kMinEdgeFalloff, kMaxEdgeFalloff, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("edge_falloff_tooltip"), "How tightly the glow hugs the outline of the hair.\nHigher values keep it to the thin edges."));
	}

	ImGui::SliderFloat(T(TKEY("interior_glow"), "Interior Glow"), &settings.InteriorGlow, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("interior_glow_tooltip"), "Glow through the body of the hair, away from its edges, relative to the edges."));
	}

	ImGui::SliderFloat(T(TKEY("absorption"), "Color Depth"), &settings.Absorption, kMinAbsorption, kMaxAbsorption, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("absorption_tooltip"), "How strongly the hair color tints the glow. Higher values give a deeper,\nmore saturated glow (blonde turns golden) that is also dimmer on dark hair."));
	}

	ImGui::SeparatorText(T(TKEY("dark_surroundings"), "Dark Surroundings"));

	ImGui::SliderFloat(T(TKEY("dark_boost"), "Dark Boost"), &settings.DarkBoost, kMinDarkBoost, kMaxDarkBoost, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dark_boost_tooltip"), "The most the glow is amplified when the surroundings are dark, so a fire or\ntorch at night stands out like the sun does by day. 1 turns the boost off."));
	}

	ImGui::SliderFloat(T(TKEY("dark_threshold"), "Dark Threshold"), &settings.DarkThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("dark_threshold_tooltip"), "Ambient brightness (as in the weather's directional ambient colors) below which\nthe boost starts. The glow is amplified by how much darker the hair's ambient light\nis than this, up to Dark Boost. Keep it below daylight ambient so the sun is unchanged."));
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
}

void HairBacklighting::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
