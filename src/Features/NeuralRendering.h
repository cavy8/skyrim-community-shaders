#pragma once

#include "Buffer.h"
#include "Feature.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <d3d11.h>
#include <winrt/base.h>

class NeuralRenderingBackend;

/**
  * @brief DLSS Neural Rendering (NGX Feature 18), using a D3D11/D3D12 backend.
 */
struct NeuralRendering : Feature
{
	virtual inline std::string GetName() override { return "Neural Rendering"; }
	virtual std::string GetDisplayName() override { return T("feature.neural_rendering.name", "Neural Rendering"); }
	virtual inline std::string GetShortName() override { return "NeuralRendering"; }
	virtual inline bool IsCore() const override { return false; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.neural_rendering.description", "Enhance lighting, color, and detail with DLSS Neural Rendering. Requires a compatible nvngx_dlssnr.dll."),
			{ T("feature.neural_rendering.key_feature_1", "Enhance the image before or after upscaling, or at the final image stage"),
				T("feature.neural_rendering.key_feature_2", "Adjust enhancements and protect neutral colors by material"),
				T("feature.neural_rendering.key_feature_3", "Adjust resolution to balance quality and performance"),
				T("feature.neural_rendering.key_feature_4", "Matched on/off comparison screenshots") } };
	}

	/** @brief Per-material multipliers and toggles applied by the local colour resolve. */
	struct CategoryStrengths
	{
		float colorStrength = 1.0f;
		float transferStrength = 1.0f;
		/// Per-category multipliers on broad relighting and detail contrast. Equal values scale the entire
		/// luminance edit.
		float broadLuminosity = 1.0f;
		float detailLuminosity = 1.0f;
		/// Limit this category's chroma change to saturation on renderer-neutral pixels (see ResolveNeuralColor).
		/// Off by default; Hair enables it.
		bool hueGuard = false;
	};

	/** @brief Material categories encoded in the deferred Masks2 target. */
	enum class MaterialCategory : std::uint32_t
	{
		kEverythingElse = 0,
		kSkin,
		kHair,
		kEyes,
		kFoliage,
		kLandscape,
		kEquipment,
		kCount
	};

	static constexpr std::size_t kMaterialCategoryCount = static_cast<std::size_t>(MaterialCategory::kCount);
	using CategoryStrengthArray = std::array<CategoryStrengths, kMaterialCategoryCount>;

	/** @brief How the colour handed to Evaluate() is encoded; mirrored by ColorTransfer.hlsli. */
	enum class ColorDomain : uint32_t
	{
		kSceneLinear = 0,   ///< Linear, open-ended HDR scene colour (every pre-tonemap placement).
		kDisplayGamma = 1,  ///< Finished gamma-2.2 display-referred frame, 0-1 in SDR (Finished Image).
		/// Pre-tonemap gamma-encoded color with Linear Lighting off; decode using kNeuralSceneGamma.
		kSceneGamma = 2,
	};

	/** @brief Where in the frame Neural Rendering runs (Settings::placement). */
	enum class Placement : uint32_t
	{
		kBeforeUpscaling = 0,
		kAfterUpscaling = 1,
		kSeparateUpscaling = 2,
		kFinishedImage = 3,
	};

	/**
	 * @brief Preset values applied to Settings; subsequent edits are tracked by MatchesPreset().
	 */
	enum class Preset : uint32_t
	{
		kFull = 0,         ///< The model's own answer applied in full; the default.
		kVanillaPlus = 1,  ///< Legacy proxy, luminance-only edit capped at one stop.
		kCount
	};

	static constexpr std::size_t kPresetCount = static_cast<std::size_t>(Preset::kCount);

	/**
	 * @brief How the scene-linear placements build the image the model sees (Settings::proxyCurve).
	 *
	 * No effect on Finished Image, whose proxy is the finished frame itself. Mirrored by
	 * ColorTransfer.hlsli (kNeuralProxyCurve*).
	 */
	enum class ProxyCurve : uint32_t
	{
		kDisplayMatched = 0,  ///< The ISHDR replica, or the ACES fallback when grading cannot be captured.
		kNeutwo = 1,          ///< Exposed scene linear through the RenoDX Neutwo curve, then the domain encode.
		kLegacy = 2,          ///< Per-channel Reinhard, no exposure, no Linear Lighting decode.
		kCount
	};

	/**
	 * @brief Display transform used by pre-tonemap proxies. Defaults to no exposure or grading; ignored
	 * for display gamma.
	 */
	struct DisplayTransform
	{
		bool vanillaGrading = false;  ///< The vanilla tonemap owns the frame and the ISHDR constants below are valid.
		float param[4]{};             ///< ISHDR Param: y white point, z Hejl-Burgess-Dawson.
		float cinematic[4]{};         ///< ISHDR Cinematic: x saturation, z contrast, w brightness.
		float tint[4]{};              ///< ISHDR Tint: xyz colour, w amount.
		/// ISHDR's AvgTex from the previous tonemap pass: x adapted luminance, y target luminance.
		ID3D11ShaderResourceView* vanillaAdaptationSRV = nullptr;
		bool postProcessExposure = false;  ///< Post Processing's Histogram Auto Exposure is active downstream.
		/// Post Processing's adapted-luminance buffer (a single float).
		ID3D11ShaderResourceView* postProcessAdaptationSRV = nullptr;
		float postProcessExposureScale = 0.18f;             ///< 0.18 * exp2(exposure compensation).
		float postProcessAdaptationRange[2]{ 0.0f, 1.0f };  ///< Linear clamp range of the adapted luminance.
	};

	/** @brief Per-evaluation settings handed to the backend. */
	struct Options
	{
		uint32_t style = 3;
		float intensity = 0.8f;
		float colorStrength = 1.0f;
		/// Weight of the model's edit (0..2): zero leaves the frame untouched, one applies it exactly.
		float transferStrength = 1.0f;
		/// Multiplier on the edge-aware low-frequency luminance edit; does not affect chroma.
		float broadLuminosity = 1.0f;
		/// Multiplier on the remaining luminance detail. Band passes are skipped when effective Broad and
		/// Detail strengths match for every category.
		float detailLuminosity = 1.0f;
		/// Band blur radius in native model texels (2..32), scaled to preserve screen coverage. Used only when
		/// bands differ.
		float bandRadius = 8.0f;
		/// Two-sided luminance-ratio limit (1/maxRatio..maxRatio), enabled by ratioGuardEnabled. One prevents
		/// luminance changes.
		float maxRatio = 2.0f;
		/// Enable the luminance-ratio limit to reduce unstable brightness changes.
		bool ratioGuardEnabled = false;
		/// Per-material strength multipliers and neutral-color guards, combined with global strengths.
		CategoryStrengthArray categoryStrengths{};
		/// Fade lower-resolution edits at depth silhouettes to reduce background bleed. No effect at native
		/// scale.
		bool depthAwareResolve = false;
		/// Evaluate every other frame and reproject the previous edit between evaluations.
		bool alternateFrames = false;
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float skinStructureStrength = -1.0f;
		bool automaticMask = true;
		/// Show raw material categories instead of the resolved image. Model evaluation still runs.
		bool debugCategoryView = false;
		/// Write raw model output, retaining renderer alpha. Supported only in display gamma; bypasses
		/// strengths and guards.
		bool rawModelOutput = false;
		/// Debug view: mark pixels the ratio guard clamped (red brighten, blue darken).
		bool debugGuardClamp = false;
		/// Debug views of the luminosity bands (mid-grey = no change, +-2 stops). Only meaningful while the bands differ.
		bool debugBroadBand = false;
		bool debugDetailBand = false;
		/// Enable staged peak-luminance readback for the settings UI.
		bool measureModelPeak = false;
		bool reset = false;

		/// Active render-resolution guide extent. Zero uses the color extent, which is valid only before
		/// upscaling.
		uint32_t guideWidth = 0;
		uint32_t guideHeight = 0;

		/// Color projection jitter in render pixels: raster position = unjittered position + offset. Zero for
		/// resolved color; the backend compensates before evaluation.
		float jitterOffsetX = 0.0f;
		float jitterOffsetY = 0.0f;

		/// Guide jitter relative to color, in guide texels. Zero when both rasters share jitter; set after
		/// upscaling.
		float guideJitterOffsetX = 0.0f;
		float guideJitterOffsetY = 0.0f;

		/// Per-axis model scale (0.25..1). Changes are debounced before rebuilding shared resources.
		float resolutionScaleX = 1.0f;
		float resolutionScaleY = 1.0f;

		/// Input/output color encoding. The resolve applies enhancements in decoded linear light.
		ColorDomain colorDomain = ColorDomain::kSceneLinear;

		/// How the scene-linear placements build the proxy; ignored by Finished Image.
		ProxyCurve proxyCurve = ProxyCurve::kDisplayMatched;

		/// Captured ISHDR grading and Post Processing exposure for pre-tonemap proxies. Finished Image uses
		/// identity.
		DisplayTransform display{};

		/// HDR display peak relative to paper white, used for display-gamma highlight rolloff. Zero for SDR and
		/// scene domains.
		float highlightWhite = 0.0f;

		/// Split-screen comparison split as a fraction of the frame width (see CompareView);
		/// negative disables it.
		float wipePosition = -1.0f;
		/// The frame is a held one (Frame Hold): hand the model zero motion.
		bool staticMotion = false;

		/// DLSS-SR quality/preset mirrored from Upscaling for Separate Upscaling; ignored otherwise.
		uint32_t superResolutionQualityMode = 1;
		uint32_t superResolutionPreset = 0;
	};

	struct Settings
	{
		bool enabled = false;
		/// Which preset the current values came from (see Preset). Never re-applied on load:
		/// the stored values win, and the UI marks the preset "(modified)" where they differ.
		uint preset = static_cast<uint>(Preset::kFull);
		/// Name of the user preset the current values came from (see UserPreset); empty when they
		/// came from the built-in @ref preset. Like @ref preset, never re-applied on load.
		std::string userPreset;
		/// Show the settings that shape the edit itself rather than where and how big it runs.
		bool showAdvanced = false;
		uint placement = static_cast<uint>(Placement::kFinishedImage);
		uint style = 0;  // 0=Default, 1=Natural, 2=Cinematic
		float intensity = 1.0f;
		float colorStrength = 1.0f;
		/// How the scene-linear placements build the proxy (see ProxyCurve).
		uint proxyCurve = static_cast<uint>(ProxyCurve::kDisplayMatched);
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float skinStructureStrength = -1.0f;
		bool automaticMask = true;
		uint resolutionMode = 0;  // 0=Uniform scale, 1=Per-axis (experimental anamorphic) scale
		float resolutionScale = 1.0f;
		float resolutionScaleX = 1.0f;
		float resolutionScaleY = 1.0f;
		float transferStrength = 1.0f;
		/// Smooth and remaining halves of the luminance edit (see Options::broadLuminosity).
		float broadLuminosity = 1.0f;
		float detailLuminosity = 1.0f;
		float bandRadius = 8.0f;  // Model texels; only used while the two above differ.
		float maxRatio = 2.0f;    // Two-sided guard on the model/proxy luminance ratio (1/x..x).
		// Off by default: Max Ratio is not applied, so a large, correct light/dark swing is never capped.
		bool ratioGuardEnabled = false;
		CategoryStrengths everythingElseStrengths;
		// Full damps skin colour: the model's own skin tint is its most visible overreach.
		CategoryStrengths skinStrengths{ 0.6f, 1.0f, 1.0f, 1.0f, false };
		// Hair is the only category that hue-guards its chroma change by default.
		CategoryStrengths hairStrengths{ 1.0f, 1.0f, 1.0f, 1.0f, true };
		CategoryStrengths eyesStrengths;
		CategoryStrengths foliageStrengths;
		CategoryStrengths landscapeStrengths;
		CategoryStrengths equipmentStrengths;
		bool depthAwareResolve = false;  // Experimental; see Options::depthAwareResolve.
		bool alternateFrames = false;
		/// Debug view: render the classified material category instead of the model's edit.
		bool debugCategoryView = false;
		bool rawModelOutput = false;  // Diagnostic: skip the resolve, write Feature 18's answer directly (Finished Image only).
	};

	Settings settings;

	/**
	 * @brief Preset-owned settings. Excludes enable, model resolution, alternate frames, Advanced
	 * visibility, and runtime comparison controls.
	 */
	struct PresetValues
	{
		uint placement;
		uint style;
		float intensity;
		float localToneStrength;
		float localStructureStrength;
		float skinStructureStrength;
		bool automaticMask;
		uint proxyCurve;
		float colorStrength;
		float transferStrength;
		float broadLuminosity;
		float detailLuminosity;
		float bandRadius;
		bool ratioGuardEnabled;
		float maxRatio;
		bool depthAwareResolve;
		CategoryStrengthArray categories;
	};

	/** @brief The preset table, indexed by Preset. */
	static const PresetValues& GetPreset(Preset a_preset);

	/**
	 * @brief Apply preset values and record the selection. Artistic tuning updates on the next evaluation;
	 * model-raster changes follow the backend's resolution debounce.
	 */
	void ApplyPreset(Preset a_preset);

	/**
	 * @brief Compare settings against a preset, excluding Placement and NR Intensity. Float tolerance is
	 * 1e-4.
	 */
	bool MatchesPreset(Preset a_preset) const;

	/** @brief The values the current @ref settings would store as a preset. */
	PresetValues CapturePresetValues() const;

	/**
	 * @brief Writes @p a_values into @ref settings without changing which preset is recorded
	 *        as their source (Settings::preset / Settings::userPreset).
	 */
	void ApplyPresetValues(const PresetValues& a_values);

	/** @brief MatchesPreset() against any value table, e.g. a user preset's. */
	bool MatchesPresetValues(const PresetValues& a_values) const;

	/**
	 * @brief User preset stored as a named JSON file in UserPresetDirectory(). Missing values default to
	 * Full; values are sanitized on load.
	 */
	struct UserPreset
	{
		std::string name;  ///< UTF-8; also the file's stem.
		PresetValues values;
	};

	/** @brief Data/SKSE/Plugins/CommunityShaders/NeuralRendering/Presets. */
	static std::filesystem::path UserPresetDirectory();

	/** @brief Re-reads every preset file; unreadable or malformed files are skipped with a warning. */
	void RefreshUserPresets();

	/** @brief The loaded user preset named @p a_name (exact match), or nullptr. */
	const UserPreset* FindUserPreset(std::string_view a_name) const;

	/** @brief Writes @p a_values into @ref settings and records @p a_preset as their source. */
	void ApplyUserPreset(const UserPreset& a_preset);

	/**
	 * @brief Creates or overwrites the preset file named @p a_name and updates the loaded list.
	 * @return False (logged) when the file cannot be written.
	 */
	bool WriteUserPreset(const std::string& a_name, const PresetValues& a_values);

	/**
	 * @brief Renames a user preset's file, following it in Settings::userPreset if it is active.
	 * @return False (logged) when the file cannot be renamed.
	 */
	bool RenameUserPreset(const std::string& a_from, const std::string& a_to);

	/**
	 * @brief Deletes a user preset's file; if it was active, the label falls back to the
	 *        built-in Settings::preset while the current values stay as they are.
	 * @return False (logged) when the file cannot be removed.
	 */
	bool DeleteUserPreset(const std::string& a_name);

	/**
	  * Runtime-only comparison aids; never saved.
	 */
	struct CompareView
	{
		bool wipe = false;          ///< Split screen: left of wipePosition shows the frame without Neural Rendering.
		float wipePosition = 0.5f;  ///< Split position as a fraction of the frame width.
		/// Keep re-evaluating one captured frame instead of the live one, so tuning changes can
		/// be judged on an identical image. Supported by Finished Image and After Upscaling.
		bool frameHold = false;
	};
	CompareView compareView;

	/**
	  * Runtime-only debug views and readbacks; never saved.
	 */
	struct DebugState
	{
		bool guardClampView = false;  ///< Mark the pixels the ratio guard actually clamped.
		bool broadBandView = false;   ///< Show the smooth half of the model's luminance edit.
		bool detailBandView = false;  ///< Show the remainder.
		bool measurePeak = false;     ///< Read the model answer's peak luminance back each frame.
	};
	DebugState debugState;

	NeuralRendering();
	~NeuralRendering();

	NeuralRendering(const NeuralRendering&) = delete;
	NeuralRendering& operator=(const NeuralRendering&) = delete;

	// Feature interface overrides
	virtual void DrawSettings() override;
	virtual void SaveSettings(json& o_json) override;
	virtual void LoadSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;
	virtual void PostPostLoad() override;

	// ---- Backend (NGX Feature 18) ----

	/**
	 * @brief Checks whether a usable nvngx_dlssnr.dll can be found and loaded.
	 * @return True when the runtime probe succeeded; the probe runs once and its result is cached.
	 */
	bool IsAvailable() const;

	/**
	 * @brief Checks whether Feature 18 has produced at least one successful frame.
	 * @return True after a successful evaluation; false does not imply the runtime is absent.
	 */
	bool IsFeatureAvailable() const;

	/**
	 * @brief Delayed resolve diagnostics, updated only while measurement is enabled.
	 */
	struct DebugReadback
	{
		float modelPeakLuminance = 0.0f;   ///< Peak luminance of the model's answer, in model-space units.
		float guardClampedPercent = 0.0f;  ///< Share of resolved pixels the ratio guard actually clamped.
		bool valid = false;
	};
	DebugReadback GetDebugReadback() const;

	/**
	 * @brief Execute Neural Rendering on the D3D11 immediate context, using compact shared color and guide
	 * textures.
	 *
	 * @param colorIn Input color resource; must be shader-readable.
	 * @param colorOut Distinct output resource receiving the neural-rendered image; must be UAV-writable.
	 * @param depth Depth resource.
	 * @param depthSRV Shader resource view over @p depth, used by the depth-guide compute pass.
	 * @param materialCategoriesSRV Packed material category and vertex-AO render target.
	 * @param motionVectors Motion-vector resource.
	 * @param motionVectorsSRV Shader resource view over @p motionVectors; reprojects a previous frame's answer.
	 * @param width Active region width in pixels.
	 * @param height Active region height in pixels.
	 * @param options Neural Rendering settings.
	 * @return True when NGX successfully evaluates the feature and the result reaches @p colorOut.
	 */
	bool Evaluate(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
		ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
		ID3D11ShaderResourceView* materialCategoriesSRV,
		ID3D11Resource* motionVectors, ID3D11ShaderResourceView* motionVectorsSRV,
		uint32_t width, uint32_t height, const Options& options);

	/**
	 * @brief Runs NR at render resolution and temporally upscales only its signed contribution.
	 *
	 * The original colour is left untouched for the game's normal DLSS pass. On success,
	 * ResolveSeparateUpscaling() can apply the private full-resolution residual afterwards.
	 */
	bool PrepareSeparateUpscaling(ID3D11Resource* colorIn, ID3D11Resource* editedColor,
		ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
		ID3D11ShaderResourceView* materialCategoriesSRV,
		ID3D11Resource* motionVectors, ID3D11ShaderResourceView* motionVectorsSRV,
		ID3D11Resource* superResolutionMotionVectors,
		uint32_t width, uint32_t height,
		uint32_t outputWidth, uint32_t outputHeight, const Options& options);

	/**
	 * @brief Applies the prepared full-resolution residual to a clean main-DLSS result.
	 * @return True only when a matching private DLSS evaluation completed this frame.
	 */
	bool ResolveSeparateUpscaling(ID3D11Resource* cleanColor, ID3D11Resource* colorOut,
		uint32_t width, uint32_t height);

	/**
	 * @brief Release private histories and shared textures on the render thread. Retains the runtime and
	 * interop device and clears the failure latch.
	 */
	void DestroyModelResources();

	// ---- Upscaling seam (the only calls made from inside Upscaling) ----

	/**
	 * @brief Seam S1, called before DLSS evaluation. Before Upscaling returns enhanced color; Separate
	 * Upscaling prepares a residual and returns the original. Failure returns the original.
	 *
	 * @param a_color kMAIN, the colour DLSS would otherwise upscale.
	 * @param a_superResolutionMotionVectors Upscaling's dilated motion-vector copy (Separate
	 *        Upscaling hands it to its private DLSS-SR alongside the raw game vectors).
	 * @return The resource DLSS should upscale this frame.
	 */
	ID3D11Resource* PrepareUpscaleInput(ID3D11Resource* a_color, ID3D11Resource* a_superResolutionMotionVectors);

	/**
	 * @brief Seams S2+S3, between Upscale() and UpscaleDepth(). Apply After/Separate results in place or
	 * snapshot render-resolution depth for Finished Image.
	 *
	 * @param a_upscaled Upscaling's display-resolution DLSS output (its sharpener input);
	 *        may be null when DLSS is not the upscale method.
	 */
	void ResolveUpscaledFrame(Texture2D* a_upscaled);

	// ---- Tonemap stage (Hooks.cpp) ----

	/**
	 * @brief Capture vanilla ISHDR constants and adaptation for the next frame. Null parameters invalidate
	 * the capture when another tonemapper owns the output.
	 *
	 * @param a_param The pass's shader parameters, or null when the vanilla pass did not run.
	 */
	void CaptureDisplayTransform(RE::ImageSpaceShaderParam* a_param);

	/**
	 * @brief Apply Finished Image after tone mapping, using an output matching the target format. Leaves
	 * the target unchanged on failure or during main-menu/loading screens.
	 *
	 * @param a_target Game render target holding this frame's finished colour.
	 */
	void ApplyFinishedImage(RE::RENDER_TARGET a_target);

	// ---- Material categories (Deferred.cpp) ----

	/**
	 * @brief Snapshot opaque Masks2 categories before decals corrupt their packed bits through alpha
	 * blending. RestoreCategories() continues capture after the deferred composite.
	 */
	void CaptureCategories();

	/**
	 * @brief Restore opaque categories after the composite consumes decal AO, then arm forward lighting
	 * capture. Called after DeferredPasses().
	 */
	void RestoreCategories();

	/**
	 * @brief Snapshot forward categories and disarm capture before Neural Rendering evaluates.
	 */
	void FinishCategoryCapture();

	/**
	 * @brief Set humanoid ownership and hair flags for lighting geometry. Material permutations take
	 * precedence over the Equipment fallback.
	 *
	 * @param a_pass The render pass being set up.
	 */
	void SetupGeometryCategory(RE::BSRenderPass* a_pass);

	// ---- Controls (Menu hotkeys and settings UI) ----

	/**
	 * @brief Reset NR and DLSS histories together on the next frame.
	 */
	void RequestHistoryReset();

	/** @brief Requests a Neural Rendering on/off comparison screenshot pair; captured over the next few rendered frames. */
	void RequestComparisonCapture();

private:
	std::unique_ptr<NeuralRenderingBackend> backend;

	/** @brief Upscaling is loaded and resolves to DLSS this frame (the only upscaler NR runs with). */
	bool IsDLSSActive() const;
	bool IsPlacement(Placement a_placement) const { return settings.placement == static_cast<uint>(a_placement); }

	/**
	 * @brief Builds the placement-independent Neural Rendering options from the current settings.
	 *
	 * Callers still set the guide extent and jitter offset, which depend on where
	 * in the frame the pass runs.
	 */
	Options MakeOptions() const;

	/**
	 * @brief Combine captured vanilla exposure/grading with active Post Processing auto exposure.
	 */
	DisplayTransform MakeDisplayTransform() const;

	/** @brief Returns the kMAIN-format output texture of the pre-tonemap placements, creating it on first use. */
	Texture2D* EnsureOutputTexture();

	/**
	 * @brief Returns a Finished Image output texture whose size, format and sample count match
	 * @p a_targetDesc, (re)creating it when they change.
	 * @return nullptr (logged once per format) when the target cannot back a UAV write plus a
	 * CopyResource back into it, e.g. an sRGB, typeless or multisampled target.
	 */
	Texture2D* EnsureFinishedImageTexture(const D3D11_TEXTURE2D_DESC& a_targetDesc);

	/**
	 * @brief Capture render-resolution depth and guide extents before UpscaleDepth(), only for Finished
	 * Image on DLSS.
	 */
	void CaptureFinishedImageGuides();

	/**
	 * @brief Evaluate tonemapped color with captured render-resolution guides. Consume guides once per
	 * upscaled frame; leave the input unchanged on failure.
	 *
	 * @param a_colorIn Caller's finished colour for this frame; must be shader-readable.
	 * @param a_colorInSRV SRV over @p a_colorIn.
	 * @param a_colorOut Receives the edited frame; must be UAV-capable and distinct from
	 * @p a_colorIn. Use a texture matching @p a_colorIn's own format so the caller can copy it back.
	 * @return True when the evaluation ran and @p a_colorOut was written.
	 */
	bool EvaluateFinishedImage(ID3D11Texture2D* a_colorIn, ID3D11ShaderResourceView* a_colorInSRV,
		ID3D11Texture2D* a_colorOut);

	/**
	 * @brief Copies this frame's colour, depth and category snapshot into the Frame Hold
	 *        textures, (re)creating them to match.
	 * @param a_colorIn The frame to hold, in whatever format its placement produces.
	 * @param a_depth Depth guide to hold alongside it: Finished Image's pre-UpscaleDepth
	 *        snapshot, or the live kMAIN depth for After Upscaling.
	 * @param a_depthSRV SRV over @p a_depth, mirrored onto the copy.
	 * @return False (the frame stays live) when a copy cannot be made.
	 */
	bool CaptureFrameHold(ID3D11Texture2D* a_colorIn, ID3D11Texture2D* a_depth, ID3D11ShaderResourceView* a_depthSRV);

	/** @brief Releases the Frame Hold textures and resets the model's history if a frame was held. */
	void ReleaseFrameHold();

	/**
	 * @brief Start of the frame bracket: history reset, placement changes, and leaving DLSS.
	 *
	 * Called from the Main_PostProcessing hook before Upscaling's own pass runs.
	 */
	void BeginFrame();

	/** @brief Releases every texture Neural Rendering owns plus the backend's shared resources. */
	void DestroyFrameResources();

	/**
	 * @brief Drives the four-frame comparison capture from the Main_PostProcessing hook.
	 * @param a_framePhaseStart True when called before the frame's upscaling pass (to force the
	 *        Neural Rendering state), false when called after compositing (to queue the screenshot).
	 */
	void ServiceComparison(bool a_framePhaseStart);

	/** @brief Loaded user presets, sorted by name; see RefreshUserPresets(). */
	std::vector<UserPreset> userPresets;
	bool userPresetsLoaded = false;

	/** Runtime state of the preset name and delete popups; never saved. */
	struct PresetEditor
	{
		enum class Action
		{
			kNone,
			kSaveAsNew,  ///< Store the current values under a new name.
			kCopy,       ///< Store the active preset's stored values under a new name.
			kRename,     ///< Rename the active user preset.
		};
		Action action = Action::kNone;
		bool popupOpen = false;
		bool openRequested = false;
		std::string name;        ///< The name being typed.
		std::string renameFrom;  ///< kRename only.
		PresetValues source{};   ///< kSaveAsNew / kCopy: the values to store.
		std::string error;       ///< Shown inside the name popup.
		std::string status;      ///< Shown under the preset buttons (a failed Save or Delete).
		std::string deleteName;  ///< The preset the delete confirmation is about.
	};
	PresetEditor presetEditor;

	/** @brief Draws the preset combo, Reset to preset, and the save/copy/rename/delete buttons. */
	void DrawPresetControls();

	/** @brief Draws the preset name and delete confirmation popups, outside any disabled block. */
	void DrawPresetPopups();

	/** @brief Opens the name popup for @p a_action, pre-filled with @p a_name. */
	void OpenPresetNamePopup(PresetEditor::Action a_action, std::string a_name, const PresetValues& a_source);

	/**
	 * @brief Why @p a_name cannot name a new preset, or nullptr when it can.
	 * @param a_name The sanitized name.
	 * @param a_renameFrom The preset being renamed (it may keep its own name), or empty.
	 */
	const char* PresetNameProblem(const std::string& a_name, const std::string& a_renameFrom) const;

	/** @brief Draws one per-category strengths tree node in the settings UI. */
	void DrawCategoryStrengths(const char* a_id, const char* a_label, CategoryStrengths& a_strengths, const char* a_tooltip = nullptr);

	/** @brief Every per-category block of @ref settings in MaterialCategory order. */
	std::array<CategoryStrengths*, kMaterialCategoryCount> CategorySettings();
	std::array<const CategoryStrengths*, kMaterialCategoryCount> CategorySettings() const;

	/**
	 * @brief Whether any category has different effective Broad and Detail strengths (global times
	 * category).
	 */
	bool BandsSeparated() const;

	/** @brief The stored proxy curve, or Display-matched when it is out of range. */
	ProxyCurve ResolveProxyCurve() const;

	/**
	 * @brief Whether kMAIN holds linear light this frame.
	 *
	 * Linear Lighting's setting, except on the flat world map, where Linear Lighting stands
	 * down (the same test HDR Display's BuildHDRData applies).
	 */
	static bool IsLinearLightingActive();

	/**
	 * @brief Pre-tonemap color domain: linear with Linear Lighting, gamma otherwise. Legacy retains scene-
	 * linear handling for compatibility.
	 */
	ColorDomain SceneColorDomain(ProxyCurve a_curve) const;

	/** kMAIN-format output of the Before/After/Separate placements. */
	Texture2D* outputTexture = nullptr;
	/**
	 * Finished Image output matching the tonemap target size and format for CopyResource.
	 */
	Texture2D* finishedImageTexture = nullptr;
	/** Last target format rejected for Finished Image, so the warning logs once rather than per frame. */
	DXGI_FORMAT finishedImageRejectedFormat = DXGI_FORMAT_UNKNOWN;
	/**
	 * Render-resolution depth captured before UpscaleDepth(), matching motion and category guides.
	 */
	Texture2D* finishedImageDepthSnapshot = nullptr;
	/** Render-resolution extent of the captured guides (dynamic resolution is locked off afterwards). */
	uint32_t finishedImageGuideWidth = 0;
	uint32_t finishedImageGuideHeight = 0;
	/** Set by the capture, consumed by the next Finished Image evaluation - one evaluation per upscaled frame. */
	bool finishedImageGuidesReady = false;

	/**
	 * Held color, depth, categories, guide extent, and jitter for repeated evaluation.
	 */
	Texture2D* heldColor = nullptr;
	Texture2D* heldDepth = nullptr;
	Texture2D* heldCategories = nullptr;
	uint32_t heldGuideWidth = 0;
	uint32_t heldGuideHeight = 0;
	float heldGuideJitterX = 0.0f;
	float heldGuideJitterY = 0.0f;
	/// Placement the held frame was captured for; a hold never survives a placement change.
	uint32_t heldPlacement = UINT_MAX;

	/** Last vanilla tonemap pass inputs captured by CaptureDisplayTransform(). */
	struct DisplayCapture
	{
		bool valid = false;
		winrt::com_ptr<ID3D11ShaderResourceView> adaptationSRV;  ///< ISHDR AvgTex (x adapted, y target luminance).
		float param[4]{};                                        ///< ISHDR Param.
		float cinematic[4]{};                                    ///< ISHDR Cinematic.
		float tint[4]{};                                         ///< ISHDR Tint.
	} displayCapture;
	/** The first successful capture is logged once so its values can be checked against the game's imagespace. */
	bool displayCaptureLogged = false;

	/**
	 * Opaque categories captured before decals, refreshed after forward lighting. Evaluation never reads
	 * live Masks2.
	 */
	Texture2D* materialCategoriesSnapshot = nullptr;
	/** True between RestoreCategories() and FinishCategoryCapture(). */
	bool forwardCaptureActive = false;
	/** Resolved once in DataLoaded(); identifies humanoid races for the Equipment category. */
	RE::BGSKeyword* actorTypeNPCKeyword = nullptr;

	bool resourcesActive = false;
	uint activePlacement = UINT_MAX;
	/** Whether DLSS was the upscale method last frame; leaving it releases every resource. */
	bool dlssWasActive = false;

	/** Set by RequestHistoryReset(); consumed by BeginFrame() into resetThisFrame. */
	std::atomic<bool> pendingReset{ false };
	bool resetThisFrame = false;

	/**
	 * Comparison request consumed by the render-thread four-frame capture sequence.
	 */
	std::atomic<bool> comparePending{ false };
	// Render-thread only. 0 = idle; 1-4 = comparison capture frame (see ServiceComparison).
	int compareStep = 0;
	bool compareUserSetting = false;
	std::string compareStamp;

	/**
	 * Bracket Upscaling: finish categories and reset history before the pass; capture comparisons after
	 * compositing.
	 */
	struct Main_PostProcessing
	{
		static void thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * Binds Masks2 to SV_Target7 around each forward (post-deferred) lighting draw so it can
	 * write its Neural Rendering category; see RestoreCategories().
	 */
	struct BSBatchRenderer_RenderPassImmediately
	{
		static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		static bool Register();
	};
};
