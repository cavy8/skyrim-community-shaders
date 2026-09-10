#include "SnowCover.h"

#include "../I18n/I18n.h"
#include "ShaderCache.h"
#include "Util.h"
#include "Utils/FileSystem.h"
#include <DDSTextureLoader.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ranges>
#include <string.h>
#include <string_view>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SnowCover::UserSettings,
	EnableExpensiveFoliage,
	AffectHavok,
	AffectFloraTint,
	SnowHeightOffset,
	EnableFireMelt,
	FireRadiusScale,
	FireInnerScale,
	FireMaxDistance)

namespace
{
	void copyString(const std::string& input, char* dst, size_t dst_size)
	{
		strncpy(dst, input.c_str(), dst_size - 1);
		dst[dst_size - 1] = '\0';
	}
}

#define I18N_KEY_PREFIX "feature.snow_cover."

void SnowCover::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable_nicer_foliage"), "Enable Nicer Foliage"), (bool*)&settings.EnableExpensiveFoliage);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_nicer_foliage_tooltip"), "Uses one more texture sample to put snow on edges of tree lods and grass."));
	}
	ImGui::Checkbox(T(TKEY("melt_snow_near_fire"), "Melt Snow Near Fire"), (bool*)&settings.EnableFireMelt);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("melt_snow_near_fire_tooltip"),
							  "Clears snow around campfires, torches and braziers.\n"
							  "Fires are recognised as additive flame draws using fire-themed textures."));
	}
	if (settings.EnableFireMelt) {
		ImGui::SliderFloat(T(TKEY("fire_melt_radius"), "Fire Melt Radius"), &settings.FireRadiusScale, 0.5f, 8.0f, "%.1fx");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("fire_melt_radius_tooltip"), "Melt radius as a multiple of the flame's bounding radius."));
		}
		ImGui::SliderFloat(T(TKEY("fire_melt_softness"), "Fire Melt Softness"), &settings.FireInnerScale, 0.0f, 0.9f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("fire_melt_softness_tooltip"), "Fraction of the radius that is fully clear. Lower values give a softer edge."));
		}
		ImGui::SliderFloat(T(TKEY("fire_melt_max_distance"), "Fire Melt Max Distance"), &settings.FireMaxDistance, 1024.0f, 40000.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("fire_melt_max_distance_tooltip"), "Fires beyond this camera distance are ignored. At most 16 fires are tracked."));
		}
		ImGui::Text(T(TKEY("tracked_fires"), "Tracked fires: %u"), perFrame.FireCount);
	}
	ImGui::Checkbox(T(TKEY("snow_on_mobile_objects"), "Snow on Mobile Objects"), (bool*)&settings.AffectHavok);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("snow_on_mobile_objects_tooltip"),
							  "Movable clutter (Havok rigidbodies) such as barrels, crates and dropped items will receive snow.\n"
							  "Snow stays on them while they move instead of vanishing. It is projected from world space,\n"
							  "so it drifts across the surface while an object is tumbling. Actors are never affected."));
	}
	ImGui::Checkbox(T(TKEY("tint_harvestable_plants"), "Tint Harvestable Plants"), (bool*)&settings.AffectFloraTint);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("tint_harvestable_plants_tooltip"),
							  "Lets the seasonal tree tint recolour harvestable flora such as mountain flowers,\n"
							  "lavender and deathbell. Their petals are not green, so the shift towards autumn\n"
							  "hues sends them to the wrong colour. Off by default; snow still settles on them."));
	}
	ImGui::SliderFloat(T(TKEY("snow_offset"), "Snow Offset"), &settings.SnowHeightOffset, -20000.0f, 20000.0f);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("snow_offset_tooltip"), "Moves the altitude that snow appears at. For testing purposes."));
	}
	ImGui::Separator();
	ImGui::Text("%s", T(TKEY("worldspace_config_hint1"), "Each config applies to one worldspace or interior cell."));
	ImGui::Text("%s", T(TKEY("worldspace_config_hint2"), "Saved config will be applied when you enter the worldspace."));

	if (ImGui::TreeNodeEx(T(TKEY("worldspace_config"), "Worldspace Config"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text(T(TKEY("current_worldspace"), "Current worldspace/cell: %s"), last_worldspace.c_str());
		ImGui::Text(T(TKEY("config_status"), "Config status: %s"), status.c_str());

		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("reload"), "Reload"))) {
			last_worldspace = "";
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("reload_tooltip"),
								  "Reloads the config for current worldspace from file."
								  "Reload is required to load new texture paths."));
		}
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("save"), "Save"))) {
			SaveConfig();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("save_tooltip"), "Saves the current config to a file."));
		}
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("defaults"), "Defaults"))) {
			wsettings = WorldSettings();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("defaults_tooltip"), "Resets the current config to default values."));
		}
		ImGui::Separator();
		ImGui::Checkbox(T(TKEY("enable_snow_cover"), "Enable Snow Cover"), (bool*)&wsettings.EnableSnowCover);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_snow_cover_tooltip"), "Enables the feature. This is set automatically when a config is found."));
		}
		if (wsettings.EnableSnowCover) {
			ImGui::Checkbox(T(TKEY("affect_grass_tint"), "Affect Grass Tint"), (bool*)&wsettings.AffectGrassTint);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("affect_grass_tint_tooltip"), "Should grass turn yellow?"));
			}
			ImGui::Checkbox(T(TKEY("affect_tree_tint"), "Affect Tree Tint"), (bool*)&wsettings.AffectTreeTint);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("affect_tree_tint_tooltip"), "Should trees turn yellow?"));
			}
			ImGui::SliderFloat(T(TKEY("foliage_color_height_offset"), "Foliage Color Height Offset"), &wsettings.FoliageHeightOffset, -2000.0f, 2000.0f);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("foliage_color_height_offset_tooltip"), "How far below/above the snow line should the foliage color start changing?"));
			}
			ImGui::SliderFloat(T(TKEY("blend_smoothness"), "Blend smoothness"), &wsettings.BlendSmoothness, 1.f, 10000.0f);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("blend_smoothness_tooltip"), "How gradual the snow transition is."));
			}
			if (ImGui::TreeNodeEx(T(TKEY("distance"), "Distance"))) {
				ImGui::SliderFloat(T(TKEY("weather_fade_start"), "Weather Fade Start"), &wsettings.WeatherFadeStart, 0.0f, 200000.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("weather_fade_start_tooltip"),
										  "Camera distance at which weather-driven snow starts fading out.\n"
										  "Keep it above ~10000 or the fade lands on the terrain LOD and the snow front crawls with the camera.\n"
										  "Seasonal and altitude snow is never distance-faded."));
				}
				ImGui::SliderFloat(T(TKEY("weather_fade_end"), "Weather Fade End"), &wsettings.WeatherFadeEnd, 0.0f, 300000.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("weather_fade_end_tooltip"), "Camera distance at which weather-driven snow is gone entirely."));
				}
				ImGui::SliderFloat(T(TKEY("object_fade_start"), "Object Fade Start"), &wsettings.ObjectFadeStart, 0.0f, 40000.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("object_fade_start_tooltip"), "Camera distance at which snow on statics and trees starts weakening."));
				}
				ImGui::SliderFloat(T(TKEY("object_fade_end"), "Object Fade End"), &wsettings.ObjectFadeEnd, 0.0f, 40000.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("object_fade_end_tooltip"), "Camera distance at which that falloff is complete."));
				}
				ImGui::SliderFloat(T(TKEY("object_fade_amount"), "Object Fade Amount"), &wsettings.ObjectFadeAmount, 0.0f, 1.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("object_fade_amount_tooltip"),
										  "Fraction of snow removed from distant statics and trees.\n"
										  "Set to 0 to disable the falloff entirely. Keeps DynDOLOD ultra-tree billboards from reading as white slabs."));
				}
				ImGui::TreePop();
			}
			if (ImGui::TreeNodeEx(T(TKEY("weather_and_seasons"), "Weather and Seasons"))) {
				ImGui::SliderInt(T(TKEY("maximum_summer_month"), "Maximum Summer Month"), (int*)&MaxSummerMonth, 0, 11);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("maximum_summer_month_tooltip"), "In which month is the snow line highest?"));
				}
				ImGui::SliderInt(T(TKEY("maximum_winter_month"), "Maximum Winter Month"), (int*)&MaxWinterMonth, 0, 11);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("maximum_winter_month_tooltip"), "In which month is the snow line lowest?"));
				}
				ImGui::SliderFloat(T(TKEY("summer_height_offset"), "Summer Height Offset"), &SummerHeightOffset, -20000.0f, 20000.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("summer_height_offset_tooltip"), "What is the snow line altitude in summer?"));
				}
				ImGui::SliderFloat(T(TKEY("winter_height_offset"), "Winter Height Offset"), &WinterHeightOffset, -20000.0f, 20000.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("winter_height_offset_tooltip"), "What is the snow line altitude in winter?"));
				}
				ImGui::SliderFloat(T(TKEY("snowing_speed"), "Snowing speed"), &snowing_speed, 0.01f, 10.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("snowing_speed_tooltip"), "How fast snowy weather accumulates snow. Also depends on the weather settings."));
				}
				ImGui::SliderFloat(T(TKEY("melting_speed"), "Melting speed"), &melting_speed, 0.01f, 10.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("melting_speed_tooltip"), "How fast snow (under snow line) disappears when it is not snowing."));
				}
				ImGui::TreePop();
			}

			if (ImGui::TreeNodeEx(T(TKEY("snow_map"), "Snow Map"))) {
				ImGui::InputText(T(TKEY("map_texture"), "Map texture"), mapbuf, 128);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("map_texture_tooltip"), "Path to the map texture relative to Data folder. Interpreted as grayscale."));
				}
				ImGui::InputFloat(T(TKEY("map_min_x"), "Min X"), &mapMin.x, 0.0f, 10.0f);
				ImGui::InputFloat(T(TKEY("map_min_y"), "Min Y"), &mapMin.y, 0.0f, 10.0f);
				ImGui::InputFloat(T(TKEY("map_max_x"), "Max X"), &mapMax.x, 0.0f, 10.0f);
				ImGui::InputFloat(T(TKEY("map_max_y"), "Max Y"), &mapMax.y, 0.0f, 10.0f);

				map_tex = std::filesystem::path(mapbuf);
				ImGui::SliderFloat(T(TKEY("snow_map_zscale"), "Snow Map Z Scale"), &wsettings.mapZscale, 0.1f, 10000.0f);
				ImGui::TreePop();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("snow_map_tooltip"), "A grayscale map of the worldspace that offsets the altitude snow appears at. Relative to game Data folder, without '.dds' "));
			}

			if (ImGui::TreeNodeEx(T(TKEY("material"), "Material"))) {
				ImGui::SliderFloat(T(TKEY("min_angle"), "Min Angle"), &wsettings.MinAngle, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("min_angle_tooltip"), "The angle snow starts appearing at. 0 = horizontal, 1 = vertical."));
				}
				ImGui::SliderFloat(T(TKEY("max_angle"), "Max Angle"), &wsettings.MaxAngle, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("max_angle_tooltip"), "The angle snow is fully visible at. 0 = horizontal, 1 = vertical."));
				}
				ImGui::InputText(T(TKEY("main_texture"), "Main texture"), tbuf, 128);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("main_texture_tooltip"), "Path to the main texture relative to Data folder, without '.dds'. Needs to be a PBR texture with a diffuse, _n, and _rmaos."));
				}
				main_tex = std::string(tbuf);
				ImGui::InputText(T(TKEY("alt_texture"), "Alt texture"), altbuf, 128);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("alt_texture_tooltip"), "Optional path to the alternative texture relative to Data folder, without '.dds'. Needs to be a PBR texture with a diffuse, _n, and _rmaos."));
				}
				alt_tex = std::string(altbuf);
				ImGui::ColorEdit4(T(TKEY("main_tint"), "Main Tint"), &wsettings.MainTint.x);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("main_tint_tooltip"), "Tint for the main texture. Alpha only affects the color but not roughness etc."));
				}
				ImGui::InputFloat(T(TKEY("main_specular"), "Main Specular"), &wsettings.MainSpec, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("main_specular_tooltip"), "The main specular multiplier. Most materials = 0.04, snow = 0.02"));
				}
				ImGui::ColorEdit4(T(TKEY("alt_tint"), "Alt Tint"), &wsettings.AltTint.x);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("alt_tint_tooltip"), "Tint for the alternative texture. Alpha only affects the color but not roughness etc."));
				}
				ImGui::InputFloat(T(TKEY("alt_specular"), "Alt Specular"), &wsettings.AltSpec, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("alt_specular_tooltip"), "The alternative specular multiplier. Most materials = 0.04, snow = 0.02"));
				}
				ImGui::SliderFloat(T(TKEY("peak_main_angle"), "Peak Main Angle"), &wsettings.PeakMainAngle, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("peak_main_angle_tooltip"), "The angle at which the main texture is the strongest. The stronger wins. 0 = horizontal, 1 = vertical."));
				}
				ImGui::SliderFloat(T(TKEY("peak_alt_angle"), "Peak Alt Angle"), &wsettings.PeakAltAngle, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("peak_alt_angle_tooltip"), "The angle at which the alternative texture is the strongest. The stronger wins. 0 = horizontal, 1 = vertical."));
				}
				ImGui::SliderFloat(T(TKEY("uv_scale"), "UV Scale"), &wsettings.UVScale, 0.1f, 10.f, "%.1f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("uv_scale_tooltip"), "The UV scale of both textures."));
				}
				ImGui::Text("%s", T(TKEY("glint_header"), "Glint"));
				ImGui::SliderFloat(T(TKEY("glint_screenspace_scale"), "Screenspace Scale"), &wsettings.ScreenSpaceScale, 0.f, 3.f, "%.3f");
				ImGui::SliderFloat(T(TKEY("glint_log_microfacet_density"), "Log Microfacet Density"), &wsettings.LogMicrofacetDensity, 0.f, 40.f, "%.3f");
				ImGui::SliderFloat(T(TKEY("glint_microfacet_roughness"), "Microfacet Roughness"), &wsettings.MicrofacetRoughness, 0.f, 1.f, "%.3f");
				ImGui::SliderFloat(T(TKEY("glint_density_randomization"), "Density Randomization"), &wsettings.DensityRandomization, 0.f, 5.f, "%.3f");
				ImGui::TreePop();
			}
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("debug_info"), "Debug Info"))) {
		ImGui::Text(T(TKEY("debug_month"), "Month: %.3f"), perFrame.Month);
		ImGui::Text(T(TKEY("debug_time_snowing"), "TimeSnowing: %.3f"), perFrame.TimeSnowing);
		ImGui::Text(T(TKEY("debug_snowing_density"), "SnowingDensity: %.3f"), perFrame.SnowingDensity);
		if (debug_text != nullptr)
			ImGui::Text(T(TKEY("debug_text"), "Debug text: %s"), debug_text);

		ImGui::TreePop();
	}

	ImGui::Spacing();
	ImGui::Spacing();
}

#undef I18N_KEY_PREFIX

SnowCover::PerFrame SnowCover::GetCommonBufferData()
{
	Reload();

	bool snowing = false;
	bool raining = false;
	if (wsettings.EnableSnowCover) {
		snowingDensity = 0;
		if (auto sky = RE::Sky::GetSingleton()) {
			if (auto currentWeather = sky->currentWeather) {
				if (currentWeather->precipitationData) {
					float particleDensity = currentWeather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
					float particleGravity = currentWeather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kGravityVelocity).f;
					snowingDensity = particleDensity * particleGravity;
				}
				if (currentWeather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow)) {
					snowing = true;
				} else if (currentWeather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy)) {
					raining = true;
				}
			}
		}
	} else {
		snowingDensity = 0;
	}
	if (auto calendar = RE::Calendar::GetSingleton()) {
		auto h = calendar->GetHour();
		auto diff = h < lastHour ? h + 24 - lastHour : h - lastHour;
		if (snowing)
			timeSnowing += diff * snowing_speed;
		else if (raining) {
			timeSnowing -= 2 * diff * melting_speed;
		} else {
			if (timeSnowing > 0) {
				timeSnowing = std::max(timeSnowing - diff * melting_speed, 0.0f);
			} else if (timeSnowing < 0) {
				timeSnowing = std::min(timeSnowing + diff * melting_speed, 0.0f);
			}
		}

		timeSnowing = std::clamp(timeSnowing, -2.0f, 2.0f);
		lastHour = h;

		auto time = calendar->GetTime();
		perFrame.Month = static_cast<float>(time.tm_mon + (time.tm_mday + (time.tm_hour + (time.tm_min + time.tm_sec / 60.0) / 60.0) / 24.0) / 32.0);
	}
	perFrame.SnowingDensity = snowingDensity;
	perFrame.TimeSnowing = timeSnowing;
	perFrame.SeasonalAltitude = GetSeasonalAltitude();
	perFrame.settings = settings;
	perFrame.wsettings = wsettings;

	uint32_t count = 0;
	if (settings.EnableFireMelt) {
		std::lock_guard lock{ fireSourcesMutex };
		for (const auto& source : fireSources) {
			if (count >= MAX_FIRE_SOURCES)
				break;
			const float radius = source.radius * source.strength;
			if (radius < 1.0f)
				continue;
			perFrame.FireSources[count++] = float4(source.position.x, source.position.y, source.position.z, radius);
		}
	}
	for (uint32_t i = count; i < MAX_FIRE_SOURCES; ++i)
		perFrame.FireSources[i] = float4(0.0f, 0.0f, 0.0f, 0.0f);
	perFrame.FireCount = count;

	return perFrame;
}

void SnowCover::SetupResources()
{
	Reload();
	if (auto calendar = RE::Calendar::GetSingleton())
		lastHour = calendar->GetHour();
}

namespace
{
	const char* GetWorldspace()
	{
		auto curr_worldspace = "none";
		auto tes = RE::TES::GetSingleton();
		if (tes) {
			auto worldspace = tes->GetRuntimeData2().worldSpace;
			if (tes->interiorCell) {
				curr_worldspace = tes->interiorCell->GetFormEditorID();
			} else if (worldspace) {
				curr_worldspace = worldspace->GetFormEditorID();
			}
		}
		return curr_worldspace;
	}
}

void SnowCover::SaveConfig()
{
	if (last_worldspace.empty())
		return;
	json config = {
		{ "AffectGrassTint", wsettings.AffectGrassTint },
		{ "AffectTreeTint", wsettings.AffectTreeTint },
		{ "FoliageHeightOffset", wsettings.FoliageHeightOffset },
		{ "UVScale", wsettings.UVScale },
		{ "MaxSummerMonth", MaxSummerMonth },
		{ "MaxWinterMonth", MaxWinterMonth },
		{ "SummerHeightOffset", SummerHeightOffset },
		{ "WinterHeightOffset", WinterHeightOffset },
		{ "MapTexture", map_tex.generic_string() },
		{ "MapZscale", wsettings.mapZscale },
		{ "BlendSmoothness", wsettings.BlendSmoothness },
		{ "WeatherFadeStart", wsettings.WeatherFadeStart },
		{ "WeatherFadeEnd", wsettings.WeatherFadeEnd },
		{ "ObjectFadeStart", wsettings.ObjectFadeStart },
		{ "ObjectFadeEnd", wsettings.ObjectFadeEnd },
		{ "ObjectFadeAmount", wsettings.ObjectFadeAmount },
		{ "ScreenSpaceScale", wsettings.ScreenSpaceScale },
		{ "LogMicrofacetDensity", wsettings.LogMicrofacetDensity },
		{ "MicrofacetRoughness", wsettings.MicrofacetRoughness },
		{ "DensityRandomization", wsettings.DensityRandomization },
		{ "MapMin", json::array({ mapMin.x, mapMin.y }) },
		{ "MapMax", json::array({ mapMax.x, mapMax.y }) },
		{ "MainTexture", main_tex.generic_string() },
		{ "AltTexture", alt_tex.generic_string() },
		{ "MainTint", json::array({ wsettings.MainTint.x,
						  wsettings.MainTint.y,
						  wsettings.MainTint.z,
						  wsettings.MainTint.w }) },
		{ "AltTint", json::array({ wsettings.AltTint.x,
						 wsettings.AltTint.y,
						 wsettings.AltTint.z,
						 wsettings.AltTint.w }) },
		{ "SnowingSpeed", snowing_speed },
		{ "MeltingSpeed", melting_speed },
		{ "PeakMainAngle", wsettings.PeakMainAngle },
		{ "PeakAltAngle", wsettings.PeakAltAngle },
		{ "MinAngle", wsettings.MinAngle },
		{ "MaxAngle", wsettings.MaxAngle },
		{ "MainSpec", wsettings.MainSpec },
		{ "AltSpec", wsettings.AltSpec },
	};

	try {
		auto curr_worldspace = GetWorldspace();
		auto path = (Util::PathHelpers::GetShadersPath() / "SnowCover" / curr_worldspace).replace_extension(std::filesystem::path(".json"));
		std::ofstream file(path);
		file << config.dump(4);
	} catch (const std::system_error& e) {
		logger::error("[Snow Cover] Error saving file: {}", e.what());
	}
	Reload();
}

void SnowCover::Reload()
{
	std::ifstream fileStream;
	std::filesystem::path path;
	try {
		std::string curr_worldspace = GetWorldspace();
		if (curr_worldspace == last_worldspace)
			return;
		last_worldspace = curr_worldspace;
		path = (Util::PathHelpers::GetShadersPath() / "SnowCover" / curr_worldspace).replace_extension(std::filesystem::path(".json"));
		if (!std::filesystem::exists(path)) {
			status = std::format("Config doesn't exist {}", path.generic_string());
			wsettings.EnableSnowCover = false;
			return;
		}
		fileStream = std::ifstream(path);
		if (!fileStream.is_open()) {
			status = std::string("Cannot open config.");
			wsettings.EnableSnowCover = false;
			logger::error("[Snow Cover] Cannot open config at {}", path.generic_string());
			return;
		}
	} catch (const std::system_error& e) {
		logger::error("[Snow Cover] Error opening file: {}", e.what());
	}

	auto whitelist_path = Util::PathHelpers::GetShadersPath() / "SnowCover" / "whitelist.txt";
	auto blacklist_path = Util::PathHelpers::GetShadersPath() / "SnowCover" / "blacklist.txt";

	whitelist = FormIdParser::parseTriNameFile(whitelist_path);
	blacklist = FormIdParser::parseTriNameFile(blacklist_path);

	json config;
	try {
		fileStream >> config;
		wsettings.AffectGrassTint = config["AffectGrassTint"];
		wsettings.AffectTreeTint = config["AffectTreeTint"];
		wsettings.FoliageHeightOffset = config["FoliageHeightOffset"];
		wsettings.UVScale = config["UVScale"];
		MaxSummerMonth = config["MaxSummerMonth"];
		MaxWinterMonth = config["MaxWinterMonth"];
		SummerHeightOffset = config["SummerHeightOffset"];
		WinterHeightOffset = config["WinterHeightOffset"];
		mapMin = float2(config["MapMin"][0], config["MapMin"][1]);
		mapMax = float2(config["MapMax"][0], config["MapMax"][1]);
		wsettings.mapScale = float2(1.0) / (mapMax - mapMin);
		wsettings.mapOffset = -mapMin * wsettings.mapScale;
		wsettings.mapZscale = config["MapZscale"];
		wsettings.BlendSmoothness = config["BlendSmoothness"];
		// Shipped worldspace configs predate the distance knobs; operator[] would throw on them.
		wsettings.WeatherFadeStart = config.value("WeatherFadeStart", DEFAULT_WEATHER_FADE_START);
		wsettings.WeatherFadeEnd = config.value("WeatherFadeEnd", DEFAULT_WEATHER_FADE_END);
		wsettings.ObjectFadeStart = config.value("ObjectFadeStart", DEFAULT_OBJECT_FADE_START);
		wsettings.ObjectFadeEnd = config.value("ObjectFadeEnd", DEFAULT_OBJECT_FADE_END);
		wsettings.ObjectFadeAmount = config.value("ObjectFadeAmount", DEFAULT_OBJECT_FADE_AMOUNT);
		wsettings.ScreenSpaceScale = config["ScreenSpaceScale"];
		wsettings.LogMicrofacetDensity = config["LogMicrofacetDensity"];
		wsettings.MicrofacetRoughness = config["MicrofacetRoughness"];
		wsettings.DensityRandomization = config["DensityRandomization"];
		wsettings.MainTint = float4(config["MainTint"][0], config["MainTint"][1], config["MainTint"][2], config["MainTint"][3]);
		wsettings.AltTint = float4(config["AltTint"][0], config["AltTint"][1], config["AltTint"][2], config["AltTint"][3]);
		snowing_speed = config["SnowingSpeed"];
		melting_speed = config["MeltingSpeed"];
		wsettings.PeakMainAngle = config["PeakMainAngle"];
		wsettings.PeakAltAngle = config["PeakAltAngle"];
		wsettings.MinAngle = config["MinAngle"];
		wsettings.MaxAngle = config["MaxAngle"];
		wsettings.MainSpec = config["MainSpec"];
		wsettings.AltSpec = config["AltSpec"];
		main_tex = std::filesystem::path(config["MainTexture"].get<std::string>());
		copyString(main_tex.generic_string().c_str(), tbuf, 256);
		auto device = globals::d3d::device;
		auto context = globals::d3d::context;
		for (auto& view : views)
			view = nullptr;
		auto data_path = Util::PathHelpers::GetDataPath();
		auto loadDDS = [&](const std::filesystem::path& file) -> winrt::com_ptr<ID3D11ShaderResourceView> {
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			if (FAILED(DirectX::CreateDDSTextureFromFile(device, context, file.native().c_str(), nullptr, view.put())))
				return nullptr;
			return view;
		};

		views[0] = loadDDS(std::filesystem::path(data_path / main_tex).replace_extension(".dds"));
		if (!views[0]) {
			logger::warn("Snow Cover: Error loading {}.dds texture", main_tex.generic_string());
			views[0] = loadDDS(Util::PathHelpers::GetShadersPath() / "SnowCover" / "default" / "main.dds");
		}
		views[1] = loadDDS(std::filesystem::path(data_path / main_tex).concat("_n.dds"));
		if (!views[1]) {
			logger::warn("Snow Cover: Error loading {}_n.dds texture", main_tex.generic_string());
			views[1] = loadDDS(Util::PathHelpers::GetShadersPath() / "SnowCover" / "default" / "main_n.dds");
		}
		views[2] = loadDDS(std::filesystem::path(data_path / main_tex).concat("_rmaos.dds"));
		if (!views[2]) {
			logger::warn("Snow Cover: Error loading {}_rmaos.dds texture", main_tex.generic_string());
			views[2] = loadDDS(Util::PathHelpers::GetShadersPath() / "SnowCover" / "default" / "main_rmaos.dds");
		}

		if (config.contains("AltTexture") && config["AltTexture"] != "") {
			alt_tex = std::filesystem::path(config["AltTexture"].get<std::string>());
			copyString(alt_tex.generic_string().c_str(), altbuf, 256);
			views[3] = loadDDS(std::filesystem::path(data_path / alt_tex).replace_extension(".dds"));
			if (!views[3]) {
				logger::warn("Snow Cover: Error loading {}.dds texture", alt_tex.generic_string());
				views[3] = views[0];
			}
			views[4] = loadDDS(std::filesystem::path(data_path / alt_tex).concat("_n.dds"));
			if (!views[4]) {
				logger::warn("Snow Cover: Error loading {}_n.dds texture", alt_tex.generic_string());
				views[4] = views[1];
			}
			views[5] = loadDDS(std::filesystem::path(data_path / alt_tex).concat("_rmaos.dds"));
			if (!views[5]) {
				logger::warn("Snow Cover: Error loading {}_rmaos.dds texture", alt_tex.generic_string());
				views[5] = views[2];
			}
		} else {
			views[3] = views[0];
			views[4] = views[1];
			views[5] = views[2];
		}

		map_tex = std::filesystem::path(config["MapTexture"].get<std::string>());
		copyString(map_tex.generic_string().c_str(), mapbuf, 256);
		views[6] = loadDDS(std::filesystem::path(data_path / map_tex).concat(".dds"));
		if (!views[6]) {
			logger::warn("Snow Cover: Error loading {}.dds texture", map_tex.generic_string());
			views[6] = loadDDS(data_path / "textures" / "gray.dds");
		}

		static constexpr std::array<const char*, 7> kViewNames{
			"SnowCover::MainAlbedo SRV", "SnowCover::MainNormal SRV", "SnowCover::MainRMAOS SRV",
			"SnowCover::AltAlbedo SRV", "SnowCover::AltNormal SRV", "SnowCover::AltRMAOS SRV",
			"SnowCover::Map SRV"
		};
		for (size_t i = 0; i < views.size(); ++i) {
			if (views[i])
				Util::SetResourceName(views[i].get(), kViewNames[i]);
		}

		wsettings.EnableSnowCover = true;
	} catch (const nlohmann::json::parse_error& e) {
		logger::error("[Snow Cover] failed to parse {} : {}", path.generic_string(), e.what());
		status = e.what();
		wsettings = WorldSettings{};
		wsettings.EnableSnowCover = false;
		return;
	} catch (const nlohmann::json::exception& e) {
		logger::error("[Snow Cover] failed to parse {} : {}", path.generic_string(), e.what());
		status = e.what();
		wsettings = WorldSettings{};
		wsettings.EnableSnowCover = false;
		return;
	} catch (...) {
		logger::error("[Snow Cover] unknown error when loading a config: {}", path.generic_string());
		status = std::string("Unknown error.");
		return;
	}
	status = std::string("Loaded.");
}

void SnowCover::Prepass()
{
	if (wsettings.EnableSnowCover) {
		ID3D11ShaderResourceView* srvs[std::tuple_size_v<decltype(views)>]{};
		for (size_t i = 0; i < views.size(); ++i)
			srvs[i] = views[i].get();
		globals::d3d::context->PSSetShaderResources(FIRST_SRV_SLOT, (uint)views.size(), srvs);
	}
}

void SnowCover::Reset()
{
	// Frame-rate independent ease; clamp guards against loading-screen delta spikes snapping the melt.
	const float dt = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f);
	std::lock_guard lock{ fireSourcesMutex };
	++fireTick;
	for (auto& source : fireSources) {
		const bool seen = fireTick - source.lastSeenTick <= FIRE_SEEN_GRACE_TICKS;
		const float target = seen ? 1.0f : 0.0f;
		const float step = (seen ? FIRE_FADE_IN_RATE : FIRE_FADE_OUT_RATE) * dt;
		source.strength = source.strength < target ? std::min(target, source.strength + step) : std::max(target, source.strength - step);
	}
	std::erase_if(fireSources, [this](const FireSource& source) {
		return source.strength <= 0.0f && fireTick - source.lastSeenTick > FIRE_SEEN_GRACE_TICKS;
	});
}

void SnowCover::LoadSettings(json& o_json)
{
	settings = o_json;
}

void SnowCover::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SnowCover::RestoreDefaultSettings()
{
	settings = {};
}

void SnowCover::CheckFireSource(RE::BSRenderPass* a_pass, uint32_t a_descriptor)
{
	if (!settings.EnableFireMelt || !wsettings.EnableSnowCover)
		return;
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return;

	using EffectFlags = SIE::ShaderCache::EffectShaderFlags;
	const bool addBlend = (a_descriptor & static_cast<uint32_t>(EffectFlags::AddBlend)) != 0;
	const bool soft = (a_descriptor & static_cast<uint32_t>(EffectFlags::Soft)) != 0;
	const bool grayscale = (a_descriptor & static_cast<uint32_t>(EffectFlags::GrayscaleToColor)) != 0;
	// Effects11's own classifier. Flames are authored both with and without a palette, so
	// GRAYSCALE_TO_COLOR must never be a requirement; this drops only soft-no-palette smoke and glow.
	if (!addBlend || (soft && !grayscale))
		return;

	const RE::NiPoint3 center = a_pass->geometry->worldBound.center;
	const float boundRadius = a_pass->geometry->worldBound.radius;
	if (boundRadius <= 0.0f || boundRadius > FIRE_MAX_BOUND_RADIUS)
		return;

	if ((center - Util::GetEyePosition()).Length() > settings.FireMaxDistance)
		return;

	if (a_pass->shaderProperty->GetRTTI() != globals::rtti::BSEffectShaderPropertyRTTI.get())
		return;
	auto material = static_cast<RE::BSEffectShaderMaterial*>(a_pass->shaderProperty->material);
	if (!material)
		return;

	// Precision guard, run last: every real flame carries one of these in its source or greyscale path.
	static constexpr std::array<std::string_view, 6> kFireTokens{ "fire", "flame", "candle", "torch", "lava", "magma" };
	const auto hasFireToken = [](const RE::BSFixedString& a_path) {
		const char* raw = a_path.c_str();
		if (!raw)
			return false;
		std::string lowered(raw);
		std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return std::ranges::any_of(kFireTokens, [&](std::string_view token) { return lowered.find(token) != std::string::npos; });
	};
	if (!hasFireToken(material->sourceTexturePath) && !hasFireToken(material->greyscaleTexturePath))
		return;

	const float meltRadius = std::min(boundRadius * settings.FireRadiusScale, FIRE_MAX_MELT_RADIUS);

	std::lock_guard lock{ fireSourcesMutex };
	for (auto& source : fireSources) {
		if ((source.position - center).Length() < FIRE_MERGE_DISTANCE) {
			source.position = center;
			source.radius = std::max(source.radius, meltRadius);
			source.lastSeenTick = fireTick;
			return;
		}
	}
	if (fireSources.size() < MAX_FIRE_SOURCES)
		fireSources.emplace_back(FireSource{ center, meltRadius, 0.0f, fireTick });
}

void SnowCover::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	if (globals::features::snowCover.wsettings.EnableSnowCover) {
		globals::features::snowCover.BSLightingShader_Setup(Pass);
	}
	func(This, Pass, RenderFlags);
}

void SnowCover::Hooks::BSEffectShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::snowCover.CheckFireSource(Pass, globals::state->currentPixelDescriptor);
	func(This, Pass, RenderFlags);
}

void SnowCover::BSLightingShader_Setup(RE::BSRenderPass* a_pass)
{
	auto state = globals::state;
	auto userData = a_pass->geometry->GetUserData();
	auto name = a_pass->geometry->name.c_str();
	// Hash once per pass; fnv_hash dereferences unguarded and BSFixedString::c_str() can be null.
	const uint64_t nameHash = name ? FormIdParser::fnv_hash(name) : 0;
	if ((a_pass->geometry->HasAnimation() || (userData && ((userData->GetObjectReference() && userData->GetObjectReference()->IsBoundAnimObject()) || userData->CanBeMoved()))) && !(name && whitelist.contains(nameHash))) {
		if (settings.AffectHavok && userData && userData->formType != RE::FormType::ActorCharacter && userData->CanBeMoved())
			state->permutationData.ExtraShaderDescriptor &= ~(uint)State::ExtraShaderDescriptors::NoSnow;
		else
			state->permutationData.ExtraShaderDescriptor |= (uint)State::ExtraShaderDescriptors::NoSnow;
	} else if (name && blacklist.contains(nameHash)) {
		state->permutationData.ExtraShaderDescriptor |= (uint)State::ExtraShaderDescriptors::NoSnow;
	} else {
		state->permutationData.ExtraShaderDescriptor &= ~(uint)State::ExtraShaderDescriptors::NoSnow;
	}

	if (!settings.AffectFloraTint && IsHarvestableFlora(userData))
		state->permutationData.ExtraShaderDescriptor |= (uint)State::ExtraShaderDescriptors::NoFoliageTint;
	else
		state->permutationData.ExtraShaderDescriptor &= ~(uint)State::ExtraShaderDescriptors::NoFoliageTint;
}

bool SnowCover::IsHarvestableFlora(RE::TESObjectREFR* a_ref)
{
	if (!a_ref)
		return false;
	auto base = a_ref->GetObjectReference();
	if (!base)
		return false;
	switch (base->GetFormType()) {
	case RE::FormType::Flora:
		return true;
	case RE::FormType::Tree:
		return static_cast<RE::TESObjectTREE*>(base)->produceItem != nullptr;
	default:
		return false;
	}
}
