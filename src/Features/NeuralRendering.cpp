#include "NeuralRendering.h"

#include "NeuralRendering/Backend.h"

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "HDRDisplay.h"
#include "LinearLighting.h"
#include "Menu.h"
#include "PostProcessing.h"
#include "PostProcessing/HistogramAutoExposure.h"
#include "ReverseZ.h"
#include "ScreenshotFeature.h"
#include "State.h"
#include "Upscaling.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "Utils/UI.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <optional>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NeuralRendering::CategoryStrengths,
	colorStrength,
	transferStrength,
	broadLuminosity,
	detailLuminosity,
	hueGuard);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NeuralRendering::Settings,
	enabled,
	preset,
	userPreset,
	showAdvanced,
	placement,
	style,
	intensity,
	colorStrength,
	proxyCurve,
	localToneStrength,
	localStructureStrength,
	skinStructureStrength,
	automaticMask,
	resolutionMode,
	resolutionScale,
	resolutionScaleX,
	resolutionScaleY,
	transferStrength,
	broadLuminosity,
	detailLuminosity,
	bandRadius,
	maxRatio,
	ratioGuardEnabled,
	everythingElseStrengths,
	skinStrengths,
	hairStrengths,
	eyesStrengths,
	foliageStrengths,
	landscapeStrengths,
	equipmentStrengths,
	depthAwareResolve,
	alternateFrames,
	debugCategoryView,
	rawModelOutput);

namespace
{
	using Preset = NeuralRendering::Preset;
	using ProxyCurve = NeuralRendering::ProxyCurve;
	using PresetValues = NeuralRendering::PresetValues;

	/// Neutral per-category block: no override of the global strengths, no hue guard.
	constexpr NeuralRendering::CategoryStrengths kNeutralCategory{ 1.0f, 1.0f, 1.0f, 1.0f, false };

	/**
	 * Preset defaults in Preset order. Vanilla-Plus uses the Legacy proxy and a luminance-only edit capped
	 * at one stop.
	 */
	constexpr PresetValues kPresets[NeuralRendering::kPresetCount] = {
		// Full
		{
			static_cast<uint>(NeuralRendering::Placement::kFinishedImage),
			0,      // Default style
			1.0f,   // intensity
			1.0f,   // localToneStrength
			1.0f,   // localStructureStrength
			-1.0f,  // skinStructureStrength: automatic
			true,   // automaticMask
			static_cast<uint>(ProxyCurve::kDisplayMatched),
			1.0f,   // colorStrength
			1.0f,   // transferStrength
			1.0f,   // broadLuminosity
			1.0f,   // detailLuminosity
			8.0f,   // bandRadius
			false,  // ratioGuardEnabled
			2.0f,   // maxRatio
			false,  // depthAwareResolve
			{ {
				kNeutralCategory,                   // Everything Else
				{ 0.6f, 1.0f, 1.0f, 1.0f, false },  // Skin: damp the model's skin tint
				{ 1.0f, 1.0f, 1.0f, 1.0f, true },   // Hair: hue-guarded
				kNeutralCategory,                   // Eyes
				kNeutralCategory,                   // Foliage
				kNeutralCategory,                   // Landscape
				kNeutralCategory,                   // Equipment
			} },
		},
		// Vanilla-Plus
		{
			static_cast<uint>(NeuralRendering::Placement::kAfterUpscaling),
			2,      // Cinematic style
			0.8f,   // intensity
			0.75f,  // localToneStrength
			0.9f,   // localStructureStrength
			0.9f,   // skinStructureStrength
			true,   // automaticMask
			static_cast<uint>(ProxyCurve::kLegacy),
			0.0f,   // colorStrength: luminance-only
			1.0f,   // transferStrength
			1.0f,   // broadLuminosity
			1.0f,   // detailLuminosity
			8.0f,   // bandRadius
			true,   // ratioGuardEnabled
			2.0f,   // maxRatio: +/-1 stop
			false,  // depthAwareResolve
			{ {
				kNeutralCategory,
				kNeutralCategory,
				kNeutralCategory,
				kNeutralCategory,
				kNeutralCategory,
				kNeutralCategory,
				kNeutralCategory,
			} },
		},
	};

	bool NearlyEqual(float a_left, float a_right)
	{
		return std::abs(a_left - a_right) <= 1e-4f;
	}

	/// JSON key of each per-category block, in CategorySettings() order.
	constexpr std::array<const char*, NeuralRendering::kMaterialCategoryCount> kCategoryKeys{ "everythingElseStrengths",
		"skinStrengths", "hairStrengths", "eyesStrengths", "foliageStrengths", "landscapeStrengths",
		"equipmentStrengths" };

	/// Format of a user preset file, for a future migration to key off.
	constexpr int kUserPresetFormat = 1;

	void SanitizeFloat(float& a_value, float a_fallback, float a_min, float a_max)
	{
		if (!std::isfinite(a_value))
			a_value = a_fallback;
		a_value = std::clamp(a_value, a_min, a_max);
	}

	/// Clamps every value a preset owns into range; shared by LoadSettings and preset files.
	void SanitizePresetValues(PresetValues& a_values)
	{
		if (a_values.placement > 3) {
			logger::warn("[NeuralRendering] Loaded placement {} out of range, clamping to 1", a_values.placement);
			a_values.placement = 1;
		}
		if (a_values.style > 2)
			a_values.style = 2;
		SanitizeFloat(a_values.intensity, 0.8f, 0.0f, 2.0f);
		SanitizeFloat(a_values.colorStrength, 1.0f, 0.0f, 2.0f);
		SanitizeFloat(a_values.localToneStrength, 1.0f, 0.0f, 2.0f);
		SanitizeFloat(a_values.localStructureStrength, 1.0f, 0.0f, 2.0f);
		SanitizeFloat(a_values.skinStructureStrength, -1.0f, -1.0f, 2.0f);
		SanitizeFloat(a_values.transferStrength, 1.0f, 0.0f, 2.0f);
		SanitizeFloat(a_values.broadLuminosity, 1.0f, 0.0f, 2.0f);
		SanitizeFloat(a_values.detailLuminosity, 1.0f, 0.0f, 2.0f);
		SanitizeFloat(a_values.bandRadius, 8.0f, 2.0f, 32.0f);
		SanitizeFloat(a_values.maxRatio, 2.0f, 1.0f, 8.0f);
		// Out of range (including the retired HDR Linear, 3) falls back to the proxy the default
		// preset uses.
		if (a_values.proxyCurve >= static_cast<uint>(ProxyCurve::kCount))
			a_values.proxyCurve = static_cast<uint>(ProxyCurve::kDisplayMatched);
		for (auto& category : a_values.categories) {
			SanitizeFloat(category.colorStrength, 1.0f, 0.0f, 2.0f);
			SanitizeFloat(category.transferStrength, 1.0f, 0.0f, 2.0f);
			SanitizeFloat(category.broadLuminosity, 1.0f, 0.0f, 2.0f);
			SanitizeFloat(category.detailLuminosity, 1.0f, 0.0f, 2.0f);
		}
	}

	json PresetValuesToJson(const PresetValues& a_values)
	{
		json result = {
			{ "format", kUserPresetFormat },
			{ "placement", a_values.placement },
			{ "style", a_values.style },
			{ "intensity", a_values.intensity },
			{ "localToneStrength", a_values.localToneStrength },
			{ "localStructureStrength", a_values.localStructureStrength },
			{ "skinStructureStrength", a_values.skinStructureStrength },
			{ "automaticMask", a_values.automaticMask },
			{ "proxyCurve", a_values.proxyCurve },
			{ "colorStrength", a_values.colorStrength },
			{ "transferStrength", a_values.transferStrength },
			{ "broadLuminosity", a_values.broadLuminosity },
			{ "detailLuminosity", a_values.detailLuminosity },
			{ "bandRadius", a_values.bandRadius },
			{ "ratioGuardEnabled", a_values.ratioGuardEnabled },
			{ "maxRatio", a_values.maxRatio },
			{ "depthAwareResolve", a_values.depthAwareResolve },
		};
		for (std::size_t index = 0; index < NeuralRendering::kMaterialCategoryCount; ++index)
			result[kCategoryKeys[index]] = a_values.categories[index];
		return result;
	}

	/// Reads a preset file's values over @p a_fallback; throws on a wrongly typed key.
	PresetValues PresetValuesFromJson(const json& a_json, const PresetValues& a_fallback)
	{
		PresetValues values = a_fallback;
		values.placement = a_json.value("placement", values.placement);
		values.style = a_json.value("style", values.style);
		values.intensity = a_json.value("intensity", values.intensity);
		values.localToneStrength = a_json.value("localToneStrength", values.localToneStrength);
		values.localStructureStrength = a_json.value("localStructureStrength", values.localStructureStrength);
		values.skinStructureStrength = a_json.value("skinStructureStrength", values.skinStructureStrength);
		values.automaticMask = a_json.value("automaticMask", values.automaticMask);
		values.proxyCurve = a_json.value("proxyCurve", values.proxyCurve);
		values.colorStrength = a_json.value("colorStrength", values.colorStrength);
		values.transferStrength = a_json.value("transferStrength", values.transferStrength);
		values.broadLuminosity = a_json.value("broadLuminosity", values.broadLuminosity);
		values.detailLuminosity = a_json.value("detailLuminosity", values.detailLuminosity);
		values.bandRadius = a_json.value("bandRadius", values.bandRadius);
		values.ratioGuardEnabled = a_json.value("ratioGuardEnabled", values.ratioGuardEnabled);
		values.maxRatio = a_json.value("maxRatio", values.maxRatio);
		values.depthAwareResolve = a_json.value("depthAwareResolve", values.depthAwareResolve);
		for (std::size_t index = 0; index < NeuralRendering::kMaterialCategoryCount; ++index) {
			if (const auto block = a_json.find(kCategoryKeys[index]); block != a_json.end() && block->is_object())
				values.categories[index] = block->get<NeuralRendering::CategoryStrengths>();
		}
		return values;
	}

	// Preset names come from ImGui as UTF-8; std::filesystem::path(std::string) would read
	// them in the ANSI code page instead.
	std::filesystem::path PathFromUtf8(std::string_view a_text)
	{
		return std::filesystem::path(std::u8string(a_text.begin(), a_text.end()));
	}

	std::string Utf8FromPath(const std::filesystem::path& a_path)
	{
		const auto text = a_path.u8string();
		return std::string(text.begin(), text.end());
	}

	std::filesystem::path UserPresetPath(const std::string& a_name)
	{
		return NeuralRendering::UserPresetDirectory() / PathFromUtf8(a_name + ".json");
	}

	Util::ConfirmationPopup& DeletePresetPopup()
	{
		static Util::ConfirmationPopup popup;
		return popup;
	}

	NeuralRenderingBackend::FrameInputs MakeFrameInputs(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
		ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV, ID3D11ShaderResourceView* materialCategoriesSRV,
		ID3D11Resource* motionVectors, ID3D11ShaderResourceView* motionVectorsSRV,
		uint32_t width, uint32_t height, const NeuralRendering::Options& options)
	{
		NeuralRenderingBackend::FrameInputs inputs;
		inputs.colorIn = colorIn;
		inputs.colorOut = colorOut;
		inputs.depth = depth;
		inputs.depthSRV = depthSRV;
		inputs.materialCategoriesSRV = materialCategoriesSRV;
		inputs.motionVectors = motionVectors;
		inputs.motionVectorsSRV = motionVectorsSRV;
		inputs.width = width;
		inputs.height = height;
		inputs.guideWidth = options.guideWidth ? options.guideWidth : width;
		inputs.guideHeight = options.guideHeight ? options.guideHeight : height;
		inputs.jitterOffsetX = options.jitterOffsetX;
		inputs.jitterOffsetY = options.jitterOffsetY;
		inputs.guideJitterOffsetX = options.guideJitterOffsetX;
		inputs.guideJitterOffsetY = options.guideJitterOffsetY;
		inputs.resolutionScaleX = options.resolutionScaleX;
		inputs.resolutionScaleY = options.resolutionScaleY;
		inputs.colorDomain = static_cast<std::uint32_t>(options.colorDomain);
		inputs.proxyCurve = static_cast<std::uint32_t>(options.proxyCurve);
		inputs.display.vanillaGrading = options.display.vanillaGrading;
		std::copy_n(options.display.param, 4, inputs.display.param);
		std::copy_n(options.display.cinematic, 4, inputs.display.cinematic);
		std::copy_n(options.display.tint, 4, inputs.display.tint);
		inputs.display.vanillaAdaptationSRV = options.display.vanillaAdaptationSRV;
		inputs.display.postProcessExposure = options.display.postProcessExposure;
		inputs.display.postProcessAdaptationSRV = options.display.postProcessAdaptationSRV;
		inputs.display.postProcessExposureScale = options.display.postProcessExposureScale;
		inputs.highlightWhite = options.highlightWhite;
		inputs.wipePosition = options.wipePosition;
		inputs.staticMotion = options.staticMotion;
		std::copy_n(options.display.postProcessAdaptationRange, 2, inputs.display.postProcessAdaptationRange);
		inputs.intensity = options.intensity;
		inputs.colorStrength = options.colorStrength;
		inputs.transferStrength = options.transferStrength;
		inputs.broadLuminosity = options.broadLuminosity;
		inputs.detailLuminosity = options.detailLuminosity;
		inputs.bandRadius = options.bandRadius;
		inputs.maxRatio = options.maxRatio;
		inputs.ratioGuardEnabled = options.ratioGuardEnabled;
		for (std::size_t index = 0; index < options.categoryStrengths.size(); ++index) {
			inputs.categoryColorStrengths[index] = options.categoryStrengths[index].colorStrength;
			inputs.categoryTransferStrengths[index] = options.categoryStrengths[index].transferStrength;
			inputs.categoryBroadLuminosity[index] = options.categoryStrengths[index].broadLuminosity;
			inputs.categoryDetailLuminosity[index] = options.categoryStrengths[index].detailLuminosity;
			inputs.categoryHueGuard[index] = options.categoryStrengths[index].hueGuard;
		}
		inputs.depthAwareResolve = options.depthAwareResolve;
		inputs.alternateFrames = options.alternateFrames;
		inputs.localToneStrength = options.localToneStrength;
		inputs.localStructureStrength = options.localStructureStrength;
		inputs.skinStructureStrength = options.skinStructureStrength;
		inputs.style = options.style;
		inputs.superResolutionQualityMode = options.superResolutionQualityMode;
		inputs.superResolutionPreset = options.superResolutionPreset;
		inputs.automaticMask = options.automaticMask;
		inputs.debugCategoryView = options.debugCategoryView;
		inputs.rawModelOutput = options.rawModelOutput;
		inputs.debugGuardClamp = options.debugGuardClamp;
		inputs.debugBroadBand = options.debugBroadBand;
		inputs.debugDetailBand = options.debugDetailBand;
		inputs.measureModelPeak = options.measureModelPeak;
		inputs.reset = options.reset;
		// Reverse Z is latched at boot, so the private DLSS SR's create-time flag stays valid.
		inputs.depthInverted = globals::features::reverseZ.IsActive();
		return inputs;
	}

	template <class T>
	void ReleaseTexture(T*& a_texture)
	{
		if (!a_texture)
			return;
		delete a_texture;
		a_texture = nullptr;
	}

	// ShowHUDMessage must run on the game's main thread; the frame hooks run on the render thread.
	void ShowHUDMessageDeferred(const char* a_message)
	{
		if (auto* task = SKSE::GetTaskInterface())
			task->AddTask([msg = std::string(a_message)]() { RE::SendHUDMessage::ShowHUDMessage(msg.c_str(), nullptr, true); });
		else
			RE::SendHUDMessage::ShowHUDMessage(a_message, nullptr, true);
	}

	bool IsHairHeadPart(const RE::BGSHeadPart* a_part)
	{
		using HeadPartType = RE::BGSHeadPart::HeadPartType;
		return a_part && (a_part->type == HeadPartType::kHair || a_part->type == HeadPartType::kFacialHair);
	}

	// Whether any of a_parts, or one of their extra parts (hairlines ride along
	// as extra parts of their hair), is a hair head part named a_partName.
	bool MatchesHairHeadPart(RE::BGSHeadPart** a_parts, std::uint32_t a_count, const RE::BSFixedString& a_partName)
	{
		if (!a_parts)
			return false;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			const auto* part = a_parts[i];
			if (!IsHairHeadPart(part))
				continue;
			if (part->formEditorID == a_partName)
				return true;
			for (const auto* extra : part->extraParts) {
				if (extra && extra->formEditorID == a_partName)
					return true;
			}
		}
		return false;
	}

	// Material-based hair detection also covers wigs without matching head parts.
	bool IsHairTintShader(const RE::BSRenderPass* a_pass)
	{
		if (!a_pass->shaderProperty || a_pass->shaderProperty->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
			return false;
		const auto* lightingProperty = static_cast<const RE::BSLightingShaderProperty*>(a_pass->shaderProperty);
		return (lightingProperty->material && lightingProperty->material->GetFeature() == RE::BSShaderMaterial::Feature::kHairTint) ||
		       lightingProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kHairTint);
	}

	// Head parts are direct children of the skinned face node, named by editor ID. Match the geometry
	// ancestor to catch hair authored with non-hair materials.
	bool IsHairHeadPartGeometry(RE::Actor* a_actor, const RE::BSGeometry* a_geometry)
	{
		const auto* faceNode = a_actor->GetFaceNodeSkinned();
		if (!faceNode)
			return false;

		const RE::NiAVObject* partRoot = a_geometry;
		while (partRoot && partRoot->parent != faceNode)
			partRoot = partRoot->parent;
		if (!partRoot)
			return false;

		auto* npc = a_actor->GetActorBase();
		if (!npc)
			return false;
		if (npc->HasOverlays() && MatchesHairHeadPart(npc->GetBaseOverlays(), npc->GetNumBaseOverlays(), partRoot->name))
			return true;
		return MatchesHairHeadPart(npc->headParts, static_cast<std::uint32_t>(std::max<std::int8_t>(npc->numHeadParts, 0)), partRoot->name);
	}
}

NeuralRendering::NeuralRendering() :
	backend(std::make_unique<NeuralRenderingBackend>())
{}

NeuralRendering::~NeuralRendering() = default;
// Backend adapter

bool NeuralRendering::IsAvailable() const
{
	return backend->IsAvailable();
}

bool NeuralRendering::IsFeatureAvailable() const
{
	return backend->IsFeatureAvailable();
}

NeuralRendering::DebugReadback NeuralRendering::GetDebugReadback() const
{
	const auto readback = backend->GetDebugReadback();
	return { readback.modelPeakLuminance, readback.guardClampedPercent, readback.valid };
}

bool NeuralRendering::Evaluate(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
	ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
	ID3D11ShaderResourceView* materialCategoriesSRV,
	ID3D11Resource* motionVectors, ID3D11ShaderResourceView* motionVectorsSRV,
	uint32_t width, uint32_t height, const Options& options)
{
	// Each placement is 1:1 in colour/output space. After-upscale colour is
	// display-resolution while the depth and motion guides retain the render
	// resolution used by DLSS SR, so their extents are carried independently.
	return backend->Evaluate(MakeFrameInputs(colorIn, colorOut, depth, depthSRV,
		materialCategoriesSRV, motionVectors, motionVectorsSRV, width, height, options));
}

bool NeuralRendering::PrepareSeparateUpscaling(ID3D11Resource* colorIn, ID3D11Resource* editedColor,
	ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
	ID3D11ShaderResourceView* materialCategoriesSRV,
	ID3D11Resource* motionVectors, ID3D11ShaderResourceView* motionVectorsSRV,
	ID3D11Resource* superResolutionMotionVectors,
	uint32_t width, uint32_t height,
	uint32_t outputWidth, uint32_t outputHeight, const Options& options)
{
	auto inputs = MakeFrameInputs(colorIn, editedColor, depth, depthSRV, materialCategoriesSRV, motionVectors,
		motionVectorsSRV, width, height, options);
	inputs.superResolutionMotionVectors = superResolutionMotionVectors;
	inputs.outputWidth = outputWidth;
	inputs.outputHeight = outputHeight;
	return backend->PrepareSeparateUpscaling(inputs);
}

bool NeuralRendering::ResolveSeparateUpscaling(ID3D11Resource* cleanColor, ID3D11Resource* colorOut,
	uint32_t width, uint32_t height)
{
	return backend->ResolveSeparateUpscaling(cleanColor, colorOut, width, height);
}

void NeuralRendering::DestroyModelResources()
{
	backend->DestroyResources();
}
// Settings

std::array<NeuralRendering::CategoryStrengths*, NeuralRendering::kMaterialCategoryCount> NeuralRendering::CategorySettings()
{
	return { &settings.everythingElseStrengths, &settings.skinStrengths, &settings.hairStrengths,
		&settings.eyesStrengths, &settings.foliageStrengths, &settings.landscapeStrengths,
		&settings.equipmentStrengths };
}

std::array<const NeuralRendering::CategoryStrengths*, NeuralRendering::kMaterialCategoryCount> NeuralRendering::CategorySettings() const
{
	return { &settings.everythingElseStrengths, &settings.skinStrengths, &settings.hairStrengths,
		&settings.eyesStrengths, &settings.foliageStrengths, &settings.landscapeStrengths,
		&settings.equipmentStrengths };
}

bool NeuralRendering::BandsSeparated() const
{
	// The resolve scales the smooth band by global x category Broad and the remainder by
	// global x category Detail; splitting only changes anything where those differ.
	for (const auto* category : CategorySettings()) {
		if (!NearlyEqual(settings.broadLuminosity * category->broadLuminosity,
				settings.detailLuminosity * category->detailLuminosity))
			return true;
	}
	return false;
}

const NeuralRendering::PresetValues& NeuralRendering::GetPreset(Preset a_preset)
{
	const auto index = std::min(static_cast<std::size_t>(a_preset), kPresetCount - 1);
	return kPresets[index];
}

void NeuralRendering::ApplyPreset(Preset a_preset)
{
	ApplyPresetValues(GetPreset(a_preset));
	settings.preset = static_cast<uint>(a_preset);
	settings.userPreset.clear();
}

bool NeuralRendering::MatchesPreset(Preset a_preset) const
{
	return MatchesPresetValues(GetPreset(a_preset));
}

NeuralRendering::PresetValues NeuralRendering::CapturePresetValues() const
{
	PresetValues values{};
	values.placement = settings.placement;
	values.style = settings.style;
	values.intensity = settings.intensity;
	values.localToneStrength = settings.localToneStrength;
	values.localStructureStrength = settings.localStructureStrength;
	values.skinStructureStrength = settings.skinStructureStrength;
	values.automaticMask = settings.automaticMask;
	values.proxyCurve = settings.proxyCurve;
	values.colorStrength = settings.colorStrength;
	values.transferStrength = settings.transferStrength;
	values.broadLuminosity = settings.broadLuminosity;
	values.detailLuminosity = settings.detailLuminosity;
	values.bandRadius = settings.bandRadius;
	values.ratioGuardEnabled = settings.ratioGuardEnabled;
	values.maxRatio = settings.maxRatio;
	values.depthAwareResolve = settings.depthAwareResolve;
	const auto categories = CategorySettings();
	for (std::size_t index = 0; index < kMaterialCategoryCount; ++index)
		values.categories[index] = *categories[index];
	return values;
}

void NeuralRendering::ApplyPresetValues(const PresetValues& a_values)
{
	settings.placement = a_values.placement;
	settings.style = a_values.style;
	settings.intensity = a_values.intensity;
	settings.localToneStrength = a_values.localToneStrength;
	settings.localStructureStrength = a_values.localStructureStrength;
	settings.skinStructureStrength = a_values.skinStructureStrength;
	settings.automaticMask = a_values.automaticMask;
	settings.proxyCurve = a_values.proxyCurve;
	settings.colorStrength = a_values.colorStrength;
	settings.transferStrength = a_values.transferStrength;
	settings.broadLuminosity = a_values.broadLuminosity;
	settings.detailLuminosity = a_values.detailLuminosity;
	settings.bandRadius = a_values.bandRadius;
	settings.ratioGuardEnabled = a_values.ratioGuardEnabled;
	settings.maxRatio = a_values.maxRatio;
	settings.depthAwareResolve = a_values.depthAwareResolve;
	auto categories = CategorySettings();
	for (std::size_t index = 0; index < kMaterialCategoryCount; ++index)
		*categories[index] = a_values.categories[index];
}

bool NeuralRendering::MatchesPresetValues(const PresetValues& a_values) const
{
	const auto& preset = a_values;
	// Placement and NR Intensity are basic controls; moving either keeps the preset.
	if (settings.style != preset.style || settings.automaticMask != preset.automaticMask ||
		settings.proxyCurve != preset.proxyCurve || settings.ratioGuardEnabled != preset.ratioGuardEnabled ||
		settings.depthAwareResolve != preset.depthAwareResolve)
		return false;
	if (!NearlyEqual(settings.localToneStrength, preset.localToneStrength) ||
		!NearlyEqual(settings.localStructureStrength, preset.localStructureStrength) ||
		!NearlyEqual(settings.skinStructureStrength, preset.skinStructureStrength) ||
		!NearlyEqual(settings.colorStrength, preset.colorStrength) ||
		!NearlyEqual(settings.transferStrength, preset.transferStrength) ||
		!NearlyEqual(settings.broadLuminosity, preset.broadLuminosity) ||
		!NearlyEqual(settings.detailLuminosity, preset.detailLuminosity))
		return false;
	// Max Ratio only exists while the guard is on, and Band Radius only while the bands are
	// separated, so an unused (and hidden) stored value cannot leave a preset looking
	// modified.
	if (settings.ratioGuardEnabled && !NearlyEqual(settings.maxRatio, preset.maxRatio))
		return false;
	if (BandsSeparated() && !NearlyEqual(settings.bandRadius, preset.bandRadius))
		return false;
	const auto categories = CategorySettings();
	for (std::size_t index = 0; index < kMaterialCategoryCount; ++index) {
		const auto& current = *categories[index];
		const auto& expected = preset.categories[index];
		if (current.hueGuard != expected.hueGuard ||
			!NearlyEqual(current.colorStrength, expected.colorStrength) ||
			!NearlyEqual(current.transferStrength, expected.transferStrength) ||
			!NearlyEqual(current.broadLuminosity, expected.broadLuminosity) ||
			!NearlyEqual(current.detailLuminosity, expected.detailLuminosity))
			return false;
	}
	return true;
}
// User presets

std::filesystem::path NeuralRendering::UserPresetDirectory()
{
	return Util::PathHelpers::GetCommunityShaderPath() / "NeuralRendering" / "Presets";
}

void NeuralRendering::RefreshUserPresets()
{
	userPresets.clear();
	userPresetsLoaded = true;
	const auto directory = UserPresetDirectory();
	std::error_code error;
	if (!std::filesystem::is_directory(directory, error))
		return;
	try {
		for (const auto& entry : std::filesystem::directory_iterator(directory)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".json")
				continue;
			const auto name = Utf8FromPath(entry.path().stem());
			try {
				std::ifstream file{ entry.path() };
				const json data = json::parse(file);
				if (!data.is_object())
					throw std::runtime_error("not a JSON object");
				auto values = PresetValuesFromJson(data, GetPreset(Preset::kFull));
				SanitizePresetValues(values);
				userPresets.push_back(UserPreset{ name, values });
			} catch (const std::exception& e) {
				logger::warn("[NeuralRendering] Skipping preset file '{}': {}", name, e.what());
			}
		}
	} catch (const std::exception& e) {
		logger::warn("[NeuralRendering] Could not list presets in {}: {}", Utf8FromPath(directory), e.what());
	}
	std::ranges::sort(userPresets, [](const UserPreset& a_left, const UserPreset& a_right) {
		return _stricmp(a_left.name.c_str(), a_right.name.c_str()) < 0;
	});
}

const NeuralRendering::UserPreset* NeuralRendering::FindUserPreset(std::string_view a_name) const
{
	const auto found = std::ranges::find(userPresets, a_name, &UserPreset::name);
	return found != userPresets.end() ? &*found : nullptr;
}

void NeuralRendering::ApplyUserPreset(const UserPreset& a_preset)
{
	ApplyPresetValues(a_preset.values);
	settings.userPreset = a_preset.name;
}

bool NeuralRendering::WriteUserPreset(const std::string& a_name, const PresetValues& a_values)
{
	const auto path = UserPresetPath(a_name);
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	{
		std::ofstream file{ path, std::ios::trunc };
		if (file)
			file << PresetValuesToJson(a_values).dump(4);
		if (!file) {
			logger::warn("[NeuralRendering] Could not write preset file {}", Utf8FromPath(path));
			return false;
		}
	}
	logger::info("[NeuralRendering] Saved preset '{}'", a_name);

	if (const auto existing = std::ranges::find(userPresets, a_name, &UserPreset::name); existing != userPresets.end()) {
		existing->values = a_values;
	} else {
		const auto position = std::ranges::find_if(userPresets, [&](const UserPreset& a_preset) {
			return _stricmp(a_name.c_str(), a_preset.name.c_str()) < 0;
		});
		userPresets.insert(position, UserPreset{ a_name, a_values });
	}
	return true;
}

bool NeuralRendering::RenameUserPreset(const std::string& a_from, const std::string& a_to)
{
	std::error_code error;
	std::filesystem::rename(UserPresetPath(a_from), UserPresetPath(a_to), error);
	if (error) {
		logger::warn("[NeuralRendering] Could not rename preset '{}' to '{}': {}", a_from, a_to, error.message());
		return false;
	}
	logger::info("[NeuralRendering] Renamed preset '{}' to '{}'", a_from, a_to);
	if (settings.userPreset == a_from)
		settings.userPreset = a_to;
	RefreshUserPresets();
	return true;
}

bool NeuralRendering::DeleteUserPreset(const std::string& a_name)
{
	std::error_code error;
	std::filesystem::remove(UserPresetPath(a_name), error);
	if (error) {
		logger::warn("[NeuralRendering] Could not delete preset '{}': {}", a_name, error.message());
		return false;
	}
	logger::info("[NeuralRendering] Deleted preset '{}'", a_name);
	if (settings.userPreset == a_name)
		settings.userPreset.clear();
	std::erase_if(userPresets, [&](const UserPreset& a_preset) { return a_preset.name == a_name; });
	return true;
}

NeuralRendering::ProxyCurve NeuralRendering::ResolveProxyCurve() const
{
	const auto stored = settings.proxyCurve;
	return stored < static_cast<uint>(ProxyCurve::kCount) ? static_cast<ProxyCurve>(stored) : ProxyCurve::kDisplayMatched;
}

bool NeuralRendering::IsLinearLightingActive()
{
	return globals::features::linearLighting.settings.enableLinearLighting && !globals::state->IsFlatWorldMapOpen();
}

NeuralRendering::ColorDomain NeuralRendering::SceneColorDomain(ProxyCurve a_curve) const
{
	if (a_curve == ProxyCurve::kLegacy || IsLinearLightingActive())
		return ColorDomain::kSceneLinear;
	return ColorDomain::kSceneGamma;
}

#define I18N_KEY_PREFIX "feature.neural_rendering."

void NeuralRendering::DrawSettings()
{
	if (!IsDLSSActive()) {
		ImGui::TextDisabled("%s", T(TKEY("requires_dlss"),
									  "Select DLSS in Upscaling to use Neural Rendering."));
		return;
	}

	const bool backendAvailable = IsAvailable();
	const bool featureAvailable = IsFeatureAvailable();
	if (!backendAvailable) {
		ImGui::TextDisabled("%s", T(TKEY("unavailable"),
									  "Neural Rendering is unavailable. Install a compatible nvngx_dlssnr.dll."));
	} else if (featureAvailable) {
		ImGui::TextUnformatted(T(TKEY("available"), "DLSS Neural Rendering is available."));
	} else {
		ImGui::TextDisabled("%s", T(TKEY("backend_ready"),
									  "Enable Neural Rendering to check compatibility."));
	}

	ImGui::Checkbox(T(TKEY("enabled"), "Enable Neural Rendering"), &settings.enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("enabled_tooltip"),
			"Enhances lighting, color, and detail with DLSS Neural Rendering. Requires a compatible nvngx_dlssnr.dll."));
	}

	ImGui::BeginDisabled(!backendAvailable);
	if (ImGui::Button(T(TKEY("compare_screenshot"), "Take Comparison Screenshot"))) {
		RequestComparisonCapture();
	}
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("compare_screenshot_tooltip"),
			"Saves matching screenshots with Neural Rendering off and on, without the HUD or menus, in Data/DLSS 5 Screenshots/. Causes a brief hitch. Requires DLSS with Frame Generation off."));
	}

	const bool controlsAvailable = settings.enabled && backendAvailable;
	if (!controlsAvailable)
		ImGui::BeginDisabled();
	DrawPresetControls();

	ImGui::Checkbox(T(TKEY("show_advanced"), "Show Advanced Settings"), &settings.showAdvanced);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("show_advanced_tooltip"),
			"Show model tuning, color and lighting controls, material adjustments, and diagnostic views."));
	}
	const char* placementLabels[] = {
		T(TKEY("placement_before"), "Before Upscaling"),
		T(TKEY("placement_after"), "After Upscaling"),
		T(TKEY("placement_separate"), "Separate Upscaling (Experimental)"),
		T(TKEY("placement_finished_image"), "Finished Image")
	};
	int placement = static_cast<int>(settings.placement);
	if (ImGui::Combo(T(TKEY("placement"), "Placement"), &placement, placementLabels, IM_ARRAYSIZE(placementLabels)))
		settings.placement = static_cast<uint>(std::clamp(placement, 0, 3));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("placement_tooltip"),
			"Before Upscaling enhances the image before DLSS upscales it. After Upscaling enhances the upscaled image.\nSeparate Upscaling enhances a lower-resolution image and upscales the changes separately. Uses more GPU time and VRAM.\nFinished Image enhances the final image after tone mapping, Depth of Field, and Motion Blur. Does not run on the main menu or loading screens."));
	}

	const char* resolutionModeLabels[] = {
		T(TKEY("resolution_mode_uniform"), "Uniform"),
		T(TKEY("resolution_mode_per_axis"), "Per-Axis (Experimental)")
	};
	int resolutionMode = static_cast<int>(settings.resolutionMode);
	if (ImGui::Combo(T(TKEY("resolution_mode"), "Model Resolution"), &resolutionMode, resolutionModeLabels, IM_ARRAYSIZE(resolutionModeLabels)))
		settings.resolutionMode = static_cast<uint>(std::clamp(resolutionMode, 0, 1));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("resolution_mode_tooltip"),
			"Uniform scales model width and height equally. Per-Axis lets you adjust them separately to balance detail and performance."));
	}
	if (settings.resolutionMode == 0) {
		ImGui::SliderFloat(T(TKEY("resolution_scale"), "Resolution Scale"), &settings.resolutionScale, 0.25f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("resolution_scale_tooltip"),
				"Model resolution relative to the image it processes. Lower values improve performance while preserving original image detail, but may reduce enhancement quality. Changes apply when the slider settles."));
		}
	} else {
		ImGui::SliderFloat(T(TKEY("resolution_scale_x"), "Horizontal Scale"), &settings.resolutionScaleX, 0.25f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("resolution_scale_x_tooltip"),
				"Model width relative to the frame width. Changes apply once the slider settles."));
		}
		ImGui::SliderFloat(T(TKEY("resolution_scale_y"), "Vertical Scale"), &settings.resolutionScaleY, 0.25f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("resolution_scale_y_tooltip"),
				"Model height relative to the frame height. Changes apply once the slider settles."));
		}
	}
	ImGui::Checkbox(T(TKEY("alternate_frames"), "Alternate Frames (Experimental)"), &settings.alternateFrames);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("alternate_frames_tooltip"),
			"Runs Neural Rendering every other frame to reduce GPU load. Reuses the previous enhancement between frames; changes may trail fast motion."));
	}

	ImGui::SliderFloat(T(TKEY("intensity"), "NR Intensity"), &settings.intensity, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("intensity_tooltip"), "Adjust the overall enhancement intensity. Changes apply when the slider settles."));
	}

	// Runtime-only comparison controls.
	ImGui::Checkbox(T(TKEY("compare_wipe"), "Split Screen"), &compareView.wipe);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("compare_wipe_tooltip"),
			"Shows the frame without Neural Rendering left of the split and with it on the right, divided "
			"by a black and white line. Not saved."));
	}
	if (compareView.wipe)
		ImGui::SliderFloat(T(TKEY("compare_wipe_position"), "Split Position"), &compareView.wipePosition, 0.0f, 1.0f, "%.2f");

	if (settings.showAdvanced) {
		ImGui::Separator();
		ImGui::TextUnformatted(T(TKEY("model_inputs"), "Model Tuning"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("model_inputs_tooltip"),
				"Adjust how Neural Rendering enhances the image. Strengths below control how much of the enhancement is applied."));
		}

		const char* neuralStyles[] = {
			T(TKEY("style_default"), "Default"),
			T(TKEY("style_natural"), "Natural"),
			T(TKEY("style_cinematic"), "Cinematic")
		};
		int neuralStyle = static_cast<int>(settings.style);
		if (ImGui::Combo(T(TKEY("style"), "NR Style"), &neuralStyle, neuralStyles, IM_ARRAYSIZE(neuralStyles)))
			settings.style = static_cast<uint>(std::clamp(neuralStyle, 0, 2));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("style_tooltip"), "Choose the Neural Rendering visual style."));
		}

		ImGui::SliderFloat(T(TKEY("local_tone"), "Local Tone Strength"), &settings.localToneStrength, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("local_tone_tooltip"), "Adjust local tone detail."));
		}
		ImGui::SliderFloat(T(TKEY("local_structure"), "Local Structure Strength"), &settings.localStructureStrength, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("local_structure_tooltip"), "Adjust local structure detail."));
		}
		ImGui::SliderFloat(T(TKEY("skin_structure"), "Skin Structure Strength"), &settings.skinStructureStrength, -1.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("skin_structure_tooltip"), "Adjust skin detail. Set to -1 to use the automatic value."));
		}
		ImGui::Checkbox(T(TKEY("automatic_mask"), "Automatic Mask"), &settings.automaticMask);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("automatic_mask_tooltip"), "Generates the skin mask automatically."));
		}
		const bool finishedImage = IsPlacement(Placement::kFinishedImage);
		const char* proxyCurveLabels[] = {
			T(TKEY("proxy_display_matched"), "Display-matched"),
			T(TKEY("proxy_neutwo"), "Neutwo"),
			T(TKEY("proxy_legacy"), "Legacy")
		};
		ImGui::BeginDisabled(finishedImage);
		int proxyCurve = static_cast<int>(std::min<uint>(settings.proxyCurve, static_cast<uint>(ProxyCurve::kLegacy)));
		if (ImGui::Combo(T(TKEY("proxy_curve"), "Proxy Curve"), &proxyCurve, proxyCurveLabels, IM_ARRAYSIZE(proxyCurveLabels)))
			settings.proxyCurve = static_cast<uint>(std::clamp(proxyCurve, 0, 2));
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("proxy_curve_tooltip"),
				"Choose the color and brightness mapping used before enhancement. Has no effect on Finished Image.\nDisplay-matched follows your tone mapping and color grading. Neutwo uses a neutral curve with exposure adjustment. Legacy uses a fixed curve without exposure adjustment."));
		}
		ImGui::Separator();
		ImGui::TextUnformatted(T(TKEY("strengths"), "Strengths"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("strengths_tooltip"),
				"Adjust how much of the enhancement is applied. Per-category strengths multiply these values."));
		}

		ImGui::SliderFloat(T(TKEY("transfer_strength"), "Transfer Strength"), &settings.transferStrength, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("transfer_strength_tooltip"),
				"Overall enhancement strength: 0 leaves the image unchanged, 1 applies the full enhancement, and 2 exaggerates it. Applies immediately."));
		}
		ImGui::SliderFloat(T(TKEY("color_strength"), "Color Strength"), &settings.colorStrength, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("color_strength_tooltip"),
				"Adjust color changes independently of lighting and detail. 0 preserves original colors, 1 applies the full color change, and 2 exaggerates it."));
		}
		ImGui::SliderFloat(T(TKEY("broad_luminosity"), "Broad Luminosity"), &settings.broadLuminosity, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("broad_luminosity_tooltip"),
				"Adjust lighting changes across large areas. Lower values preserve more of the original lighting while retaining detail enhancements."));
		}
		ImGui::SliderFloat(T(TKEY("detail_luminosity"), "Detail Luminosity"), &settings.detailLuminosity, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("detail_luminosity_tooltip"),
				"Adjust changes to local contrast and fine detail. Match Broad Luminosity to adjust all lighting changes equally."));
		}
		if (BandsSeparated()) {
			ImGui::SliderFloat(T(TKEY("band_radius"), "Band Radius"), &settings.bandRadius, 2.0f, 32.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("band_radius_tooltip"),
					"Adjust the size of lighting changes assigned to Broad and Detail. Larger values put more changes in Detail; smaller values put more in Broad. Only used when their strengths differ."));
			}
		}
		ImGui::Checkbox(T(TKEY("ratio_guard_enabled"), "Enable Ratio Guard"), &settings.ratioGuardEnabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("ratio_guard_enabled_tooltip"),
				"Limit brightness changes to reduce flashes or flicker. May weaken intended lighting and shadow changes. Set the limit with Max Ratio."));
		}
		if (settings.ratioGuardEnabled) {
			ImGui::SliderFloat(T(TKEY("max_ratio"), "Max Ratio"), &settings.maxRatio, 1.0f, 8.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(T(TKEY("max_ratio_tooltip"),
					"Maximum brightness change in either direction. 2 allows half to twice the original brightness; 1 prevents brightness changes. Lower values provide a stricter limit."));
			}
		}
		ImGui::Separator();
		ImGui::TextUnformatted(T(TKEY("category_overrides"), "Per-Category Overrides"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("category_overrides_tooltip"),
				"Adjust each material category independently. Category strengths multiply the global strengths. Neutral Colour Guard reduces unwanted color shifts."));
		}

		DrawCategoryStrengths("Skin", T(TKEY("category_skin"), "Skin"), settings.skinStrengths);
		DrawCategoryStrengths("Hair", T(TKEY("category_hair"), "Hair"), settings.hairStrengths);
		DrawCategoryStrengths("Eyes", T(TKEY("category_eyes"), "Eyes"), settings.eyesStrengths);
		DrawCategoryStrengths("Foliage", T(TKEY("category_foliage"), "Foliage"), settings.foliageStrengths,
			T(TKEY("category_foliage_tooltip"), "Trees and grass."));
		DrawCategoryStrengths("Landscape", T(TKEY("category_landscape"), "Landscape"), settings.landscapeStrengths);
		DrawCategoryStrengths("Equipment", T(TKEY("category_equipment"), "Equipment"), settings.equipmentStrengths,
			T(TKEY("category_equipment_tooltip"), "Armor, clothing, and weapons worn or wielded by humanoid actors. Bare skin counts as Skin."));
		DrawCategoryStrengths("EverythingElse", T(TKEY("category_everything_else"), "Everything Else"),
			settings.everythingElseStrengths,
			T(TKEY("category_everything_else_tooltip"),
				"Architecture, clutter, water, sky, particles, and other unclassified areas."));

		ImGui::Separator();
		ImGui::Checkbox(T(TKEY("depth_aware_resolve"), "Depth-Aware Silhouette Preservation (Experimental)"), &settings.depthAwareResolve);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("depth_aware_resolve_tooltip"),
				"When the model runs below full resolution, fades its edit across depth edges so background "
				"changes do not bleed into thin foreground geometry. Has no effect at a resolution scale of 1.0."));
		}

		// Frame Hold needs a placement that captures its own input; Before and Separate
		// Upscaling hand their frame straight to DLSS and have nothing to freeze.
		const bool frameHoldSupported = finishedImage || IsPlacement(Placement::kAfterUpscaling);
		ImGui::BeginDisabled(!frameHoldSupported);
		ImGui::Checkbox(T(TKEY("frame_hold"), "Frame Hold"), &compareView.frameHold);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("frame_hold_tooltip"),
				"Freeze the image to compare tuning changes while the game and HUD keep running. Available with Finished Image and After Upscaling. Combine with Split Screen for an on/off comparison. Not saved."));
		}
		ImGui::Separator();
		ImGui::TextUnformatted(T(TKEY("debug"), "Debug"));

		ImGui::Checkbox(T(TKEY("debug_category_view"), "Show Material Categories"), &settings.debugCategoryView);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("debug_category_view_tooltip"),
				"Show material categories by color: red for Skin, orange for Hair, yellow for Eyes, green for Foliage, cyan for Landscape, purple for Equipment, and near-black for Everything Else. Neural Rendering continues running."));
		}

		ImGui::BeginDisabled(!finishedImage);
		ImGui::Checkbox(T(TKEY("raw_model_output"), "Raw Model Output"), &settings.rawModelOutput);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("raw_model_output_tooltip"),
				"Show the enhancement before strengths, guards, and blending are applied. Available with Finished Image. Use temporarily to compare with your tuned result."));
		}

		ImGui::BeginDisabled(!BandsSeparated());
		ImGui::Checkbox(T(TKEY("debug_broad_band"), "Show Broad Band"), &debugState.broadBandView);
		ImGui::Checkbox(T(TKEY("debug_detail_band"), "Show Detail Band"), &debugState.detailBandView);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("debug_band_tooltip"),
				"Show lighting changes in one band: gray means unchanged, black means two stops darker, and white means two stops brighter. Available when Broad and Detail strengths differ. Not saved."));
		}

		ImGui::Checkbox(T(TKEY("debug_guard_clamp"), "Show Guard Clamping"), &debugState.guardClampView);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("debug_guard_clamp_tooltip"),
				"Highlight brightness changes limited by Ratio Guard: red for brightening, blue for darkening. Requires Ratio Guard. Not saved."));
		}

		ImGui::Checkbox(T(TKEY("debug_measure_peak"), "Measure Model Output Peak"), &debugState.measurePeak);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("debug_measure_peak_tooltip"),
				"Measure the brightest value in the enhancement. Values above 1 exceed the normal 0-1 range. Adds a small performance cost. Not saved."));
		}
		if (debugState.measurePeak || debugState.guardClampView) {
			const auto readback = GetDebugReadback();
			if (readback.valid) {
				// Formatted here rather than handed to ImGui as a format string: the text
				// comes from a translation file, which must never reach printf as one.
				ImGui::TextUnformatted(std::format("{} {:.3f} - {} {:.2f}%",
					T(TKEY("debug_peak_label"), "Model peak"), readback.modelPeakLuminance,
					T(TKEY("debug_clamped_label"), "guard clamped"), readback.guardClampedPercent)
						.c_str());
			} else {
				ImGui::TextDisabled("%s", T(TKEY("debug_readback_pending"), "Measuring..."));
			}
		}
	}

	if (!controlsAvailable)
		ImGui::EndDisabled();

	DrawPresetPopups();
}

void NeuralRendering::DrawPresetControls()
{
	if (!userPresetsLoaded)
		RefreshUserPresets();

	const char* builtInNames[] = {
		T(TKEY("preset_full"), "Full"),
		T(TKEY("preset_vanilla_plus"), "Vanilla-Plus")
	};
	const auto builtIn = static_cast<Preset>(std::min<uint>(settings.preset, static_cast<uint>(Preset::kVanillaPlus)));
	const auto builtInIndex = static_cast<std::size_t>(builtIn);
	// A user preset whose file has gone falls back to the built-in label it was based on.
	const auto* userPreset = settings.userPreset.empty() ? nullptr : FindUserPreset(settings.userPreset);
	const bool userActive = userPreset != nullptr;
	const std::string activeName = userActive ? userPreset->name : builtInNames[builtInIndex];
	const bool presetIntact = MatchesPresetValues(userActive ? userPreset->values : GetPreset(builtIn));
	const std::string presetPreview = presetIntact ?
	                                      activeName :
	                                      std::format("{} {}", activeName, T(TKEY("preset_modified"), "(modified)"));
	if (ImGui::BeginCombo(T(TKEY("preset"), "Preset"), presetPreview.c_str())) {
		// Re-read the folder each time the list opens, so a preset file someone shared shows up
		// without a restart.
		if (ImGui::IsWindowAppearing())
			RefreshUserPresets();
		for (std::size_t index = 0; index < kPresetCount; ++index) {
			const bool selected = !userActive && index == builtInIndex;
			if (ImGui::Selectable(builtInNames[index], selected))
				ApplyPreset(static_cast<Preset>(index));
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		if (!userPresets.empty())
			ImGui::Separator();
		for (std::size_t index = 0; index < userPresets.size(); ++index) {
			const auto& preset = userPresets[index];
			const bool selected = userActive && preset.name == settings.userPreset;
			ImGui::PushID(static_cast<int>(index));
			if (ImGui::Selectable(preset.name.c_str(), selected))
				ApplyUserPreset(preset);
			ImGui::PopID();
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("preset_tooltip"),
			"Full applies color, lighting, and detail enhancements without a brightness limit.\nVanilla-Plus preserves original colors and limits brightness changes to one stop in either direction.\nSaved presets appear below the built-in presets. Adjust settings freely; changed presets are marked modified."));
	}

	// The combo may have re-read the folder or switched preset; resolve the active one again.
	userPreset = settings.userPreset.empty() ? nullptr : FindUserPreset(settings.userPreset);
	const PresetValues& activeValues = userPreset ? userPreset->values : GetPreset(builtIn);
	const std::string activeLabel = userPreset ? userPreset->name : builtInNames[builtInIndex];
	const bool modified = !MatchesPresetValues(activeValues);
	if (modified) {
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("preset_reset"), "Reset to preset")))
			ApplyPresetValues(activeValues);
	}

	if (userPreset) {
		if (Util::ButtonWithFlash(T(TKEY("preset_save"), "Save"))) {
			presetEditor.status.clear();
			if (!WriteUserPreset(userPreset->name, CapturePresetValues()))
				presetEditor.status = T(TKEY("preset_write_failed"), "Could not write the preset file. See the log for details.");
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(std::format("{} '{}'",
				T(TKEY("preset_save_tooltip"), "Stores the current values in"), activeLabel)
					.c_str());
		}
		ImGui::SameLine();
	}
	if (Util::ButtonWithFlash(T(TKEY("preset_save_as_new"), "Save As New..."))) {
		presetEditor.status.clear();
		OpenPresetNamePopup(PresetEditor::Action::kSaveAsNew, {}, CapturePresetValues());
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("preset_save_as_new_tooltip"),
			"Save current settings as a new preset in Data/SKSE/Plugins/CommunityShaders/NeuralRendering/Presets/. Copy preset files to share or import them. Mod Organizer 2 saves new files in Overwrite."));
	}
	ImGui::SameLine();
	if (Util::ButtonWithFlash(T(TKEY("preset_copy"), "Copy..."))) {
		presetEditor.status.clear();
		// Suggest "<name> Copy", then "<name> Copy 2", ... until the name is free.
		const std::string base = std::format("{} {}", activeLabel, T(TKEY("preset_copy_suffix"), "Copy"));
		std::string suggestion = base;
		for (int number = 2; PresetNameProblem(suggestion, {}) && number < 100; ++number)
			suggestion = std::format("{} {}", base, number);
		OpenPresetNamePopup(PresetEditor::Action::kCopy, suggestion, activeValues);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(std::format("{} '{}'",
			T(TKEY("preset_copy_tooltip"), "Creates a new preset from the stored values (not your unsaved edits) of"),
			activeLabel)
				.c_str());
	}
	if (userPreset) {
		ImGui::SameLine();
		if (Util::ButtonWithFlash(T(TKEY("preset_rename"), "Rename..."))) {
			presetEditor.status.clear();
			OpenPresetNamePopup(PresetEditor::Action::kRename, userPreset->name, userPreset->values);
			presetEditor.renameFrom = userPreset->name;
		}
		ImGui::SameLine();
		if (Util::ErrorButtonWithFlash(T(TKEY("preset_delete"), "Delete"))) {
			presetEditor.status.clear();
			presetEditor.deleteName = userPreset->name;
			auto& popup = DeletePresetPopup();
			popup.title = std::format("{}###NeuralRenderingDeletePreset", T(TKEY("preset_delete_title"), "Delete Preset"));
			popup.message = std::format("{} '{}'\n\n{}", T(TKEY("preset_delete_confirm"), "Delete the preset"),
				userPreset->name, T(TKEY("preset_delete_confirm_detail"), "Its file is removed. This cannot be undone."));
			popup.confirmLabel = T(TKEY("preset_delete"), "Delete");
			popup.cancelLabel = T(TKEY("preset_cancel"), "Cancel");
			popup.Request();
		}
	}
	if (!presetEditor.status.empty())
		ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Error, "%s", presetEditor.status.c_str());
}

void NeuralRendering::OpenPresetNamePopup(PresetEditor::Action a_action, std::string a_name, const PresetValues& a_source)
{
	presetEditor.action = a_action;
	presetEditor.name = std::move(a_name);
	presetEditor.renameFrom.clear();
	presetEditor.source = a_source;
	presetEditor.error.clear();
	presetEditor.openRequested = true;
}

const char* NeuralRendering::PresetNameProblem(const std::string& a_name, const std::string& a_renameFrom) const
{
	if (a_name.empty())
		return T(TKEY("preset_name_required"), "Enter a name.");
	// Compared in English and in the current language, so neither spelling can shadow one.
	const char* builtInNames[] = { "Full", "Vanilla-Plus", T(TKEY("preset_full"), "Full"),
		T(TKEY("preset_vanilla_plus"), "Vanilla-Plus") };
	for (const char* builtInName : builtInNames) {
		if (Util::IEquals(a_name, builtInName))
			return T(TKEY("preset_name_built_in"), "The built-in presets cannot be replaced. Choose another name.");
	}
	// Windows file names are case-insensitive; renaming a preset to a new case of its own name is fine.
	for (const auto& preset : userPresets) {
		if (Util::IEquals(a_name, preset.name) && !Util::IEquals(preset.name, a_renameFrom))
			return T(TKEY("preset_name_taken"), "A preset with this name already exists.");
	}
	return nullptr;
}

void NeuralRendering::DrawPresetPopups()
{
	using Action = PresetEditor::Action;
	const char* title = presetEditor.action == Action::kRename ? T(TKEY("preset_rename_title"), "Rename Preset") :
	                    presetEditor.action == Action::kCopy   ? T(TKEY("preset_copy_title"), "Copy Preset") :
	                                                             T(TKEY("preset_save_as_new_title"), "Save Preset As");
	// The ### suffix keeps one popup ID whatever the translated title is.
	const std::string popupId = std::format("{}###NeuralRenderingPresetName", title);
	if (presetEditor.openRequested) {
		presetEditor.openRequested = false;
		presetEditor.popupOpen = true;
		ImGui::OpenPopup(popupId.c_str());
	}
	if (auto popup = Util::CenteredPopupModal(popupId.c_str(), &presetEditor.popupOpen)) {
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		const bool submitted = ImGui::InputText(T(TKEY("preset_name"), "Name"), &presetEditor.name,
			ImGuiInputTextFlags_EnterReturnsTrue);
		const std::string name = Util::FileHelpers::SanitizeFileName(presetEditor.name);
		const char* problem = PresetNameProblem(name, presetEditor.renameFrom);
		const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;
		if (problem && !presetEditor.name.empty())
			ImGui::TextColored(palette.Error, "%s", problem);
		else if (!problem)
			ImGui::TextDisabled("%s", std::format("{} {}.json", T(TKEY("preset_file_label"), "File:"), name).c_str());

		ImGui::Separator();
		const char* confirmLabel = presetEditor.action == Action::kRename ? T(TKEY("preset_rename_confirm"), "Rename") :
		                           presetEditor.action == Action::kCopy   ? T(TKEY("preset_copy_confirm"), "Create Copy") :
		                                                                    T(TKEY("preset_save"), "Save");
		ImGui::BeginDisabled(problem != nullptr);
		const bool confirmed = Util::ButtonWithFlash(confirmLabel, ImVec2(ThemeManager::Constants::POPUP_BUTTON_WIDTH, 0)) ||
		                       (submitted && !problem);
		ImGui::EndDisabled();
		if (confirmed) {
			bool succeeded = false;
			if (presetEditor.action == Action::kRename) {
				succeeded = name == presetEditor.renameFrom || RenameUserPreset(presetEditor.renameFrom, name);
			} else if (WriteUserPreset(name, presetEditor.source)) {
				// Select the new preset without replacing unsaved live edits.
				settings.userPreset = name;
				succeeded = true;
			}
			if (succeeded) {
				presetEditor.popupOpen = false;
				ImGui::CloseCurrentPopup();
			} else {
				presetEditor.error = T(TKEY("preset_write_failed"), "Could not write the preset file. See the log for details.");
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("preset_cancel"), "Cancel"), ImVec2(ThemeManager::Constants::POPUP_BUTTON_WIDTH, 0))) {
			presetEditor.popupOpen = false;
			ImGui::CloseCurrentPopup();
		}
		if (!presetEditor.error.empty())
			ImGui::TextColored(palette.Error, "%s", presetEditor.error.c_str());
	}

	if (DeletePresetPopup().Draw() && !DeleteUserPreset(presetEditor.deleteName))
		presetEditor.status = T(TKEY("preset_delete_failed"), "Could not delete the preset file. See the log for details.");
}

void NeuralRendering::DrawCategoryStrengths(const char* a_id, const char* a_label, CategoryStrengths& a_strengths, const char* a_tooltip)
{
	if (!ImGui::TreeNodeEx(a_id, ImGuiTreeNodeFlags_None, "%s", a_label))
		return;
	if (a_tooltip) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(a_tooltip);
	}
	ImGui::SliderFloat(T(TKEY("transfer_strength"), "Transfer Strength"),
		&a_strengths.transferStrength, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("color_strength"), "Color Strength"),
		&a_strengths.colorStrength, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("broad_luminosity"), "Broad Luminosity"),
		&a_strengths.broadLuminosity, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("detail_luminosity"), "Detail Luminosity"),
		&a_strengths.detailLuminosity, 0.0f, 2.0f, "%.2f");
	ImGui::Checkbox(T(TKEY("hue_guard"), "Neutral Colour Guard"), &a_strengths.hueGuard);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("hue_guard_tooltip"),
			"Reduce unwanted tinting of gray and near-gray shading in this category. Enabled for Hair by default."));
	}
	ImGui::TreePop();
}

#undef I18N_KEY_PREFIX

void NeuralRendering::SaveSettings(json& o_json)
{
	o_json = settings;
}

void NeuralRendering::LoadSettings(json& o_json)
{
	// Record key presence before deserialization for settings migration.
	const bool hasPreset = o_json.is_object() && o_json.contains("preset");
	const bool hasShowAdvanced = o_json.is_object() && o_json.contains("showAdvanced");
	const bool hasBandStrengths = o_json.is_object() &&
	                              (o_json.contains("broadLuminosity") || o_json.contains("detailLuminosity"));
	float legacyLuminosity = 1.0f;
	bool hasLegacyLuminosity = false;
	if (o_json.is_object()) {
		if (const auto entry = o_json.find("luminosityStrength");
			entry != o_json.end() && entry->is_number()) {
			legacyLuminosity = entry->get<float>();
			hasLegacyLuminosity = true;
		}
	}
	// The same split per category, keyed by each block's JSON name (kCategoryKeys).
	std::array<std::optional<float>, kMaterialCategoryCount> legacyCategoryLuminosity{};
	if (o_json.is_object()) {
		for (std::size_t index = 0; index < kMaterialCategoryCount; ++index) {
			const auto block = o_json.find(kCategoryKeys[index]);
			if (block == o_json.end() || !block->is_object() || block->contains("broadLuminosity") ||
				block->contains("detailLuminosity"))
				continue;
			if (const auto entry = block->find("luminosityStrength"); entry != block->end() && entry->is_number())
				legacyCategoryLuminosity[index] = entry->get<float>();
		}
	}

	settings = o_json;

	// A category block written before the split carried one luminosityStrength; Broad = Detail
	// = that value is the same edit, exactly as for the global pair below.
	{
		const auto categories = CategorySettings();
		for (std::size_t index = 0; index < kMaterialCategoryCount; ++index) {
			if (legacyCategoryLuminosity[index]) {
				categories[index]->broadLuminosity = *legacyCategoryLuminosity[index];
				categories[index]->detailLuminosity = *legacyCategoryLuminosity[index];
			}
		}
	}

	// Preserve legacy luminosity by assigning it to both bands; omit the old key on save.
	if (hasLegacyLuminosity && !hasBandStrengths) {
		settings.broadLuminosity = legacyLuminosity;
		settings.detailLuminosity = legacyLuminosity;
	}

	// A config from before presets existed keeps every value it stored; it is labelled Full and,
	// wherever it differs, "Full (modified)". Nobody's look changes silently on upgrade.
	if (!hasPreset)
		settings.preset = static_cast<uint>(Preset::kFull);

	{
		auto values = CapturePresetValues();
		SanitizePresetValues(values);
		ApplyPresetValues(values);
	}
	if (settings.resolutionMode > 1)
		settings.resolutionMode = 1;
	// Scales above native (model supersampling) are no longer offered; a saved one runs at native.
	SanitizeFloat(settings.resolutionScale, 1.0f, 0.25f, 1.0f);
	SanitizeFloat(settings.resolutionScaleX, 1.0f, 0.25f, 1.0f);
	SanitizeFloat(settings.resolutionScaleY, 1.0f, 0.25f, 1.0f);
	if (settings.preset >= static_cast<uint>(Preset::kCount))
		settings.preset = static_cast<uint>(Preset::kFull);

	// A user preset whose file is gone is forgotten; its values stay and read as the built-in
	// preset they were based on, "(modified)" wherever they differ.
	RefreshUserPresets();
	if (!settings.userPreset.empty() && !FindUserPreset(settings.userPreset)) {
		logger::info("[NeuralRendering] Preset '{}' no longer exists; keeping its values", settings.userPreset);
		settings.userPreset.clear();
	}

	// Keep Advanced visible for migrated settings that differ from Full.
	if (!hasShowAdvanced)
		settings.showAdvanced = !MatchesPreset(static_cast<Preset>(settings.preset));
}

void NeuralRendering::RestoreDefaultSettings()
{
	settings = {};
	ApplyPreset(Preset::kFull);
	settings.showAdvanced = false;
}

void NeuralRendering::MigrateLegacyUpscalingSettings(json& a_root)
{
	const std::string name = globals::features::neuralRendering.GetName();
	if (!a_root.is_object() || a_root.contains(name))
		return;
	auto upscaling = a_root.find(globals::features::upscaling.GetName());
	if (upscaling == a_root.end() || !upscaling->is_object())
		return;

	constexpr std::string_view kLegacyPrefix = "neuralRendering";
	json migrated = json::object();
	for (const auto& [key, value] : upscaling->items()) {
		if (key.size() <= kLegacyPrefix.size() || !key.starts_with(kLegacyPrefix))
			continue;
		std::string newKey = key.substr(kLegacyPrefix.size());
		newKey[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(newKey[0])));
		migrated[newKey] = value;
	}
	if (migrated.empty())
		return;

	logger::info("[NeuralRendering] Migrated {} legacy settings from the Upscaling section", migrated.size());
	a_root[name] = std::move(migrated);
}
// Lifecycle and hooks

void NeuralRendering::DataLoaded()
{
	// Vanilla keyword on every playable/NPC race; creatures lack it.
	if (auto form = RE::TESForm::LookupByEditorID("ActorTypeNPC"))
		actorTypeNPCKeyword = form->As<RE::BGSKeyword>();
	if (!actorTypeNPCKeyword)
		logger::warn("[NeuralRendering] ActorTypeNPC keyword not found; the Equipment category will be empty");
}

void NeuralRendering::PostPostLoad()
{
	// Chained on the call site Upscaling hooks for its own pass (installed first, because
	// Upscaling precedes Neural Rendering in the feature list), so this brackets it.
	stl::write_thunk_call<Main_PostProcessing>(REL::RelocationID(100430, 107148).address() + REL::Relocate(0x1F0, 0x1E7));

	// Flags geometry belonging to humanoid actors and hair for the material categories
	// (see SetupGeometryCategory).
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);

	// Lets forward (post-deferred) lighting draws - sorted alpha geometry such as
	// hair, and the first-person view - write their Neural Rendering category
	// (see RestoreCategories).
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));

	if (!MenuOpenCloseEventHandler::Register())
		logger::warn("[NeuralRendering] MenuOpenCloseEventHandler registration failed; temporal history may survive loading transitions");

	logger::info("[NeuralRendering] Installed hooks");
}

bool NeuralRendering::IsDLSSActive() const
{
	const auto& upscaling = globals::features::upscaling;
	return upscaling.loaded && upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS;
}

void NeuralRendering::RequestHistoryReset()
{
	pendingReset.store(true, std::memory_order_release);
	globals::features::upscaling.pendingDLSSReset.store(true, std::memory_order_release);
}

void NeuralRendering::RequestComparisonCapture()
{
	comparePending.store(true, std::memory_order_release);
}

void NeuralRendering::DestroyFrameResources()
{
	DestroyModelResources();
	resourcesActive = false;
	activePlacement = UINT_MAX;

	ReleaseTexture(outputTexture);
	ReleaseTexture(finishedImageTexture);
	ReleaseTexture(finishedImageDepthSnapshot);
	finishedImageGuidesReady = false;
	ReleaseTexture(materialCategoriesSnapshot);
	ReleaseTexture(heldColor);
	ReleaseTexture(heldDepth);
	ReleaseTexture(heldCategories);
	heldPlacement = UINT_MAX;
}

void NeuralRendering::BeginFrame()
{
	resetThisFrame = pendingReset.exchange(false, std::memory_order_acq_rel);

	// Leaving DLSS releases everything: NR only ever runs with it, and the next DLSS
	// session may come back at a different render resolution.
	const bool dlssActive = IsDLSSActive();
	if (dlssWasActive && !dlssActive)
		DestroyFrameResources();
	dlssWasActive = dlssActive;

	if (settings.enabled && activePlacement != settings.placement) {
		if (activePlacement != UINT_MAX && resourcesActive) {
			DestroyModelResources();
			resourcesActive = false;
		}
		activePlacement = settings.placement;
	}
	if (!settings.enabled && resourcesActive) {
		DestroyModelResources();
		resourcesActive = false;
		activePlacement = UINT_MAX;
	}

	// A held frame only means something while the placement that captured it is still running.
	const bool frameHoldPlacement = IsPlacement(Placement::kFinishedImage) || IsPlacement(Placement::kAfterUpscaling);
	if (heldColor && (!compareView.frameHold || !settings.enabled || !frameHoldPlacement ||
						 heldPlacement != settings.placement))
		ReleaseFrameHold();
}

Texture2D* NeuralRendering::EnsureOutputTexture()
{
	if (outputTexture)
		return outputTexture;

	// Always a full-size, non-aliasing texture matching kMAIN.
	auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!main.texture || !main.SRV || !main.UAV)
		return nullptr;

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	outputTexture = new Texture2D(texDesc, "NeuralRendering::Output");
	outputTexture->CreateSRV(srvDesc);
	outputTexture->CreateUAV(uavDesc);
	return outputTexture;
}

ID3D11Resource* NeuralRendering::PrepareUpscaleInput(ID3D11Resource* a_color, ID3D11Resource* a_superResolutionMotionVectors)
{
	if (!loaded || !settings.enabled ||
		!(IsPlacement(Placement::kBeforeUpscaling) || IsPlacement(Placement::kSeparateUpscaling)) ||
		!IsDLSSActive() || !IsAvailable())
		return a_color;

	auto* output = EnsureOutputTexture();
	if (!output)
		return a_color;

	auto renderer = globals::game::renderer;
	const auto& upscaling = globals::features::upscaling;
	auto renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });
	const uint32_t renderWidth = static_cast<uint32_t>(renderSize.x);
	const uint32_t renderHeight = static_cast<uint32_t>(renderSize.y);

	resourcesActive = true;
	Options options = MakeOptions();
	// Before the upscaler colour and guides are both at render resolution,
	// and the colour is the jittered raster DLSS is about to de-jitter. Hand
	// the model the same offset Streamline gets so it can see a stable framing.
	options.guideWidth = renderWidth;
	options.guideHeight = renderHeight;
	options.jitterOffsetX = -upscaling.jitter.x;
	options.jitterOffsetY = -upscaling.jitter.y;
	// Pre-tonemap: show the model the frame exposed and graded as it will be displayed.
	options.display = MakeDisplayTransform();
	options.colorDomain = SceneColorDomain(options.proxyCurve);
	const auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	// The pre-blended-decals snapshot, not the live Masks2 - see CaptureCategories.
	auto* materialCategoriesSRV = materialCategoriesSnapshot ? materialCategoriesSnapshot->srv.get() : nullptr;
	// Use raw motion vectors: DLSS dilation assigns foreground motion to background pixels and
	// destabilizes NR edges.
	ID3D11Resource* upscaleInput = a_color;
	// A perf event, not a profiler pass: this runs inside Upscaling's own "Upscaling::Upscale"
	// profiler pass, and profiler passes do not nest.
	globals::state->BeginPerfEvent("NeuralRendering::Generate");
	if (IsPlacement(Placement::kBeforeUpscaling)) {
		if (Evaluate(a_color,
				output->resource.get(),
				depth.texture,
				depth.depthSRV,
				materialCategoriesSRV,
				motionVector.texture,
				motionVector.SRV,
				renderWidth,
				renderHeight,
				options)) {
			upscaleInput = output->resource.get();
		}
	} else {
		const uint32_t nativeWidth = static_cast<uint32_t>(globals::game::graphicsState->screenWidth);
		const uint32_t nativeHeight = static_cast<uint32_t>(globals::game::graphicsState->screenHeight);
		PrepareSeparateUpscaling(a_color,
			output->resource.get(),
			depth.texture,
			depth.depthSRV,
			materialCategoriesSRV,
			motionVector.texture,
			motionVector.SRV,
			a_superResolutionMotionVectors,
			renderWidth,
			renderHeight,
			nativeWidth,
			nativeHeight,
			options);
	}
	globals::state->EndPerfEvent();
	return upscaleInput;
}

void NeuralRendering::ResolveUpscaledFrame(Texture2D* a_upscaled)
{
	if (!loaded)
		return;

	if (a_upscaled && settings.enabled && IsDLSSActive() &&
		(IsPlacement(Placement::kAfterUpscaling) || IsPlacement(Placement::kSeparateUpscaling))) {
		auto* output = EnsureOutputTexture();
		bool resultValid = false;
		const uint32_t nativeWidth = static_cast<uint32_t>(globals::game::graphicsState->screenWidth);
		const uint32_t nativeHeight = static_cast<uint32_t>(globals::game::graphicsState->screenHeight);

		if (output && IsPlacement(Placement::kAfterUpscaling) && IsAvailable()) {
			resourcesActive = true;
			auto renderer = globals::game::renderer;
			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			// The category snapshot (opaque categories captured before decals, forward categories added after), not the live Masks2 - see CaptureCategories.
			auto* materialCategoriesSRV = materialCategoriesSnapshot ? materialCategoriesSnapshot->srv.get() : nullptr;
			auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
			// After the upscaler the colour input is display resolution, but depth and
			// motion vectors are still the game's render-resolution targets.
			const auto guideSize = Util::ConvertToDynamic(float2{ (float)nativeWidth, (float)nativeHeight });
			Options options = MakeOptions();
			options.guideWidth = static_cast<uint32_t>(guideSize.x);
			options.guideHeight = static_cast<uint32_t>(guideSize.y);
			// Guides retain render jitter after DLSS resolves color. Offset guide lookups to align category and
			// depth boundaries.
			const auto& jitter = globals::features::upscaling.jitter;
			options.guideJitterOffsetX = -jitter.x;
			options.guideJitterOffsetY = -jitter.y;
			// Pre-tonemap: show the model the frame exposed and graded as it will be displayed.
			options.display = MakeDisplayTransform();
			options.colorDomain = SceneColorDomain(options.proxyCurve);

			// Hold color and guides together; release the hold if their dimensions change.
			ID3D11Texture2D* colorIn = a_upscaled->resource.get();
			ID3D11Texture2D* depthTexture = depth.texture;
			ID3D11ShaderResourceView* depthSRV = depth.depthSRV;
			if (compareView.frameHold) {
				const bool heldMatches = heldColor && heldColor->desc.Width == a_upscaled->desc.Width &&
				                         heldColor->desc.Height == a_upscaled->desc.Height &&
				                         heldColor->desc.Format == a_upscaled->desc.Format;
				if (heldColor && !heldMatches)
					ReleaseFrameHold();
				if (!heldColor && CaptureFrameHold(colorIn, depth.texture, depth.depthSRV)) {
					heldGuideWidth = options.guideWidth;
					heldGuideHeight = options.guideHeight;
					heldGuideJitterX = options.guideJitterOffsetX;
					heldGuideJitterY = options.guideJitterOffsetY;
					options.reset = true;
				}
				if (heldColor) {
					colorIn = heldColor->resource.get();
					depthTexture = heldDepth->resource.get();
					depthSRV = heldDepth->srv.get();
					materialCategoriesSRV = heldCategories->srv.get();
					options.guideWidth = heldGuideWidth;
					options.guideHeight = heldGuideHeight;
					options.guideJitterOffsetX = heldGuideJitterX;
					options.guideJitterOffsetY = heldGuideJitterY;
					options.staticMotion = true;
				}
			}

			// Raw game motion-vector target, not the dilated ghosting-reduction copy
			// DLSS consumes; see the matching note in PrepareUpscaleInput().
			globals::profiler->BeginPass("NeuralRendering::Generate");
			resultValid = Evaluate(colorIn,
				output->resource.get(),
				depthTexture,
				depthSRV,
				materialCategoriesSRV,
				motionVector.texture,
				motionVector.SRV,
				nativeWidth,
				nativeHeight,
				options);
			globals::profiler->EndPass();
		} else if (output && IsPlacement(Placement::kSeparateUpscaling)) {
			globals::profiler->BeginPass("NeuralRendering::Generate");
			resultValid = ResolveSeparateUpscaling(a_upscaled->resource.get(), output->resource.get(), nativeWidth, nativeHeight);
			globals::profiler->EndPass();
		}

		// Hand the result back through Upscaling's own sharpener input, so its sharpening and
		// resolve stay unaware of Neural Rendering (one display-resolution copy).
		if (resultValid)
			globals::d3d::context->CopyResource(a_upscaled->resource.get(), output->resource.get());
	}

	// Finished Image evaluates later, at the tonemap, so it snapshots depth now
	// for the same reason After Upscaling evaluates before the expansion that follows.
	CaptureFinishedImageGuides();
}

void NeuralRendering::SetupGeometryCategory(RE::BSRenderPass* a_pass)
{
	auto deferred = globals::deferred;
	auto state = globals::state;
	constexpr auto humanoidFlag = static_cast<uint32_t>(State::ExtraShaderDescriptors::IsHumanoidActor);
	constexpr auto hairFlag = static_cast<uint32_t>(State::ExtraShaderDescriptors::IsHair);

	// Set category flags for both deferred and forward lighting draws.
	const bool writesCategories = deferred->deferredPass || forwardCaptureActive;

	bool isHumanoidActor = false;
	bool isHair = false;
	if (writesCategories && settings.enabled && actorTypeNPCKeyword && a_pass->geometry) {
		// Hair by shader authoring (wigs) or by head part (hairlines, braids and
		// strands authored with other shader types); see Lighting.hlsl.
		isHair = IsHairTintShader(a_pass);
		if (auto userData = a_pass->geometry->GetUserData()) {
			if (auto actor = userData->As<RE::Actor>()) {
				// Humanoid equipment excludes skin, hair, and eyes, which the shader classifies first.
				if (auto race = actor->GetRace())
					isHumanoidActor = race->HasKeyword(actorTypeNPCKeyword);
				isHair = isHair || IsHairHeadPartGeometry(actor, a_pass->geometry);
			}
		}
	}

	auto& descriptor = state->permutationData.ExtraShaderDescriptor;
	descriptor &= ~(humanoidFlag | hairFlag);
	if (isHumanoidActor)
		descriptor |= humanoidFlag;
	if (isHair)
		descriptor |= hairFlag;
}

void NeuralRendering::CaptureCategories()
{
	// A new frame's opaque categories supersede any forward capture still armed
	// from a frame that never reached Main_PostProcessing.
	forwardCaptureActive = false;

	// Only paid for when Neural Rendering can actually consume it: DLSS-only.
	// The decode shader always samples the category texture now, since each
	// category's hue guard toggle needs to know which material a pixel is.
	if (!settings.enabled)
		return;
	if (!IsDLSSActive())
		return;

	auto renderer = globals::game::renderer;
	auto& masks2 = renderer->GetRuntimeData().renderTargets[MASKS2];
	if (!masks2.texture)
		return;

	// Allocate after opaque rendering: the repurposed Masks2 target may not exist during native target
	// creation.
	if (!materialCategoriesSnapshot) {
		D3D11_TEXTURE2D_DESC texDesc{};
		masks2.texture->GetDesc(&texDesc);
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		materialCategoriesSnapshot = new Texture2D(texDesc, "NeuralRendering::MaterialCategoriesSnapshot");
		materialCategoriesSnapshot->CreateSRV(srvDesc);
	}

	globals::profiler->BeginPass("NeuralRendering::CategoryCapture");
	globals::d3d::context->CopyResource(materialCategoriesSnapshot->resource.get(), masks2.texture);
	globals::profiler->EndPass();
}

void NeuralRendering::RestoreCategories()
{
	forwardCaptureActive = false;
	if (!materialCategoriesSnapshot || !settings.enabled || !IsDLSSActive())
		return;

	auto& masks2 = globals::game::renderer->GetRuntimeData().renderTargets[MASKS2];
	if (!masks2.texture)
		return;

	// Masks2 is unbound here (EndDeferred cleared the OM before DeferredPasses),
	// and the composite has already read the decal-blended AO it held.
	globals::profiler->BeginPass("NeuralRendering::CategoryCapture");
	globals::d3d::context->CopyResource(masks2.texture, materialCategoriesSnapshot->resource.get());
	globals::profiler->EndPass();
	forwardCaptureActive = true;
}

void NeuralRendering::FinishCategoryCapture()
{
	if (!forwardCaptureActive)
		return;
	forwardCaptureActive = false;

	auto& masks2 = globals::game::renderer->GetRuntimeData().renderTargets[MASKS2];
	if (!materialCategoriesSnapshot || !masks2.texture)
		return;

	globals::profiler->BeginPass("NeuralRendering::CategoryCapture");
	globals::d3d::context->CopyResource(materialCategoriesSnapshot->resource.get(), masks2.texture);
	globals::profiler->EndPass();
}

void NeuralRendering::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::neuralRendering.SetupGeometryCategory(Pass);
	func(This, Pass, RenderFlags);
}

void NeuralRendering::BSBatchRenderer_RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	auto& neuralRendering = globals::features::neuralRendering;
	auto* deferred = globals::deferred;
	auto* state = globals::state;
	auto& runtimeData = globals::game::shadowState->GetRuntimeData();

	// Capture only main-view lighting draws. Reflection and cubemap targets may be incompatible with
	// Masks2.
	const bool bindCategories = neuralRendering.forwardCaptureActive &&
	                            !deferred->deferredPass && state->inWorld &&
	                            a_pass && a_pass->shader &&
	                            a_pass->shader->shaderType.get() == RE::BSShader::Type::Lighting &&
	                            !(state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)) &&
	                            runtimeData.renderTargets[0] == deferred->forwardRenderTargets[0];

	if (!bindCategories) {
		func(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	runtimeData.renderTargets[7] = MASKS2;
	runtimeData.setRenderTargetMode[7] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
	runtimeData.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	func(a_pass, a_technique, a_alphaTest, a_renderFlags);

	runtimeData.renderTargets[7] = RE::RENDER_TARGET::kNONE;
	runtimeData.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}
// Options and display transform

NeuralRendering::Options NeuralRendering::MakeOptions() const
{
	const auto& upscalingSettings = globals::features::upscaling.settings;
	Options options{};
	options.style = settings.style;
	options.intensity = settings.intensity;
	options.colorStrength = settings.colorStrength;
	options.transferStrength = settings.transferStrength;
	options.broadLuminosity = settings.broadLuminosity;
	options.detailLuminosity = settings.detailLuminosity;
	options.bandRadius = settings.bandRadius;
	options.maxRatio = settings.maxRatio;
	options.ratioGuardEnabled = settings.ratioGuardEnabled;
	const auto setCategoryStrengths = [&](MaterialCategory category, const CategoryStrengths& strengths) {
		options.categoryStrengths[static_cast<std::size_t>(category)] = strengths;
	};
	setCategoryStrengths(MaterialCategory::kEverythingElse, settings.everythingElseStrengths);
	setCategoryStrengths(MaterialCategory::kSkin, settings.skinStrengths);
	setCategoryStrengths(MaterialCategory::kHair, settings.hairStrengths);
	setCategoryStrengths(MaterialCategory::kEyes, settings.eyesStrengths);
	setCategoryStrengths(MaterialCategory::kFoliage, settings.foliageStrengths);
	setCategoryStrengths(MaterialCategory::kLandscape, settings.landscapeStrengths);
	setCategoryStrengths(MaterialCategory::kEquipment, settings.equipmentStrengths);
	options.depthAwareResolve = settings.depthAwareResolve;
	options.alternateFrames = settings.alternateFrames;
	options.localToneStrength = settings.localToneStrength;
	options.localStructureStrength = settings.localStructureStrength;
	options.skinStructureStrength = settings.skinStructureStrength;
	options.automaticMask = settings.automaticMask;
	options.debugCategoryView = settings.debugCategoryView;
	options.rawModelOutput = settings.rawModelOutput;
	options.proxyCurve = ResolveProxyCurve();
	// The band views are only meaningful once the two strengths actually separate the bands,
	// and they are mutually exclusive; Broad wins if both are ticked.
	const bool bandsSeparated = BandsSeparated();
	options.debugBroadBand = bandsSeparated && debugState.broadBandView;
	options.debugDetailBand = bandsSeparated && debugState.detailBandView && !options.debugBroadBand;
	options.debugGuardClamp = debugState.guardClampView;
	options.measureModelPeak = debugState.measurePeak;
	options.wipePosition = compareView.wipe ? std::clamp(compareView.wipePosition, 0.0f, 1.0f) : -1.0f;
	options.reset = resetThisFrame;
	const bool perAxis = settings.resolutionMode == 1;
	options.resolutionScaleX = perAxis ? settings.resolutionScaleX : settings.resolutionScale;
	options.resolutionScaleY = perAxis ? settings.resolutionScaleY : settings.resolutionScale;
	options.superResolutionQualityMode = upscalingSettings.qualityMode;
	options.superResolutionPreset = upscalingSettings.presetDLSS;
	return options;
}

void NeuralRendering::CaptureDisplayTransform(RE::ImageSpaceShaderParam* a_param)
{
	auto& capture = displayCapture;
	capture.valid = false;
	capture.adaptationSRV = nullptr;
	if (!settings.enabled || !a_param || !globals::d3d::context ||
		globals::state->GetTonemapOwner() != State::TonemapOwner::kVanilla)
		return;

	// ISHDR.hlsl's PerGeometry constants, as float4 slots of the pass's pixel constant group:
	// Flags c0, TimingData c1, Param c2, Cinematic c3, Tint c4 (Fade and the blur data follow).
	constexpr std::uint32_t kParamOffset = 8;
	constexpr std::uint32_t kCinematicOffset = 12;
	constexpr std::uint32_t kTintOffset = 16;
	constexpr std::uint32_t kRequiredFloats = kTintOffset + 4;
	if (!a_param->pixelConstantGroup || a_param->pixelConstantGroupSize < kRequiredFloats)
		return;
	std::copy_n(a_param->pixelConstantGroup + kParamOffset, 4, capture.param);
	std::copy_n(a_param->pixelConstantGroup + kCinematicOffset, 4, capture.cinematic);
	std::copy_n(a_param->pixelConstantGroup + kTintOffset, 4, capture.tint);

	// Values outside what an imagespace can express mean the layout above is not what this
	// pass carries; fall back to the plain proxy rather than grade the model's view with noise.
	const auto within = [](float value, float low, float high) {
		return std::isfinite(value) && value >= low && value <= high;
	};
	const bool plausible = within(capture.param[1], 0.0f, 1000.0f) &&
	                       within(capture.cinematic[0], 0.0f, 4.0f) &&
	                       within(capture.cinematic[2], 0.1f, 4.0f) &&
	                       within(capture.cinematic[3], 0.1f, 4.0f) &&
	                       within(capture.tint[0], 0.0f, 4.0f) && within(capture.tint[1], 0.0f, 4.0f) &&
	                       within(capture.tint[2], 0.0f, 4.0f) && within(capture.tint[3], 0.0f, 1.0f);
	if (!plausible) {
		if (!displayCaptureLogged) {
			displayCaptureLogged = true;
			logger::warn(
				"[NeuralRendering] Display transform: implausible ISHDR constants "
				"(white {:.3f}, saturation {:.3f}, contrast {:.3f}, brightness {:.3f}, tint amount {:.3f}); "
				"using the plain proxy",
				capture.param[1], capture.cinematic[0], capture.cinematic[2], capture.cinematic[3], capture.tint[3]);
		}
		return;
	}

	// The vanilla pass samples its adaptation texture (AvgTex) at pixel-shader slot 2 and leaves
	// it bound. It is a tiny, uniform target; anything larger is not the adaptation.
	winrt::com_ptr<ID3D11ShaderResourceView> adaptationSRV;
	globals::d3d::context->PSGetShaderResources(2, 1, adaptationSRV.put());
	if (!adaptationSRV)
		return;
	winrt::com_ptr<ID3D11Resource> resource;
	adaptationSRV->GetResource(resource.put());
	const auto texture = resource ? resource.try_as<ID3D11Texture2D>() : nullptr;
	if (!texture)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	constexpr UINT kMaxAdaptationExtent = 64;
	if (!desc.Width || !desc.Height || desc.Width > kMaxAdaptationExtent || desc.Height > kMaxAdaptationExtent)
		return;

	capture.adaptationSRV = adaptationSRV;
	capture.valid = true;
	if (!displayCaptureLogged) {
		displayCaptureLogged = true;
		logger::info(
			"[NeuralRendering] Display transform captured: adaptation {}x{} format {}, "
			"white {:.3f} filmic {:.0f}, saturation {:.3f} contrast {:.3f} brightness {:.3f}, "
			"tint ({:.3f}, {:.3f}, {:.3f}) x {:.3f}",
			desc.Width, desc.Height, static_cast<int>(desc.Format),
			capture.param[1], capture.param[2], capture.cinematic[0], capture.cinematic[2], capture.cinematic[3],
			capture.tint[0], capture.tint[1], capture.tint[2], capture.tint[3]);
	}
}

NeuralRendering::DisplayTransform NeuralRendering::MakeDisplayTransform() const
{
	DisplayTransform display{};

	const auto& capture = displayCapture;
	if (capture.valid && capture.adaptationSRV) {
		display.vanillaGrading = true;
		display.vanillaAdaptationSRV = capture.adaptationSRV.get();
		std::copy_n(capture.param, 4, display.param);
		std::copy_n(capture.cinematic, 4, display.cinematic);
		std::copy_n(capture.tint, 4, display.tint);
	}

	// Post Processing's Composite applies its auto exposure downstream of every pre-tonemap
	// placement whether or not it owns the tonemap; mirror its formula (composite.ps.hlsl).
	auto& postProcessing = globals::features::postProcessing;
	if (postProcessing.loaded && !postProcessing.bypass && !postProcessing.IsTonemapOwnedByEffects11()) {
		auto* autoExposure = postProcessing.GetPipelineFeature<HistogramAutoExposure>(PostProcessing::FeaturePipelineIndex::AutoExposure);
		if (autoExposure && autoExposure->enabled && autoExposure->GetAdaptationSRV()) {
			display.postProcessExposure = true;
			display.postProcessAdaptationSRV = autoExposure->GetAdaptationSRV();
			display.postProcessExposureScale = 0.18f * std::exp2(autoExposure->settings.ExposureCompensation);
			display.postProcessAdaptationRange[0] = std::exp2(autoExposure->settings.AdaptationRange.x - 3.0f);
			display.postProcessAdaptationRange[1] = std::exp2(autoExposure->settings.AdaptationRange.y - 3.0f);
		}
	}
	return display;
}
// Finished Image

bool NeuralRendering::EvaluateFinishedImage(ID3D11Texture2D* a_colorIn, ID3D11ShaderResourceView* a_colorInSRV,
	ID3D11Texture2D* a_colorOut)
{
	if (!settings.enabled || !IsPlacement(Placement::kFinishedImage))
		return false;

	// Log resource failures once Finished Image is active.
	if (!IsDLSSActive()) {
		logger::debug("[NeuralRendering] Finished Image skipped: upscale method is not DLSS");
		return false;
	}
	if (!IsAvailable()) {
		logger::debug("[NeuralRendering] Finished Image skipped: backend unavailable");
		return false;
	}
	if (!a_colorIn || !a_colorInSRV || !a_colorOut) {
		logger::debug("[NeuralRendering] Finished Image skipped: missing colour input or output texture");
		return false;
	}

	auto renderer = globals::game::renderer;
	// Consume guides once per upscaled frame; repeated evaluations on different targets would reset
	// history.
	if (!finishedImageGuidesReady || !finishedImageDepthSnapshot) {
		logger::debug("[NeuralRendering] Finished Image skipped: no guides captured for this frame");
		return false;
	}
	finishedImageGuidesReady = false;

	auto* depthTexture = finishedImageDepthSnapshot->resource.get();
	auto* depthSRV = finishedImageDepthSnapshot->srv.get();
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	if (!depthTexture || !depthSRV || !motionVector.texture || !motionVector.SRV) {
		logger::debug(
			"[NeuralRendering] Finished Image skipped: depth or motion-vector guide missing "
			"(depthSnapshot={} depthSnapshotSRV={} motionVector.texture={} motionVector.SRV={})",
			(void*)depthTexture, (void*)depthSRV, (void*)motionVector.texture, (void*)motionVector.SRV);
		return false;
	}

	// Use active screen dimensions, not potentially padded texture allocations. Guide extents were
	// captured separately.
	const uint32_t nativeWidth = static_cast<uint32_t>(globals::game::graphicsState->screenWidth);
	const uint32_t nativeHeight = static_cast<uint32_t>(globals::game::graphicsState->screenHeight);
	if (!nativeWidth || !nativeHeight) {
		logger::debug("[NeuralRendering] Finished Image skipped: zero screen size ({}x{})", nativeWidth, nativeHeight);
		return false;
	}

	// The category snapshot (opaque categories captured before decals, forward categories added after), not the live Masks2 - see CaptureCategories.
	auto* materialCategoriesSRV = materialCategoriesSnapshot ? materialCategoriesSnapshot->srv.get() : nullptr;

	// Color is resolved at display resolution; depth, motion, and categories remain jittered at render
	// resolution.
	const auto& jitter = globals::features::upscaling.jitter;
	Options options = MakeOptions();
	options.guideWidth = finishedImageGuideWidth;
	options.guideHeight = finishedImageGuideHeight;
	options.guideJitterOffsetX = -jitter.x;
	options.guideJitterOffsetY = -jitter.y;

	// Match HDROutputCS: the HDR redirect may hold linear color when Linear Lighting or Post Processing
	// owns the output.
	const auto& hdrDisplay = globals::features::hdrDisplay;
	const bool sceneLinear = hdrDisplay.loaded && hdrDisplay.framebufferRedirected &&
	                         (IsLinearLightingActive() ||
								 globals::state->GetTonemapOwner() == State::TonemapOwner::kPostProcessing);
	options.colorDomain = sceneLinear ? ColorDomain::kSceneLinear : ColorDomain::kDisplayGamma;
	// Finished Image uses an identity display transform regardless of the stored proxy curve.
	options.proxyCurve = ProxyCurve::kDisplayMatched;

	// Frame Hold: evaluate one captured frame every frame instead of the live one. The live
	// guides were still consumed above, so the one-evaluation-per-frame contract holds.
	ID3D11Resource* colorIn = a_colorIn;
	if (compareView.frameHold) {
		D3D11_TEXTURE2D_DESC liveDesc{};
		a_colorIn->GetDesc(&liveDesc);
		const bool heldMatches = heldColor && heldColor->desc.Width == liveDesc.Width &&
		                         heldColor->desc.Height == liveDesc.Height && heldColor->desc.Format == liveDesc.Format;
		if (heldColor && !heldMatches)
			ReleaseFrameHold();
		if (!heldColor && CaptureFrameHold(a_colorIn, finishedImageDepthSnapshot->resource.get(),
							  finishedImageDepthSnapshot->srv.get())) {
			heldGuideWidth = finishedImageGuideWidth;
			heldGuideHeight = finishedImageGuideHeight;
			heldGuideJitterX = options.guideJitterOffsetX;
			heldGuideJitterY = options.guideJitterOffsetY;
			options.reset = true;
		}
		if (heldColor) {
			colorIn = heldColor->resource.get();
			depthTexture = heldDepth->resource.get();
			depthSRV = heldDepth->srv.get();
			materialCategoriesSRV = heldCategories->srv.get();
			options.guideWidth = heldGuideWidth;
			options.guideHeight = heldGuideHeight;
			options.guideJitterOffsetX = heldGuideJitterX;
			options.guideJitterOffsetY = heldGuideJitterY;
			options.staticMotion = true;
		}
	}
	// HDR gamma output uses 1.0 at paper white. Supply the display peak for highlight rolloff.
	if (!sceneLinear && hdrDisplay.loaded && hdrDisplay.framebufferRedirected && hdrDisplay.settings.hdrPaperWhite > 0)
		options.highlightWhite = static_cast<float>(hdrDisplay.settings.hdrPeakNits) / static_cast<float>(hdrDisplay.settings.hdrPaperWhite);

	globals::profiler->BeginPass("NeuralRendering::Generate");
	const bool evaluated = Evaluate(colorIn, a_colorOut,
		depthTexture, depthSRV, materialCategoriesSRV, motionVector.texture, motionVector.SRV,
		nativeWidth, nativeHeight, options);
	globals::profiler->EndPass();
	if (!evaluated) {
		logger::debug(
			"[NeuralRendering] Finished Image skipped: Evaluate() returned false "
			"(see preceding [NeuralRendering] log lines for the reason)");
		return false;
	}

	resourcesActive = true;
	return true;
}

bool NeuralRendering::CaptureFrameHold(ID3D11Texture2D* a_colorIn, ID3D11Texture2D* a_depth,
	ID3D11ShaderResourceView* a_depthSRV)
{
	if (!a_colorIn || !a_depth || !a_depthSRV || !materialCategoriesSnapshot || !materialCategoriesSnapshot->srv)
		return false;

	// Mirror source descriptions, including depth bind flags, to keep CopyResource valid.
	const auto makeCopy = [](const D3D11_TEXTURE2D_DESC& a_desc, ID3D11ShaderResourceView* a_sourceSRV,
							  const char* a_name) -> Texture2D* {
		try {
			auto* copy = new Texture2D(a_desc, a_name);
			if (a_sourceSRV) {
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				a_sourceSRV->GetDesc(&srvDesc);
				copy->CreateSRV(srvDesc);
			}
			return copy;
		} catch (const std::exception& e) {
			logger::warn("[NeuralRendering] Frame Hold: {} creation failed ({})", a_name, e.what());
			return nullptr;
		}
	};

	D3D11_TEXTURE2D_DESC colorDesc{};
	a_colorIn->GetDesc(&colorDesc);
	colorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	colorDesc.Usage = D3D11_USAGE_DEFAULT;
	colorDesc.CPUAccessFlags = 0;
	colorDesc.MiscFlags = 0;

	D3D11_TEXTURE2D_DESC depthDesc{};
	a_depth->GetDesc(&depthDesc);

	heldColor = makeCopy(colorDesc, nullptr, "NeuralRendering::HeldColor");
	heldDepth = makeCopy(depthDesc, a_depthSRV, "NeuralRendering::HeldDepth");
	heldCategories = makeCopy(materialCategoriesSnapshot->desc, materialCategoriesSnapshot->srv.get(), "NeuralRendering::HeldCategories");
	if (!heldColor || !heldDepth || !heldCategories) {
		ReleaseTexture(heldColor);
		ReleaseTexture(heldDepth);
		ReleaseTexture(heldCategories);
		return false;
	}

	auto* context = globals::d3d::context;
	context->CopyResource(heldColor->resource.get(), a_colorIn);
	context->CopyResource(heldDepth->resource.get(), a_depth);
	context->CopyResource(heldCategories->resource.get(), materialCategoriesSnapshot->resource.get());
	heldPlacement = settings.placement;
	return true;
}

void NeuralRendering::ReleaseFrameHold()
{
	const bool wasHeld = heldColor != nullptr;
	ReleaseTexture(heldColor);
	ReleaseTexture(heldDepth);
	ReleaseTexture(heldCategories);
	heldPlacement = UINT_MAX;
	// The model's history is of the held frame; the live frame it returns to is unrelated.
	if (wasHeld)
		RequestHistoryReset();
}

void NeuralRendering::CaptureFinishedImageGuides()
{
	finishedImageGuidesReady = false;
	if (!IsDLSSActive() || !settings.enabled || !IsPlacement(Placement::kFinishedImage))
		return;

	auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	if (!depth.texture || !depth.depthSRV)
		return;

	D3D11_TEXTURE2D_DESC depthDesc{};
	depth.texture->GetDesc(&depthDesc);
	auto*& snapshot = finishedImageDepthSnapshot;
	if (snapshot && (snapshot->desc.Width != depthDesc.Width || snapshot->desc.Height != depthDesc.Height ||
						snapshot->desc.Format != depthDesc.Format)) {
		ReleaseTexture(snapshot);
	}
	if (!snapshot) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		depth.depthSRV->GetDesc(&srvDesc);
		try {
			// The source's own description, bind flags included, the same way kMAIN_COPY's depth
			// mirrors kMAIN's, so the whole-resource copy below is valid for a depth-stencil source.
			snapshot = new Texture2D(depthDesc, "NeuralRendering::FinishedImageDepth");
			snapshot->CreateSRV(srvDesc);
		} catch (const std::exception& e) {
			static bool loggedFailure = false;
			if (!loggedFailure) {
				loggedFailure = true;
				logger::warn("[NeuralRendering] Finished Image disabled: depth snapshot creation failed ({})", e.what());
			}
			ReleaseTexture(snapshot);
			return;
		}
	}

	globals::d3d::context->CopyResource(snapshot->resource.get(), depth.texture);

	// The same render-resolution extent the After Upscaling placement passes, computed at the
	// same point in the frame - before dynamicResolutionLock turns dynamic resolution off.
	const auto guideSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });
	finishedImageGuideWidth = static_cast<uint32_t>(guideSize.x);
	finishedImageGuideHeight = static_cast<uint32_t>(guideSize.y);
	finishedImageGuidesReady = true;
}

Texture2D* NeuralRendering::EnsureFinishedImageTexture(const D3D11_TEXTURE2D_DESC& a_targetDesc)
{
	auto*& texture = finishedImageTexture;
	if (texture && texture->desc.Width == a_targetDesc.Width && texture->desc.Height == a_targetDesc.Height &&
		texture->desc.Format == a_targetDesc.Format && texture->desc.SampleDesc.Count == a_targetDesc.SampleDesc.Count)
		return texture;

	ReleaseTexture(texture);

	// Require a single-sample typed UAV format compatible with CopyResource.
	auto device = globals::d3d::device;
	D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2{ a_targetDesc.Format, 0 };
	const bool uavCapable = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2))) &&
	                        (support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE);
	if (!uavCapable || a_targetDesc.SampleDesc.Count != 1) {
		if (finishedImageRejectedFormat != a_targetDesc.Format) {
			finishedImageRejectedFormat = a_targetDesc.Format;
			logger::warn(
				"[NeuralRendering] Finished Image disabled: tonemap output format {} (samples {}) "
				"cannot be written through a UAV",
				static_cast<int>(a_targetDesc.Format), a_targetDesc.SampleDesc.Count);
		}
		return nullptr;
	}

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = a_targetDesc.Width;
	desc.Height = a_targetDesc.Height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = a_targetDesc.Format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	try {
		// Only the resource is needed: the backend creates and caches its own UAV over it.
		texture = new Texture2D(desc, "NeuralRendering::FinishedImageTexture");
	} catch (const std::exception& e) {
		logger::warn("[NeuralRendering] Finished Image disabled: output texture creation failed ({})", e.what());
		texture = nullptr;
		return nullptr;
	}

	logger::debug("[NeuralRendering] Finished Image output allocated {}x{} format {}",
		desc.Width, desc.Height, static_cast<int>(desc.Format));
	return texture;
}

void NeuralRendering::ApplyFinishedImage(RE::RENDER_TARGET a_target)
{
	if (!loaded || !settings.enabled || !IsPlacement(Placement::kFinishedImage))
		return;
	// Matches every pipeline pass's own DisableInMainLoadingMenu()-style guard - nothing should
	// be editing the main menu background or a loading screen.
	if (globals::state->IsMainOrLoadingMenuOpen()) {
		logger::debug("[NeuralRendering] Finished Image skipped: main menu or loading screen open");
		return;
	}

	auto& targetRT = globals::game::renderer->GetRuntimeData().renderTargets[a_target];
	if (!targetRT.SRV) {
		logger::debug("[NeuralRendering] Finished Image skipped: render target {} has no SRV",
			static_cast<int>(a_target));
		return;
	}

	// kFRAMEBUFFER on flat can alias the swap-chain backbuffer through its views alone, with a
	// null texture pointer (see ScreenshotFeature's ResolveSlotTexture); recover it from the SRV.
	winrt::com_ptr<ID3D11Texture2D> targetTextureHolder;
	ID3D11Texture2D* targetTexture = targetRT.texture;
	if (!targetTexture) {
		winrt::com_ptr<ID3D11Resource> resource;
		targetRT.SRV->GetResource(resource.put());
		if (resource)
			targetTextureHolder = resource.try_as<ID3D11Texture2D>();
		targetTexture = targetTextureHolder.get();
	}
	if (!targetTexture) {
		logger::debug("[NeuralRendering] Finished Image skipped: render target {} has no 2D texture",
			static_cast<int>(a_target));
		return;
	}

	// Match the tonemap target format; kMAIN may differ and cannot be copied back into it.
	D3D11_TEXTURE2D_DESC targetDesc{};
	targetTexture->GetDesc(&targetDesc);
	auto* finishedImage = EnsureFinishedImageTexture(targetDesc);
	if (!finishedImage)
		return;

	if (!EvaluateFinishedImage(targetTexture, targetRT.SRV, finishedImage->resource.get()))
		return;

	globals::d3d::context->CopyResource(targetTexture, finishedImage->resource.get());
}

// Frame bracket and comparison capture

// Four-frame comparison: reset and warm up with NR off, capture off, reset and warm up with NR on,
// then capture on and restore settings. Both captures precede the HUD.
void NeuralRendering::ServiceComparison(bool a_framePhaseStart)
{
	if (a_framePhaseStart) {
		if (compareStep == 0 && comparePending.exchange(false, std::memory_order_acq_rel)) {
			const bool canCompare = IsDLSSActive() &&
			                        !globals::features::upscaling.ShouldUseFrameGenerationThisFrame() &&
			                        IsAvailable() &&
			                        globals::features::screenshotFeature.loaded;
			if (!canCompare) {
				ShowHUDMessageDeferred("Neural Rendering comparison needs DLSS active and Frame Generation off");
				return;
			}

			SYSTEMTIME st;
			GetLocalTime(&st);
			compareStamp = std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}_{:03}",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
			compareUserSetting = settings.enabled;

			compareStep = 1;
			settings.enabled = false;  // frame 1: OFF, warm-up
			RequestHistoryReset();
		}
		return;
	}

	// Capture before the HUD, then advance the comparison sequence.
	switch (compareStep) {
	case 1:
		compareStep = 2;
		settings.enabled = false;  // frame 2: OFF, capture
		break;
	case 2:
		globals::features::screenshotFeature.Capture(
			ScreenshotFeature::NeuralRenderingComparisonPath(compareStamp, "_NR-off"), /*forceCleanNoUI=*/true);
		compareStep = 3;
		settings.enabled = true;  // frame 3: ON, warm-up
		RequestHistoryReset();
		break;
	case 3:
		compareStep = 4;
		settings.enabled = true;  // frame 4: ON, capture
		break;
	case 4:
		globals::features::screenshotFeature.Capture(
			ScreenshotFeature::NeuralRenderingComparisonPath(compareStamp, "_NR-on"), /*forceCleanNoUI=*/true);
		settings.enabled = compareUserSetting;  // restore
		RequestHistoryReset();
		compareStep = 0;
		ShowHUDMessageDeferred("Saved Neural Rendering comparison to Data/DLSS 5 Screenshots");
		break;
	default:
		break;
	}
}

void NeuralRendering::Main_PostProcessing::thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5)
{
	auto& neuralRendering = globals::features::neuralRendering;

	// World and first-person geometry are done; take the categories the forward
	// lighting draws added before anything below reads the snapshot.
	neuralRendering.FinishCategoryCapture();
	neuralRendering.ServiceComparison(/*framePhaseStart=*/true);
	neuralRendering.BeginFrame();

	// Upscaling's own pass: upscaling (with the PrepareUpscaleInput / ResolveUpscaledFrame
	// seam calls), sharpening, then the game's imagespace chain including the tonemap.
	func(a_this, a3, a_target, a_4, a_5);

	neuralRendering.ServiceComparison(/*framePhaseStart=*/false);
}

RE::BSEventNotifyControl NeuralRendering::MenuOpenCloseEventHandler::ProcessEvent(
	const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// Loading transitions (cell/worldspace changes, initial load) break temporal continuity.
	if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening)
		globals::features::neuralRendering.pendingReset.store(true, std::memory_order_release);

	return RE::BSEventNotifyControl::kContinue;
}

bool NeuralRendering::MenuOpenCloseEventHandler::Register()
{
	static MenuOpenCloseEventHandler singleton;
	static bool registered = false;
	if (registered)
		return true;

	auto* ui = globals::game::ui;
	if (!ui)
		return false;
	auto* source = ui->GetEventSource<RE::MenuOpenCloseEvent>();
	if (!source)
		return false;

	source->AddEventSink(&singleton);
	registered = true;
	return true;
}
