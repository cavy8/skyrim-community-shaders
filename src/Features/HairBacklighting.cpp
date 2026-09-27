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
	Absorption);

namespace
{
	constexpr float kMaxStrength = 10.0f;
	constexpr float kMinScatterWidth = 0.1f;
	constexpr float kMaxScatterWidth = 1.0f;
	constexpr float kMinEdgeFalloff = 0.5f;
	constexpr float kMaxEdgeFalloff = 8.0f;
	constexpr float kMinAbsorption = 0.25f;
	constexpr float kMaxAbsorption = 3.0f;
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
}

void HairBacklighting::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
