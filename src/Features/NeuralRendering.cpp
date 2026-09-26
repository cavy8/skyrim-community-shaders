#include "NeuralRendering.h"

#include "NeuralRendering/Backend.h"

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "HDRDisplay.h"
#include "LinearLighting.h"
#include "PostProcessing.h"
#include "PostProcessing/HistogramAutoExposure.h"
#include "ScreenshotFeature.h"
#include "State.h"
#include "Upscaling.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cmath>
#include <format>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NeuralRendering::CategoryStrengths,
	colorStrength,
	transferStrength,
	luminosityStrength,
	hueGuard);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NeuralRendering::Settings,
	enabled,
	placement,
	style,
	intensity,
	colorStrength,
	localToneStrength,
	localStructureStrength,
	skinStructureStrength,
	automaticMask,
	resolutionMode,
	resolutionScale,
	resolutionScaleX,
	resolutionScaleY,
	transferStrength,
	luminosityStrength,
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
	NeuralRenderingBackend::FrameInputs MakeFrameInputs(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
		ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV, ID3D11ShaderResourceView* materialCategoriesSRV,
		ID3D11Resource* motionVectors,
		uint32_t width, uint32_t height, const NeuralRendering::Options& options)
	{
		NeuralRenderingBackend::FrameInputs inputs;
		inputs.colorIn = colorIn;
		inputs.colorOut = colorOut;
		inputs.depth = depth;
		inputs.depthSRV = depthSRV;
		inputs.materialCategoriesSRV = materialCategoriesSRV;
		inputs.motionVectors = motionVectors;
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
		inputs.display.vanillaGrading = options.display.vanillaGrading;
		std::copy_n(options.display.param, 4, inputs.display.param);
		std::copy_n(options.display.cinematic, 4, inputs.display.cinematic);
		std::copy_n(options.display.tint, 4, inputs.display.tint);
		inputs.display.vanillaAdaptationSRV = options.display.vanillaAdaptationSRV;
		inputs.display.postProcessExposure = options.display.postProcessExposure;
		inputs.display.postProcessAdaptationSRV = options.display.postProcessAdaptationSRV;
		inputs.display.postProcessExposureScale = options.display.postProcessExposureScale;
		std::copy_n(options.display.postProcessAdaptationRange, 2, inputs.display.postProcessAdaptationRange);
		inputs.intensity = options.intensity;
		inputs.colorStrength = options.colorStrength;
		inputs.transferStrength = options.transferStrength;
		inputs.luminosityStrength = options.luminosityStrength;
		inputs.maxRatio = options.maxRatio;
		inputs.ratioGuardEnabled = options.ratioGuardEnabled;
		for (std::size_t index = 0; index < options.categoryStrengths.size(); ++index) {
			inputs.categoryColorStrengths[index] = options.categoryStrengths[index].colorStrength;
			inputs.categoryTransferStrengths[index] = options.categoryStrengths[index].transferStrength;
			inputs.categoryLuminosityStrengths[index] = options.categoryStrengths[index].luminosityStrength;
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
		inputs.reset = options.reset;
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

	// Hair by shader authoring: the hair-tint material (which the HAIR technique
	// already covers) or the hair soft-lighting property flag on any other
	// material. This is what identifies wigs and other hair worn as equipment,
	// which have no head part to match.
	bool IsHairTintShader(const RE::BSRenderPass* a_pass)
	{
		if (!a_pass->shaderProperty || a_pass->shaderProperty->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
			return false;
		const auto* lightingProperty = static_cast<const RE::BSLightingShaderProperty*>(a_pass->shaderProperty);
		return (lightingProperty->material && lightingProperty->material->GetFeature() == RE::BSShaderMaterial::Feature::kHairTint) ||
		       lightingProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kHairTint);
	}

	// Head parts hang directly under the actor's skinned face node, each as a
	// child named by the part's editor ID (what Actor::GetHeadPartObject looks
	// up), so the head part a geometry belongs to is its ancestor sitting right
	// under that node. Classifying hair from the NPC record rather than the
	// material catches the hairlines, braids and loose strands that hair mods
	// author with the default or skin-tint shader type instead of hair tint.
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

// ---------------------------------------------------------------------------------------------
// Backend adapter
// ---------------------------------------------------------------------------------------------

bool NeuralRendering::IsAvailable() const
{
	return backend->IsAvailable();
}

bool NeuralRendering::IsFeatureAvailable() const
{
	return backend->IsFeatureAvailable();
}

bool NeuralRendering::Evaluate(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
	ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
	ID3D11ShaderResourceView* materialCategoriesSRV,
	ID3D11Resource* motionVectors,
	uint32_t width, uint32_t height, const Options& options)
{
	// Each placement is 1:1 in colour/output space. After-upscale colour is
	// display-resolution while the depth and motion guides retain the render
	// resolution used by DLSS SR, so their extents are carried independently.
	return backend->Evaluate(MakeFrameInputs(colorIn, colorOut, depth, depthSRV,
		materialCategoriesSRV, motionVectors, width, height, options));
}

bool NeuralRendering::PrepareSeparateUpscaling(ID3D11Resource* colorIn, ID3D11Resource* editedColor,
	ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
	ID3D11ShaderResourceView* materialCategoriesSRV,
	ID3D11Resource* motionVectors, ID3D11Resource* superResolutionMotionVectors,
	uint32_t width, uint32_t height,
	uint32_t outputWidth, uint32_t outputHeight, const Options& options)
{
	auto inputs = MakeFrameInputs(colorIn, editedColor, depth, depthSRV, materialCategoriesSRV, motionVectors, width, height, options);
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

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

#define I18N_KEY_PREFIX "feature.neural_rendering."

void NeuralRendering::DrawSettings()
{
	if (!IsDLSSActive()) {
		ImGui::TextDisabled("%s", T(TKEY("requires_dlss"),
			"DLSS Neural Rendering requires the DLSS upscaling method. Select DLSS in the Upscaling feature first."));
		return;
	}

	const bool backendAvailable = IsAvailable();
	const bool featureAvailable = IsFeatureAvailable();
	if (!backendAvailable) {
		ImGui::TextDisabled("%s", T(TKEY("unavailable"),
			"DLSS Neural Rendering is unavailable. Install a compatible user-supplied nvngx_dlssnr.dll."));
	} else if (featureAvailable) {
		ImGui::TextUnformatted(T(TKEY("available"), "DLSS Neural Rendering is available."));
	} else {
		ImGui::TextDisabled("%s", T(TKEY("backend_ready"),
			"NGX backend ready; feature support will be tested when enabled."));
	}

	ImGui::Checkbox(T(TKEY("enabled"), "Enable Neural Rendering"), &settings.enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("enabled_tooltip"),
			"Applies DLSS 5 Neural Rendering to the upscaled image. A compatible user-supplied nvngx_dlssnr.dll is required."));
	}

	ImGui::BeginDisabled(!backendAvailable);
	if (ImGui::Button(T(TKEY("compare_screenshot"), "Take Comparison Screenshot"))) {
		RequestComparisonCapture();
	}
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("compare_screenshot_tooltip"),
			"Renders a few extra frames to save a matched pair - one with Neural Rendering off, one on "
			"- with no HUD or menu, into Data/DLSS 5 Screenshots/. Causes a brief hitch. Requires DLSS with Frame Generation off."));
	}

	const bool controlsAvailable = settings.enabled && backendAvailable;
	if (!controlsAvailable)
		ImGui::BeginDisabled();

	// --- Pipeline: where, and at what resolution, Neural Rendering runs ---
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
			"Before Upscaling lets the game's DLSS reconstruct the NR-edited scene. After Upscaling runs NR at display resolution.\n"
			"Separate Upscaling runs NR at render resolution, sends only its signed contribution through a second private DLSS history, "
			"then applies it to the clean main-DLSS result. This experimental mode costs another DLSS evaluation and additional VRAM.\n"
			"Finished Image runs NR last, after the frame's tonemap - Effects11's, Post Processing's, or vanilla's, whichever owned "
			"it - instead of the linear HDR scene the other placements approximate with a proxy. Depth of Field and Motion Blur "
			"already ran earlier in Post Processing's own pipeline (it tonemaps last, not them), so this does not run before them. "
			"Disabled over the main menu and loading screens."));
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
			"Uniform runs the model at one scale of the frame it processes.\n"
			"Per-Axis scales width and height independently (for example 0.65 x 0.85), trading a little "
			"horizontal detail for a larger reduction of the neural workload."));
	}
	if (settings.resolutionMode == 0) {
		ImGui::SliderFloat(T(TKEY("resolution_scale"), "Resolution Scale"), &settings.resolutionScale, 0.25f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("resolution_scale_tooltip"),
				"Resolution the model runs at, relative to the frame it processes.\n"
				"Below 1.0 the model works on a downsampled copy and only its lighting and colour edit is applied "
				"to the full-resolution frame, so fine detail is kept; 0.75-0.85 cuts the neural cost by roughly a "
				"third with little visible loss. Above 1.0 supersamples the model input.\n"
				"Changes apply once the slider settles."));
		}
	} else {
		ImGui::SliderFloat(T(TKEY("resolution_scale_x"), "Horizontal Scale"), &settings.resolutionScaleX, 0.25f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("resolution_scale_x_tooltip"),
				"Model width relative to the frame width. Changes apply once the slider settles."));
		}
		ImGui::SliderFloat(T(TKEY("resolution_scale_y"), "Vertical Scale"), &settings.resolutionScaleY, 0.25f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("resolution_scale_y_tooltip"),
				"Model height relative to the frame height. Changes apply once the slider settles."));
		}
	}
	ImGui::Checkbox(T(TKEY("depth_aware_resolve"), "Depth-Aware Silhouette Preservation"), &settings.depthAwareResolve);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("depth_aware_resolve_tooltip"),
			"When the model runs below full resolution, fades its edit across depth edges so background "
			"changes do not bleed into thin foreground geometry. Has no effect at a resolution scale of 1.0."));
	}
	ImGui::Checkbox(T(TKEY("alternate_frames"), "Alternate Frames (Experimental)"), &settings.alternateFrames);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("alternate_frames_tooltip"),
			"Runs the model every other frame and re-applies its previous result to the frames in between, "
			"fading it wherever the image changed. Halves the neural cost, but fast motion may show a "
			"one-frame lag in the model's lighting and detail changes."));
	}

	// --- Model tuning: information handed to the DLSS Neural Rendering model itself ---
	ImGui::Separator();
	ImGui::TextUnformatted(T(TKEY("model_inputs"), "Model Tuning"));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("model_inputs_tooltip"),
			"These are handed to the DLSS Neural Rendering model itself, guiding what it does to the frame. "
			"The strengths further down control how much of its answer Cav's Unity Shaders actually applies."));
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

	ImGui::SliderFloat(T(TKEY("intensity"), "NR Intensity"), &settings.intensity, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("intensity_tooltip"), "Adjust the overall Neural Rendering intensity."));
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
		ImGui::TextUnformatted(T(TKEY("skin_structure_tooltip"), "Adjust skin structure detail. -1 disables this control."));
	}
	ImGui::Checkbox(T(TKEY("automatic_mask"), "Automatic Mask"), &settings.automaticMask);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("automatic_mask_tooltip"), "Generates the skin mask automatically."));
	}

	// --- Strengths: how much of the model's answer Cav's Unity Shaders applies ---
	ImGui::Separator();
	ImGui::TextUnformatted(T(TKEY("strengths"), "Strengths"));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("strengths_tooltip"),
			"How much of the model's answer is actually applied to the frame. The per-category overrides "
			"below multiply on top of these as a final adjustment layer."));
	}

	ImGui::SliderFloat(T(TKEY("color_strength"), "Color Strength"), &settings.colorStrength, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("color_strength_tooltip"),
			"Blend the model's color changes independently of its bounded lighting and detail changes. 1 is the "
			"model's own color change; above 1 extrapolates the same change further."));
	}
	ImGui::SliderFloat(T(TKEY("transfer_strength"), "Transfer Strength"), &settings.transferStrength, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("transfer_strength_tooltip"),
			"How much of the model's edit is applied to the frame. 0 leaves the frame untouched, 1 applies the "
			"model's change exactly, 2 exaggerates it. Unlike NR Intensity this takes effect immediately."));
	}
	ImGui::SliderFloat(T(TKEY("luminosity_strength"), "Luminosity Strength"), &settings.luminosityStrength, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("luminosity_strength_tooltip"),
			"Scales only the model's light/dark change, on top of Transfer Strength; its color and detail edit "
			"are unaffected. Lower it if Neural Rendering reads as too contrasty without giving up its color work."));
	}
	ImGui::Checkbox(T(TKEY("ratio_guard_enabled"), "Enable Ratio Guard"), &settings.ratioGuardEnabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("ratio_guard_enabled_tooltip"),
			"Off by default: the model's light/dark change is applied exactly as it computed it, however far it "
			"swings - including turning a lit surface fully into shadow. Turn this on to cap that swing with Max "
			"Ratio below, if a specific scene flashes or flickers; capping it can also crush shadow detail the "
			"model was correctly reproducing."));
	}
	if (settings.ratioGuardEnabled) {
		ImGui::SliderFloat(T(TKEY("max_ratio"), "Max Ratio"), &settings.maxRatio, 1.0f, 8.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(T(TKEY("max_ratio_tooltip"),
				"How far the model's light/dark change is allowed to push a pixel, as a multiple of its original "
				"brightness in either direction (2 means at most half as dark or twice as bright). 1 disables any "
				"brightness change. Lower this if a specific scene flashes or flickers; raising it further "
				"re-approaches the guard being off."));
		}
	}

	// --- Per-category overrides, each with its own hue guard ---
	ImGui::Separator();
	ImGui::TextUnformatted(T(TKEY("category_overrides"), "Per-Category Overrides"));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("category_overrides_tooltip"),
			"Override the strengths above, and toggle hue guard, independently for each material category. "
			"The strengths above still apply afterwards as a final multiplier over every category."));
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
			"Static architecture and clutter, plus water, sky, particles, UI, and anything not covered above."));

	// --- Debug: inspect the category classification itself ---
	ImGui::Separator();
	ImGui::TextUnformatted(T(TKEY("debug"), "Debug"));

	ImGui::Checkbox(T(TKEY("debug_category_view"), "Show Material Categories"), &settings.debugCategoryView);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("debug_category_view_tooltip"),
			"Replaces the frame with a flat colour per classified material category (red Skin, orange Hair, "
			"yellow Eyes, green Foliage, cyan Landscape, purple Equipment, near-black Everything Else). Shows "
			"the raw per-pixel classification, not the per-category strengths above. Neural Rendering still "
			"evaluates normally underneath, so this costs the same as leaving it off."));
	}

	ImGui::BeginDisabled(!IsPlacement(Placement::kFinishedImage));
	ImGui::Checkbox(T(TKEY("raw_model_output"), "Raw Model Output"), &settings.rawModelOutput);
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("raw_model_output_tooltip"),
			"Finished Image only. Writes what the DLSS model actually produced straight to the screen, skipping "
			"every strength, guard, and blend above entirely. Useful for telling apart a weak model answer from "
			"an over-conservative resolve - not meant to be left on."));
	}

	if (!controlsAvailable)
		ImGui::EndDisabled();
}

void NeuralRendering::DrawCategoryStrengths(const char* a_id, const char* a_label, CategoryStrengths& a_strengths, const char* a_tooltip)
{
	if (!ImGui::TreeNodeEx(a_id, ImGuiTreeNodeFlags_None, "%s", a_label))
		return;
	if (a_tooltip) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(a_tooltip);
	}
	ImGui::SliderFloat(T(TKEY("color_strength"), "Color Strength"),
		&a_strengths.colorStrength, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("transfer_strength"), "Transfer Strength"),
		&a_strengths.transferStrength, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("luminosity_strength"), "Luminosity Strength"),
		&a_strengths.luminosityStrength, 0.0f, 2.0f, "%.2f");
	ImGui::Checkbox(T(TKEY("hue_guard"), "Neutral Colour Guard"), &a_strengths.hueGuard);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(T(TKEY("hue_guard_tooltip"),
			"Stops the model from tinting this category's renderer-neutral shading (grey, or near-grey "
			"shadows) with its own colour bias. Surfaces the model already recolours are unaffected. "
			"Only Hair guards by default."));
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
	settings = o_json;

	if (settings.placement > 3) {
		logger::warn("[NeuralRendering] Loaded placement {} out of range, clamping to 1", settings.placement);
		settings.placement = 1;
	}
	if (settings.style > 2)
		settings.style = 2;
	const auto sanitizeFloat = [](float& value, float fallback, float min, float max) {
		if (!std::isfinite(value))
			value = fallback;
		value = std::clamp(value, min, max);
	};
	sanitizeFloat(settings.intensity, 0.8f, 0.0f, 2.0f);
	sanitizeFloat(settings.colorStrength, 1.0f, 0.0f, 1.0f);
	sanitizeFloat(settings.localToneStrength, 1.0f, 0.0f, 2.0f);
	sanitizeFloat(settings.localStructureStrength, 1.0f, 0.0f, 2.0f);
	sanitizeFloat(settings.skinStructureStrength, -1.0f, -1.0f, 2.0f);
	if (settings.resolutionMode > 1)
		settings.resolutionMode = 1;
	sanitizeFloat(settings.resolutionScale, 1.0f, 0.25f, 2.0f);
	sanitizeFloat(settings.resolutionScaleX, 1.0f, 0.25f, 2.0f);
	sanitizeFloat(settings.resolutionScaleY, 1.0f, 0.25f, 2.0f);
	sanitizeFloat(settings.transferStrength, 1.0f, 0.0f, 2.0f);
	sanitizeFloat(settings.luminosityStrength, 1.0f, 0.0f, 2.0f);
	const auto sanitizeCategoryStrengths = [&](CategoryStrengths& strengths) {
		sanitizeFloat(strengths.colorStrength, 1.0f, 0.0f, 1.0f);
		sanitizeFloat(strengths.transferStrength, 1.0f, 0.0f, 2.0f);
		sanitizeFloat(strengths.luminosityStrength, 1.0f, 0.0f, 2.0f);
	};
	sanitizeCategoryStrengths(settings.everythingElseStrengths);
	sanitizeCategoryStrengths(settings.skinStrengths);
	sanitizeCategoryStrengths(settings.hairStrengths);
	sanitizeCategoryStrengths(settings.eyesStrengths);
	sanitizeCategoryStrengths(settings.foliageStrengths);
	sanitizeCategoryStrengths(settings.landscapeStrengths);
	sanitizeCategoryStrengths(settings.equipmentStrengths);
}

void NeuralRendering::RestoreDefaultSettings()
{
	settings = {};
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

// ---------------------------------------------------------------------------------------------
// Lifecycle and hooks
// ---------------------------------------------------------------------------------------------

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
	const auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	// The pre-blended-decals snapshot, not the live Masks2 - see CaptureCategories.
	auto* materialCategoriesSRV = materialCategoriesSnapshot ? materialCategoriesSnapshot->srv.get() : nullptr;
	// Hand the model the game's raw motion-vector target, not the 5x5
	// dilated ghosting-reduction copy Streamline gets. That copy tags a
	// two-texel rim of background with foreground motion, which is a
	// deliberate lie for DLSS's history rejection. The model feeds its own
	// temporal state and was trained on plain per-pixel vectors, so the
	// dilated rim reads as flicker or smear along moving edges.
	ID3D11Resource* upscaleInput = a_color;
	globals::profiler->BeginPass("NeuralRendering::Generate");
	if (IsPlacement(Placement::kBeforeUpscaling)) {
		if (Evaluate(a_color,
				output->resource.get(),
				depth.texture,
				depth.depthSRV,
				materialCategoriesSRV,
				motionVector.texture,
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
			a_superResolutionMotionVectors,
			renderWidth,
			renderHeight,
			nativeWidth,
			nativeHeight,
			options);
	}
	globals::profiler->EndPass();
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
			// Those guides are also still jittered, while the colour here is the frame
			// DLSS has already resolved onto the unjittered grid. Uncorrected, a guide
			// lookup indexes the unjittered grid but reads a texel whose sample sits up
			// to half a texel away, and that error swings coherently across the image
			// every frame as the jitter phase advances - flickering the per-category
			// strength and the silhouette fade along every category and depth boundary,
			// worst where the two sides' strengths differ most (the hairline, and thin
			// strands, which are boundary along their whole length). Before the upscaler
			// colour and guides are jittered alike, so that path leaves this zero.
			const auto& jitter = globals::features::upscaling.jitter;
			options.guideJitterOffsetX = -jitter.x;
			options.guideJitterOffsetY = -jitter.y;
			// Pre-tonemap: show the model the frame exposed and graded as it will be displayed.
			options.display = MakeDisplayTransform();
			// Raw game motion-vector target, not the dilated ghosting-reduction copy
			// DLSS consumes; see the matching note in PrepareUpscaleInput().
			globals::profiler->BeginPass("NeuralRendering::Generate");
			resultValid = Evaluate(a_upscaled->resource.get(),
				output->resource.get(),
				depth.texture,
				depth.depthSRV,
				materialCategoriesSRV,
				motionVector.texture,
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

	// Every lighting draw that writes Masks2: the deferred pass and the forward
	// draws between RestoreCategories and FinishCategoryCapture. A forward draw
	// seen with cleared flags would land skinned armor in Skin and rigid armor in
	// Everything Else.
	const bool writesCategories = deferred->deferredPass || forwardCaptureActive;

	bool isHumanoidActor = false;
	bool isHair = false;
	if (writesCategories && settings.enabled && actorTypeNPCKeyword && a_pass->geometry) {
		// Hair by shader authoring (wigs) or by head part (hairlines, braids and
		// strands authored with other shader types); see Lighting.hlsl.
		isHair = IsHairTintShader(a_pass);
		if (auto userData = a_pass->geometry->GetUserData()) {
			if (auto actor = userData->As<RE::Actor>()) {
				// Any geometry owned by a humanoid actor - skinned armor/clothing
				// as well as rigid weapons, shields and helmets attached to its
				// skeleton. Skin (body and face) and eyes are claimed by their own
				// material permutations before the shader consults this flag.
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

	// Created lazily here: this only ever runs mid-frame (from Deferred's
	// blended-decals hook), after Masks2 has genuinely been (re)created and
	// rendered into. Masks2 is a repurposed native target (see MASKS2 in
	// Deferred.h) that is not guaranteed to exist during the game's own
	// render-target (re)creation.
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

	// Only the forward lighting draws of the main world view, into the same
	// full-resolution colour target the deferred pass restored: a cubemap
	// face or reflection target in slot 0 would fail OMSetRenderTargets
	// against a render-resolution Masks2.
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

// ---------------------------------------------------------------------------------------------
// Options and display transform
// ---------------------------------------------------------------------------------------------

NeuralRendering::Options NeuralRendering::MakeOptions() const
{
	const auto& upscalingSettings = globals::features::upscaling.settings;
	Options options{};
	options.style = settings.style;
	options.intensity = settings.intensity;
	options.colorStrength = settings.colorStrength;
	options.transferStrength = settings.transferStrength;
	options.luminosityStrength = settings.luminosityStrength;
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
			logger::warn("[NeuralRendering] Display transform: implausible ISHDR constants "
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
		logger::info("[NeuralRendering] Display transform captured: adaptation {}x{} format {}, "
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

// ---------------------------------------------------------------------------------------------
// Finished Image
// ---------------------------------------------------------------------------------------------

bool NeuralRendering::EvaluateFinishedImage(ID3D11Texture2D* a_colorIn, ID3D11ShaderResourceView* a_colorInSRV,
	ID3D11Texture2D* a_colorOut)
{
	if (!settings.enabled || !IsPlacement(Placement::kFinishedImage))
		return false;

	// Past this point the user has clearly opted into this placement, so every remaining
	// early-out is logged at debug level - the fail-closed checks below are silent by design,
	// which otherwise looks identical to "doing nothing".
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
	// Guides captured for this upscaled frame by CaptureFinishedImageGuides().
	// Consuming them means a later tonemap-pass call this frame (a different colour target)
	// cannot re-run the model, which would reset its temporal history every frame.
	if (!finishedImageGuidesReady || !finishedImageDepthSnapshot) {
		logger::debug("[NeuralRendering] Finished Image skipped: no guides captured for this frame");
		return false;
	}
	finishedImageGuidesReady = false;

	auto* depthTexture = finishedImageDepthSnapshot->resource.get();
	auto* depthSRV = finishedImageDepthSnapshot->srv.get();
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	if (!depthTexture || !depthSRV || !motionVector.texture || !motionVector.SRV) {
		logger::debug("[NeuralRendering] Finished Image skipped: depth or motion-vector guide missing "
					  "(depthSnapshot={} depthSnapshotSRV={} motionVector.texture={} motionVector.SRV={})",
			(void*)depthTexture, (void*)depthSRV, (void*)motionVector.texture, (void*)motionVector.SRV);
		return false;
	}

	// The authoritative active resolution, same as every other Neural Rendering call site
	// (e.g. the After Upscaling placement) - not each resource's own GetDesc(), which can
	// legitimately be a larger, differently-padded allocation than the frame's active region.
	// This is the colour extent only; the guides' render-resolution extent was captured with them.
	const uint32_t nativeWidth = static_cast<uint32_t>(globals::game::graphicsState->screenWidth);
	const uint32_t nativeHeight = static_cast<uint32_t>(globals::game::graphicsState->screenHeight);
	if (!nativeWidth || !nativeHeight) {
		logger::debug("[NeuralRendering] Finished Image skipped: zero screen size ({}x{})", nativeWidth, nativeHeight);
		return false;
	}

	// The category snapshot (opaque categories captured before decals, forward categories added after), not the live Masks2 - see CaptureCategories.
	auto* materialCategoriesSRV = materialCategoriesSnapshot ? materialCategoriesSnapshot->srv.get() : nullptr;

	// Same guide contract as the After Upscaling placement: the colour is display resolution and
	// already resolved onto the unjittered grid, while depth (the pre-UpscaleDepth snapshot),
	// motion vectors and the category snapshot are render resolution and still carry this
	// frame's TAA jitter. Without these the model's motion vectors and every guide lookup are
	// misscaled below native and swing with the jitter phase every frame.
	const auto& jitter = globals::features::upscaling.jitter;
	Options options = MakeOptions();
	options.guideWidth = finishedImageGuideWidth;
	options.guideHeight = finishedImageGuideHeight;
	options.guideJitterOffsetX = -jitter.x;
	options.guideJitterOffsetY = -jitter.y;

	// The tonemap output is gamma-encoded display colour, except when HDR Display has redirected
	// kFRAMEBUFFER to its float16 texture and the scene arriving there is linear - the same test
	// HDROutputCS applies (isSceneLinear || postProcessOutput). Post Processing owning the tonemap
	// is its effective DisableVanillaTonemapping (see PostProcessing::GetCommonBufferData()).
	const auto& hdrDisplay = globals::features::hdrDisplay;
	const bool sceneLinear = hdrDisplay.loaded && hdrDisplay.framebufferRedirected &&
	                         (globals::features::linearLighting.settings.enableLinearLighting ||
								 globals::state->GetTonemapOwner() == State::TonemapOwner::kPostProcessing);
	options.colorDomain = sceneLinear ? ColorDomain::kSceneLinear : ColorDomain::kDisplayGamma;

	globals::profiler->BeginPass("NeuralRendering::Generate");
	const bool evaluated = Evaluate(a_colorIn, a_colorOut,
		depthTexture, depthSRV, materialCategoriesSRV, motionVector.texture,
		nativeWidth, nativeHeight, options);
	globals::profiler->EndPass();
	if (!evaluated) {
		logger::debug("[NeuralRendering] Finished Image skipped: Evaluate() returned false "
					  "(see preceding [NeuralRendering] log lines for the reason)");
		return false;
	}

	resourcesActive = true;
	return true;
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

	// The backend writes the edit through a typed UAV and the caller copies it back with
	// CopyResource, which needs a matching single-sample texture. sRGB, typeless and
	// multisampled targets can't do both, so fail closed rather than write wrong colours.
	auto device = globals::d3d::device;
	D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2{ a_targetDesc.Format, 0 };
	const bool uavCapable = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2))) &&
	                        (support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE);
	if (!uavCapable || a_targetDesc.SampleDesc.Count != 1) {
		if (finishedImageRejectedFormat != a_targetDesc.Format) {
			finishedImageRejectedFormat = a_targetDesc.Format;
			logger::warn("[NeuralRendering] Finished Image disabled: tonemap output format {} (samples {}) "
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

	// The tonemap output is kFRAMEBUFFER (or HDR Display's float16 redirect of it), whose format
	// differs from kMAIN. CopyResource between mismatched formats is silently dropped, so the
	// edit goes into a texture matching this target exactly rather than outputTexture.
	D3D11_TEXTURE2D_DESC targetDesc{};
	targetTexture->GetDesc(&targetDesc);
	auto* finishedImage = EnsureFinishedImageTexture(targetDesc);
	if (!finishedImage)
		return;

	if (!EvaluateFinishedImage(targetTexture, targetRT.SRV, finishedImage->resource.get()))
		return;

	globals::d3d::context->CopyResource(targetTexture, finishedImage->resource.get());
}

// ---------------------------------------------------------------------------------------------
// Frame bracket and comparison capture
// ---------------------------------------------------------------------------------------------

// Drives the comparison capture from the Main_PostProcessing hook. Called before Upscaling's
// pass so the forced Neural Rendering state is in place before the frame's upscaling runs,
// and again after compositing to queue the matching screenshot / advance the state machine.
// Four frames, symmetric so the pair is a fair A/B:
//
//   step 1: Neural Rendering OFF, DLSS history reset   -> warm-up, discarded
//   step 2: Neural Rendering OFF, converged one frame  -> queue "_NR-off"
//   step 3: Neural Rendering ON,  DLSS history reset    -> warm-up, discarded
//   step 4: Neural Rendering ON,  converged one frame   -> queue "_NR-on", restore setting
//
// The warm-up frames matter because Feature 18 needs one successful evaluation before it
// contributes and DLSS needs a frame to settle after a reset; without them the two halves
// would be captured at different points of convergence.
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

	// Frame phase end: post-processing is done and the game UI has not been drawn yet, so
	// a capture here has no HUD and no CS menu. Grab the frame where applicable, then set
	// up the next step.
	switch (compareStep) {
	case 1:
		compareStep = 2;
		settings.enabled = false;  // frame 2: OFF, capture
		break;
	case 2:
		// Runs at the end of Main_PostProcessing, before the game UI is drawn -> no HUD, no CS menu.
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
