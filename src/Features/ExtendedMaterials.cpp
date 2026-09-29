#include "ExtendedMaterials.h"
#include "../I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.extended_materials."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ExtendedMaterials::Settings,
	EnableComplexMaterial,
	EnableParallax,
	EnableTerrain,
	EnableHeightBlending,
	EnableShadows,
	EnableParallaxWarpingFix,
	ParallaxQuality,
	EnableNormalMapShadows,
	NormalMapShadowHeightScale,
	NormalMapShadowLength,
	NormalMapShadowHardness,
	HeightMapShadowMode)

void ExtendedMaterials::DataLoaded()
{
	if (&settings.EnableTerrain) {
		if (auto bLandSpecular = globals::game::iniSettingCollection->GetSetting("bLandSpecular:Landscape"); bLandSpecular) {
			if (!bLandSpecular->data.b) {
				logger::info("[CPM] Changing bLandSpecular from {} to {} to support Terrain Parallax", bLandSpecular->data.b, true);
				bLandSpecular->data.b = true;
			}
		}
	}
}

void ExtendedMaterials::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("complex_material"), "Complex Material"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable_complex_material"), "Enable Complex Material"), (bool*)&settings.EnableComplexMaterial);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_complex_material_tooltip"),
								  "Enables support for the Complex Material specification which makes use of the environment mask. "
								  "This includes parallax, as well as more realistic metals and specular reflections. "
								  "May lead to some warped textures on modded content which have an invalid alpha channel in their environment mask. "));
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("parallax"), "Parallax"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable_parallax"), "Enable Parallax"), (bool*)&settings.EnableParallax);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_parallax_tooltip"), "Enables parallax on standard meshes made for parallax."));
		}

		if (ImGui::Checkbox(T(TKEY("enable_legacy_terrain"), "Enable Legacy Terrain"), (bool*)&settings.EnableTerrain)) {
			if (settings.EnableTerrain) {
				DataLoaded();
			}
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_legacy_terrain_tooltip"),
								  "Enables terrain parallax using the alpha channel of each landscape texture. "
								  "Therefore, all landscape textures must support parallax for this effect to work properly. "));
		}
		ImGui::Checkbox(T(TKEY("enable_height_blending"), "Enable Terrain Height Blending"), (bool*)&settings.EnableHeightBlending);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_height_blending_tooltip"), "Enables landscape texture blending based on parallax. "));
		}
		ImGui::Checkbox(T(TKEY("enable_parallax_warping_fix"), "Enable Parallax Warping Fix"), (bool*)&settings.EnableParallaxWarpingFix);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_parallax_warping_fix_tooltip"), "Enables a fix reducing parallax scale on curved and smooth normal triangles."));
		}
		ImGui::SliderFloat(T(TKEY("parallax_quality"), "Parallax Quality"), &settings.ParallaxQuality, MinParallaxQuality, MaxParallaxQuality, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("parallax_quality_tooltip"),
								  "Scales how many height samples parallax takes per pixel, on meshes and terrain. "
								  "1.00x is the default. Higher values reduce stepping and slicing at grazing angles at extra GPU cost; "
								  "lower values are faster but show more stepping."));
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("soft_shadows"), "Approximate Soft Shadows"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable_shadows"), "Enable Shadows"), (bool*)&settings.EnableShadows);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_shadows_tooltip"),
								  "Enables cheap soft shadows when using parallax. "
								  "This applies to all directional and point lights. "));
		}

		ImGui::Checkbox(T(TKEY("enable_normal_map_shadows"), "Enable Normal Mapping Shadows"), (bool*)&settings.EnableNormalMapShadows);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_normal_map_shadows_tooltip"),
								  "Self-shadowing traced from the normal map. "
								  "Applies to the sun and point lights."));
		}

		if (settings.EnableNormalMapShadows) {
			{
				auto _disabled = Util::DisableGuard(!settings.EnableShadows);
				const char* heightMapShadowModeNames[] = {
					T(TKEY("height_map_shadow_mode_parallax"), "Parallax Soft Shadows"),
					T(TKEY("height_map_shadow_mode_normal_map"), "Normal Mapping Shadows"),
					T(TKEY("height_map_shadow_mode_both"), "Both")
				};
				int heightMapShadowMode = static_cast<int>(settings.HeightMapShadowMode);
				if (ImGui::Combo(T(TKEY("height_map_shadow_mode"), "Height-Mapped Surfaces"), &heightMapShadowMode, heightMapShadowModeNames, IM_ARRAYSIZE(heightMapShadowModeNames))) {
					settings.HeightMapShadowMode = static_cast<uint>(heightMapShadowMode);
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("height_map_shadow_mode_tooltip"),
									  "Shadow technique for surfaces that have a height map (parallax, complex material, PBR displacement, terrain parallax).\n"
									  "Parallax Soft Shadows: cheapest. Darkens crevices from the height map at any light angle.\n"
									  "Normal Mapping Shadows: sharp shadows cast by normal map detail. Strongest when the light is low, subtle when it is high.\n"
									  "Both: parallax soft shadows combined with normal mapping shadows. Highest GPU cost.\n"
									  "When Enable Shadows is off, height-mapped surfaces always use Normal Mapping Shadows."));
			}

			ImGui::SliderFloat(T(TKEY("normal_map_shadow_height_scale"), "Normal Shadow Height Scale"), &settings.NormalMapShadowHeightScale, MinNormalMapShadowHeightScale, MaxNormalMapShadowHeightScale, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("normal_map_shadow_height_scale_tooltip"),
									  "Scales the relief reconstructed from normal map slopes. 1.00x matches the shading normals."));
			}
			ImGui::SliderFloat(T(TKEY("normal_map_shadow_length"), "Normal Shadow Length"), &settings.NormalMapShadowLength, MinNormalMapShadowLength, MaxNormalMapShadowLength, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("normal_map_shadow_length_tooltip"),
									  "Maximum trace distance toward the light, in texture UV units."));
			}
			ImGui::SliderFloat(T(TKEY("normal_map_shadow_hardness"), "Normal Shadow Hardness"), &settings.NormalMapShadowHardness, MinNormalMapShadowHardness, MaxNormalMapShadowHardness, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("normal_map_shadow_hardness_tooltip"),
									  "Higher values give sharper, darker shadow edges."));
			}
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

#undef I18N_KEY_PREFIX

void ExtendedMaterials::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float value, float minValue, float maxValue, float fallback) {
		return std::isfinite(value) ? std::clamp(value, minValue, maxValue) : fallback;
	};
	settings.ParallaxQuality = sanitize(settings.ParallaxQuality, MinParallaxQuality, MaxParallaxQuality, defaults.ParallaxQuality);
	settings.NormalMapShadowHeightScale = sanitize(settings.NormalMapShadowHeightScale, MinNormalMapShadowHeightScale, MaxNormalMapShadowHeightScale, defaults.NormalMapShadowHeightScale);
	settings.NormalMapShadowLength = sanitize(settings.NormalMapShadowLength, MinNormalMapShadowLength, MaxNormalMapShadowLength, defaults.NormalMapShadowLength);
	settings.NormalMapShadowHardness = sanitize(settings.NormalMapShadowHardness, MinNormalMapShadowHardness, MaxNormalMapShadowHardness, defaults.NormalMapShadowHardness);
	settings.HeightMapShadowMode = std::min(settings.HeightMapShadowMode, 2u);
}

void ExtendedMaterials::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ExtendedMaterials::RestoreDefaultSettings()
{
	settings = {};
}

bool ExtendedMaterials::HasShaderDefine(RE::BSShader::Type shaderType)
{
	switch (shaderType) {
	case RE::BSShader::Type::Lighting:
		return true;
	default:
		return false;
	}
}
