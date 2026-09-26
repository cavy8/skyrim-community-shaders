#include "ProceduralSun.h"

#include <algorithm>
#include <cmath>

#include "../I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.procedural_sun."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ProceduralSun::Settings,
	enabled,
	sunDiskAngularRadius,
	diskIntensity,
	edgeSoftness,
	haloEnabled,
	haloAngularWidth,
	haloIntensity,
	haloFalloff,
	cloudExtinction,
	excludeFromAdaptation)

namespace
{
	constexpr float kMinimumAngularRadius = DirectX::XMConvertToRadians(0.05f);
	constexpr float kMaximumAngularRadius = DirectX::XMConvertToRadians(5.0f);
	constexpr float kMaximumDiskIntensity = 20.0f;
	constexpr float kMinimumEdgeSoftness = 0.01f;
	constexpr float kMaximumEdgeSoftness = 1.0f;
	constexpr float kMinimumHaloAngularWidth = DirectX::XMConvertToRadians(0.05f);
	constexpr float kMaximumHaloAngularWidth = DirectX::XMConvertToRadians(5.0f);
	constexpr float kMaximumHaloIntensity = 20.0f;
	constexpr float kMinimumHaloFalloff = 1.0f;
	constexpr float kMaximumHaloFalloff = 100.0f;
	constexpr float kMaximumCloudExtinction = 16.0f;

	void ClampSettings(ProceduralSun::Settings& settings)
	{
		const ProceduralSun::Settings defaults;
		settings.enabled = settings.enabled != 0;
		settings.haloEnabled = settings.haloEnabled != 0;
		settings.excludeFromAdaptation = settings.excludeFromAdaptation != 0;
		if (!std::isfinite(settings.sunDiskAngularRadius))
			settings.sunDiskAngularRadius = defaults.sunDiskAngularRadius;
		if (!std::isfinite(settings.diskIntensity))
			settings.diskIntensity = defaults.diskIntensity;
		if (!std::isfinite(settings.edgeSoftness))
			settings.edgeSoftness = defaults.edgeSoftness;
		if (!std::isfinite(settings.haloAngularWidth))
			settings.haloAngularWidth = defaults.haloAngularWidth;
		if (!std::isfinite(settings.haloIntensity))
			settings.haloIntensity = defaults.haloIntensity;
		if (!std::isfinite(settings.haloFalloff))
			settings.haloFalloff = defaults.haloFalloff;
		if (!std::isfinite(settings.cloudExtinction))
			settings.cloudExtinction = defaults.cloudExtinction;

		settings.sunDiskAngularRadius = std::clamp(settings.sunDiskAngularRadius, kMinimumAngularRadius, kMaximumAngularRadius);
		settings.diskIntensity = std::clamp(settings.diskIntensity, 0.0f, kMaximumDiskIntensity);
		settings.edgeSoftness = std::clamp(settings.edgeSoftness, kMinimumEdgeSoftness, kMaximumEdgeSoftness);
		settings.haloAngularWidth = std::clamp(settings.haloAngularWidth, kMinimumHaloAngularWidth, kMaximumHaloAngularWidth);
		settings.haloIntensity = std::clamp(settings.haloIntensity, 0.0f, kMaximumHaloIntensity);
		settings.haloFalloff = std::clamp(settings.haloFalloff, kMinimumHaloFalloff, kMaximumHaloFalloff);
		settings.cloudExtinction = std::clamp(settings.cloudExtinction, 0.0f, kMaximumCloudExtinction);
	}
}

void ProceduralSun::DrawSettings()
{
	bool enabled = settings.enabled != 0;
	if (ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &enabled))
		settings.enabled = enabled;

	ImGui::SliderAngle(T(TKEY("angular_radius"), "Angular Radius"), &settings.sunDiskAngularRadius, 0.05f, 5.0f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("angular_radius_tooltip"), "Angular size of the sun disc. The real sun's angular radius is about 0.27 degrees; the 0.53 degree default reads closer to the vanilla sun."));

	ImGui::SliderFloat(T(TKEY("disk_intensity"), "Disk Intensity"), &settings.diskIntensity, 0.0f, kMaximumDiskIntensity, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("disk_intensity_tooltip"), "Brightness of the procedural disc before the existing sky brightness and HDR adjustments."));

	ImGui::SliderFloat(T(TKEY("edge_softness"), "Edge Softness"), &settings.edgeSoftness, kMinimumEdgeSoftness, kMaximumEdgeSoftness, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("edge_softness_tooltip"), "Width of the anti-aliased transition at the edge of the sun disc."));

	ImGui::SliderFloat(T(TKEY("cloud_extinction"), "Cloud Extinction"), &settings.cloudExtinction, 0.0f, kMaximumCloudExtinction, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("cloud_extinction_tooltip"), "How strongly clouds block the sun disc, inner halo, and glare. 0 is plain cloud blending; higher values let less sunlight through thin clouds. The clouds' own colour is unchanged."));

	bool excludeFromAdaptation = settings.excludeFromAdaptation != 0;
	if (ImGui::Checkbox(T(TKEY("exclude_from_adaptation"), "Hide From Effects11 Adaptation"), &excludeFromAdaptation))
		settings.excludeFromAdaptation = excludeFromAdaptation;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("exclude_from_adaptation_tooltip"), "Replaces the sun disc with the surrounding sky in the image Effects11 eye adaptation measures, so the bright disc does not darken the whole frame. Bloom and lens effects still see the full disc. No effect without Effects11."));

	bool haloEnabled = settings.haloEnabled != 0;
	if (ImGui::Checkbox(T(TKEY("halo_enabled"), "Enable Halo"), &haloEnabled))
		settings.haloEnabled = haloEnabled;

	ImGui::BeginDisabled(!haloEnabled);
	ImGui::SliderAngle(T(TKEY("halo_width"), "Halo Width"), &settings.haloAngularWidth, 0.05f, 5.0f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("halo_width_tooltip"), "Angular distance the halo extends beyond the edge of the sun disc."));

	ImGui::SliderFloat(T(TKEY("halo_intensity"), "Halo Intensity"), &settings.haloIntensity, 0.0f, kMaximumHaloIntensity, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("halo_intensity_tooltip"), "Brightness of the halo before the normal sun tint, HDR, and bloom adjustments."));

	ImGui::SliderFloat(T(TKEY("halo_falloff"), "Halo Falloff"), &settings.haloFalloff, kMinimumHaloFalloff, kMaximumHaloFalloff, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("halo_falloff_tooltip"), "How quickly the halo fades with angle. Higher values concentrate it near the sun disc."));
	ImGui::EndDisabled();
}

#undef I18N_KEY_PREFIX

void ProceduralSun::LoadSettings(json& o_json)
{
	settings = o_json;
	ClampSettings(settings);
}

void ProceduralSun::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ProceduralSun::RestoreDefaultSettings()
{
	settings = {};
}

ProceduralSun::PerFrameData ProceduralSun::GetCommonBufferData() const
{
	return {
		.enabled = settings.enabled,
		.sunDiskCos = std::cos(settings.sunDiskAngularRadius),
		.diskIntensity = settings.diskIntensity,
		.edgeSoftness = settings.edgeSoftness,
		.haloEnabled = settings.haloEnabled,
		.sunHaloCos = std::cos(settings.sunDiskAngularRadius + settings.haloAngularWidth),
		.haloIntensity = settings.haloIntensity,
		.haloFalloff = settings.haloFalloff,
		.cloudExtinction = settings.cloudExtinction,
		.sunVisibility = GetSunVisibility(),
		.radianceLimit = GetMainTargetRadianceLimit()
	};
}

float ProceduralSun::GetMainTargetRadianceLimit()
{
	const auto renderer = globals::game::renderer;
	const auto rtv = renderer ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].RTV : nullptr;
	if (!rtv)
		return 1.0f;

	D3D11_RENDER_TARGET_VIEW_DESC desc{};
	rtv->GetDesc(&desc);
	switch (desc.Format) {
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
		return 4096.0f;
	default:
		return 1.0f;
	}
}

float ProceduralSun::GetSunVisibility()
{
	const auto sky = globals::game::sky;
	const auto sun = sky ? sky->sun : nullptr;
	if (!sun || !sun->root || !sun->sunBaseNode || !sun->sunBase)
		return 0.0f;
	if (sun->root->GetFlags().any(RE::NiAVObject::Flag::kHidden) || sun->sunBaseNode->GetFlags().any(RE::NiAVObject::Flag::kHidden))
		return 0.0f;

	const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(sun->sunBase->GetGeometryRuntimeData().shaderProperty.get());
	if (!prop)
		return 0.0f;

	const float alpha = prop->kBlendColor.alpha;
	return alpha > 0.0f ? std::min(alpha, 1.0f) : 0.0f;
}
