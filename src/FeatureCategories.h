#pragma once
#include <array>
#include <string_view>

/** @brief Canonical category labels used to group features in the menu UI. */
namespace FeatureCategories
{
	inline constexpr std::string_view kCharacters = "Characters";
	inline constexpr std::string_view kDisplay = "Display";
	inline constexpr std::string_view kGrassAndFoliage = "Grass & Foliage";
	inline constexpr std::string_view kLandscapeAndTextures = "Landscape & Textures";
	inline constexpr std::string_view kLighting = "Lighting";
	inline constexpr std::string_view kMaterials = "Materials";
	inline constexpr std::string_view kOther = "Other";
	inline constexpr std::string_view kPostProcessing = "Post-Processing";
	inline constexpr std::string_view kShadows = "Shadows";
	inline constexpr std::string_view kSkyAndWeather = "Sky & Weather";
	inline constexpr std::string_view kUtility = "Utility";
	inline constexpr std::string_view kWater = "Water";

	inline constexpr std::array kOrdered = {
		kDisplay,
		kLighting,
		kShadows,
		kSkyAndWeather,
		kMaterials,
		kCharacters,
		kLandscapeAndTextures,
		kGrassAndFoliage,
		kWater,
		kPostProcessing,
		kOther,
	};
}
