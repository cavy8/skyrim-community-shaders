#include "FireEffects.h"

#include "I18n/I18n.h"
#include "Util.h"
#include "Utils/MathUtils.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FireEffects::Settings,
	FireIntensity,
	FireCurve,
	LightSpriteIntensity,
	LightSpriteCurve,
	FireColorPreservation)

namespace
{
	constexpr float kMaxIntensity = 10.0f;
	constexpr float kMinCurve = 0.1f;
	constexpr float kMaxCurve = 8.0f;
}

void FireEffects::DrawSettings()
{
	ImGui::SliderFloat(T("feature.post_processing.fire.fire_intensity", "Fire Intensity"), &settings.FireIntensity, 0.0f, kMaxIntensity, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.post_processing.fire.fire_intensity_tooltip", "Brightness of flames, embers and spell bolts drawn as additive fire."));
	ImGui::SliderFloat(T("feature.post_processing.fire.fire_curve", "Fire Curve"), &settings.FireCurve, kMinCurve, kMaxCurve, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.post_processing.fire.fire_curve_tooltip", "Contrast of fire: above 1 darkens the dim parts of a flame and keeps its core, below 1 fills it out."));
	ImGui::SliderFloat(T("feature.post_processing.fire.fire_color_preservation", "Fire Color Preservation"), &settings.FireColorPreservation, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.post_processing.fire.fire_color_preservation_tooltip", "Keeps the hue and saturation a flame's texture shows in vanilla, where bright flames turn orange and yellow, at the same brightness. 0 lets bright fire drift toward a pale red."));
	ImGui::SliderFloat(T("feature.post_processing.fire.light_sprite_intensity", "Light Sprite Intensity"), &settings.LightSpriteIntensity, 0.0f, kMaxIntensity, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T("feature.post_processing.fire.light_sprite_intensity_tooltip", "Brightness of soft glows such as candle and torch halos, sky glows and magic light sprites."));
	ImGui::SliderFloat(T("feature.post_processing.fire.light_sprite_curve", "Light Sprite Curve"), &settings.LightSpriteCurve, kMinCurve, kMaxCurve, "%.2f");
}

void FireEffects::RestoreDefaultSettings()
{
	settings = {};
}

void FireEffects::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	settings.FireIntensity = Util::ClampFinite(settings.FireIntensity, defaults.FireIntensity, 0.0f, kMaxIntensity);
	settings.FireCurve = Util::ClampFinite(settings.FireCurve, defaults.FireCurve, kMinCurve, kMaxCurve);
	settings.LightSpriteIntensity = Util::ClampFinite(settings.LightSpriteIntensity, defaults.LightSpriteIntensity, 0.0f, kMaxIntensity);
	settings.LightSpriteCurve = Util::ClampFinite(settings.LightSpriteCurve, defaults.LightSpriteCurve, kMinCurve, kMaxCurve);
	settings.FireColorPreservation = Util::ClampFinite(settings.FireColorPreservation, defaults.FireColorPreservation, 0.0f, 1.0f);
}

void FireEffects::SaveSettings(json& o_json)
{
	o_json = settings;
}
