#pragma once

#include "Buffer.h"
#include "Feature.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <d3d11.h>
#include <winrt/base.h>

class NeuralRenderingBackend;

/**
 * @brief DLSS Neural Rendering (NGX Feature 18).
 *
 * Personal-original feature. Feature 18 only ever exposes a D3D12 ABI, so this does not talk to
 * the Streamline-owned NGX core: it drives a directly loaded nvngx_dlssnr.dll through the
 * transport layer in `NeuralRendering/`, bridging Skyrim's D3D11 resources into D3D12 with shared
 * textures. The runtime DLL is proprietary and must be supplied by the user; it is never shipped.
 *
 * Neural Rendering consumes renderer and Upscaling resources at several pipeline stages but is
 * not part of the Upscaling feature. Its only entry points inside Upscaling are the seam calls
 * PrepareUpscaleInput() and ResolveUpscaledFrame(); everything else (the Main_PostProcessing
 * frame bracket, material-category capture, the tonemap-stage Finished Image pass, loading-screen
 * history resets) is hooked or called from outside Upscaling. See docs/development/neural-rendering.md.
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
		return { T("feature.neural_rendering.description", "DLSS 5 Neural Rendering (NGX Feature 18) with a user-supplied nvngx_dlssnr.dll"),
			{ T("feature.neural_rendering.key_feature_1", "Four pipeline placements, from before upscaling to the finished image"),
				T("feature.neural_rendering.key_feature_2", "Per-material-category strengths and hue guards"),
				T("feature.neural_rendering.key_feature_3", "Reduced-resolution model evaluation"),
				T("feature.neural_rendering.key_feature_4", "Matched on/off comparison screenshots") } };
	}

	/** @brief Per-material multipliers and toggles applied by the local colour resolve. */
	struct CategoryStrengths
	{
		float colorStrength = 1.0f;
		float transferStrength = 1.0f;
		/// Independent multiplier on the model's light/dark (luminance) change for this
		/// category; lower values keep the category's own colour and detail transfer
		/// while damping contrast swings. See Options::luminosityStrength.
		float luminosityStrength = 1.0f;
		/// Restrict this category's chroma change to a saturation change on
		/// renderer-neutral pixels (see ColorTransfer.hlsli, ResolveNeuralColor).
		/// Off by default; Hair defaults this on (see Settings).
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
	 * @brief Display transform the pre-tonemap placements' proxy replicates, so the model
	 *        sees the frame the way the user will (see ColorTransfer.hlsli, NeuralDisplayTransform).
	 *
	 * Default-constructed it is the identity: no exposure and the plain hue-preserving Reinhard
	 * proxy. Ignored for the display-gamma colour domain, which is already a finished frame.
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
		float postProcessExposureScale = 0.18f;               ///< 0.18 * exp2(exposure compensation).
		float postProcessAdaptationRange[2]{ 0.0f, 1.0f };  ///< Linear clamp range of the adapted luminance.
	};

	/** @brief Per-evaluation settings handed to the backend. */
	struct Options
	{
		uint32_t style = 3;
		float intensity = 0.8f;
		float colorStrength = 1.0f;
		/// Overall weight of the model's edit on the frame (0..2). Zero leaves the
		/// frame untouched, one applies the model's change exactly, two doubles its
		/// relative luminance change (still inside the resolve's ratio guard).
		float transferStrength = 1.0f;
		/// Additional multiplier on top of Transfer Strength applied only to the
		/// model's light/dark (luminance) change; the chroma transfer gated by
		/// Color Strength is unaffected. Lower values keep the model's colour and
		/// detail edit while damping the contrast swings that drive it. Zero
		/// leaves luminance untouched (still subject to Transfer Strength being
		/// non-zero); one matches the pre-luminosity-strength behaviour exactly.
		float luminosityStrength = 1.0f;
		/// Two-sided guard (1/maxRatio..maxRatio) on the model/proxy luminance
		/// ratio the resolve applies (see ColorTransfer.hlsli, ResolveNeuralColor),
		/// only in effect while @ref ratioGuardEnabled is true. One disables any
		/// luminance change; the previous hardcoded, always-on behaviour was
		/// exactly 2.
		float maxRatio = 2.0f;
		/// Whether @ref maxRatio is applied at all. Off by default: the model's
		/// light/dark change reaches the frame exactly as computed, however far
		/// it swings - including a correct edit that puts a lit surface fully
		/// into shadow, which no small guard value can pass. Turning this on
		/// trades some of that range for protection against a single unstable
		/// model frame flashing or flickering.
		bool ratioGuardEnabled = false;
		/// Per-material multipliers and hue-guard toggles; always in effect. The
		/// global strengths above are still applied afterwards as the final
		/// adjustment layer, and each category's own hue guard toggle replaces a
		/// single master switch (see CategoryStrengths::hueGuard).
		CategoryStrengthArray categoryStrengths{};
		/// When the model runs below the colour resolution, fade its edit across
		/// depth silhouettes so a bilinearly upsampled background edit does not
		/// bleed into thin foreground geometry. No effect at native scale.
		bool depthAwareResolve = true;
		/// Experimental: evaluate the model every other frame and re-apply its
		/// previous answer to the fresh frame in between, fading it where the
		/// content under a pixel changed. Halves the neural cost; the model's
		/// own temporal state then sees every second frame.
		bool alternateFrames = false;
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float skinStructureStrength = -1.0f;
		bool automaticMask = true;
		/// Debug view: DecodeColorCS renders each pixel's classified material category
		/// (NeuralRenderingCategories::DebugColor) instead of blending the model's edit.
		/// The model still evaluates normally; only the final decode is replaced, so this
		/// carries the full Neural Rendering cost - it is for tuning category boundaries,
		/// not a lightweight preview.
		bool debugCategoryView = false;
		/// Diagnostic: write Feature 18's answer directly to the output, preserving
		/// the renderer's alpha, instead of blending it through the resolve. Only
		/// honoured in the display-gamma colour domain (Finished Image) - in scene
		/// linear this would dump a display-referred model answer into a linear HDR
		/// buffer the game's own tonemapper still has to process, not a meaningful
		/// image. Not meant to ship on; it exists to tell apart a weak model answer
		/// from an over-conservative resolve.
		bool rawModelOutput = false;
		bool reset = false;

		/// Valid region of @p depth and @p motionVectors, i.e. the game's render
		/// (dynamic) resolution. Zero means "same as width/height" - correct only
		/// when Neural Rendering runs before the upscaler, where the colour input is
		/// also at render resolution. After the upscaler the colour input is at
		/// display resolution while the guides are still at render resolution, so
		/// this must be set.
		uint32_t guideWidth = 0;
		uint32_t guideHeight = 0;

		/// Sub-pixel TAA jitter of @p colorIn in render pixels, using the same
		/// convention Streamline receives (a scene point at unjittered position u
		/// lands in the raster at u + offset). Feature 18 has no jitter parameter,
		/// so the backend resamples the frame onto the unjittered grid before the
		/// model sees it and maps the edit back afterwards. Zero when the colour
		/// input is the already-unjittered upscaled frame.
		float jitterOffsetX = 0.0f;
		float jitterOffsetY = 0.0f;

		/// Sub-pixel TAA jitter of the depth/motion/category guides in guide
		/// pixels, same convention. It is what the decode has to undo to line a
		/// guide lookup up with the colour pixel asking for it, so it is the
		/// guides' jitter *relative to the colour raster*, not their absolute
		/// jitter: zero whenever the colour input is the game's jittered render,
		/// which is jittered alike. Set it only after the upscaler, where the
		/// colour is the resolved unjittered frame and the guides are not.
		float guideJitterOffsetX = 0.0f;
		float guideJitterOffsetY = 0.0f;

		/// Resolution the model runs at relative to the colour region it
		/// processes, per axis (0.25..2). Below one the model sees a downsampled
		/// proxy and only its bounded luminance/colour edit is applied to the
		/// full-resolution frame, which keeps native detail while cutting the neural
		/// cost roughly with the pixel count. Above one supersamples the model input.
		/// The backend debounces changes so a slider drag does not rebuild the
		/// feature every frame.
		float resolutionScaleX = 1.0f;
		float resolutionScaleY = 1.0f;

		/// How the colour input is encoded. Scene linear compresses it with a Reinhard
		/// proxy before the model sees it; display gamma hands an already-tonemapped
		/// frame through unchanged (only HDR over-range pixels are scaled down) and
		/// applies the edit in linear light decoded with the same 2.2 curve.
		ColorDomain colorDomain = ColorDomain::kSceneLinear;

		/// Display transform the scene-linear proxy replicates so the model sees the frame
		/// the way the user will. Set by the pre-tonemap placements (built from the captured
		/// ISHDR pass and Post Processing's auto exposure); left at the identity by Finished
		/// Image, which already runs on the finished frame.
		DisplayTransform display{};

		/// DLSS-SR quality/preset selections mirrored from Upscaling settings when
		/// Separate Upscaling is active. They are ignored by the other placements.
		uint32_t superResolutionQualityMode = 1;
		uint32_t superResolutionPreset = 0;
	};

	struct Settings
	{
		bool enabled = false;
		uint placement = static_cast<uint>(Placement::kFinishedImage);
		uint style = 0;  // 0=Default, 1=Natural, 2=Cinematic
		float intensity = 1.0f;
		float colorStrength = 1.0f;
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float skinStructureStrength = -1.0f;
		bool automaticMask = true;
		uint resolutionMode = 0;  // 0=Uniform scale, 1=Per-axis (experimental anamorphic) scale
		float resolutionScale = 1.0f;
		float resolutionScaleX = 1.0f;
		float resolutionScaleY = 1.0f;
		float transferStrength = 1.0f;
		float luminosityStrength = 1.0f;
		float maxRatio = 2.0f;  // Two-sided guard on the model/proxy luminance ratio (1/x..x).
		// Off by default: the guard above is not applied at all, so a correct large
		// light/dark swing (e.g. a lit surface the model puts fully into shadow)
		// is never capped. On, Max Ratio governs the swing as before.
		bool ratioGuardEnabled = false;
		CategoryStrengths everythingElseStrengths;
		CategoryStrengths skinStrengths;
		// Hair is the only category that hue-guards its chroma change by default.
		CategoryStrengths hairStrengths{ 1.0f, 1.0f, 1.0f, true };
		CategoryStrengths eyesStrengths;
		CategoryStrengths foliageStrengths;
		CategoryStrengths landscapeStrengths;
		CategoryStrengths equipmentStrengths;
		bool depthAwareResolve = true;
		bool alternateFrames = false;
		/// Debug view: render each pixel's classified material category as a flat colour instead
		/// of the model's edit. See Options::debugCategoryView.
		bool debugCategoryView = false;
		bool rawModelOutput = false;  // Diagnostic: skip the resolve, write Feature 18's answer directly (Finished Image only).
	};

	Settings settings;

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

	/**
	 * @brief Moves Neural Rendering settings saved by older builds out of the Upscaling section.
	 *
	 * Before Neural Rendering was its own feature, its settings were stored as
	 * `neuralRendering*` keys inside the "Upscaling" object of the settings JSON. When @p a_root
	 * has no "Neural Rendering" object yet, this builds one from those keys (prefix dropped,
	 * first letter lower-cased). Called by State::LoadFromJson before any feature loads.
	 * Stale keys are left in the Upscaling object: Upscaling's loader ignores unknown keys and
	 * its next save drops them.
	 * @param a_root The whole settings JSON.
	 */
	static void MigrateLegacyUpscalingSettings(json& a_root);

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
	 * @brief Executes Neural Rendering on the current D3D11 immediate context.
	 *
	 * The shared textures backing the D3D12 bridge are compact allocations at the
	 * active colour and guide extents. This keeps Feature 18's creation raster and
	 * per-frame subrect contract identical.
	 *
	 * @param colorIn Input color resource; must be shader-readable.
	 * @param colorOut Distinct output resource receiving the neural-rendered image; must be UAV-writable.
	 * @param depth Depth resource.
	 * @param depthSRV Shader resource view over @p depth, used by the depth-guide compute pass.
	 * @param materialCategoriesSRV Packed material category and vertex-AO render target.
	 * @param motionVectors Motion-vector resource.
	 * @param width Active region width in pixels.
	 * @param height Active region height in pixels.
	 * @param options Neural Rendering settings.
	 * @return True when NGX successfully evaluates the feature and the result reaches @p colorOut.
	 */
	bool Evaluate(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
		ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
		ID3D11ShaderResourceView* materialCategoriesSRV,
		ID3D11Resource* motionVectors,
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
		ID3D11Resource* motionVectors, ID3D11Resource* superResolutionMotionVectors,
		uint32_t width, uint32_t height,
		uint32_t outputWidth, uint32_t outputHeight, const Options& options);

	/**
	 * @brief Applies the prepared full-resolution residual to a clean main-DLSS result.
	 * @return True only when a matching private DLSS evaluation completed this frame.
	 */
	bool ResolveSeparateUpscaling(ID3D11Resource* cleanColor, ID3D11Resource* colorOut,
		uint32_t width, uint32_t height);

	/**
	 * @brief Retire private NGX histories and shared textures without shutting down
	 *        the process-wide NGX runtime.
	 *
	 * Placement and upscaler changes call this from the render thread. The private
	 * D3D12 interop/runtime instance is intentionally retained for safe reuse. This
	 * also clears the failure latch so toggling Neural Rendering can retry a failure.
	 */
	void DestroyModelResources();

	// ---- Upscaling seam (the only calls made from inside Upscaling) ----

	/**
	 * @brief Seam S1: Before / Separate Upscaling, from Upscaling::Upscale()'s DLSS branch.
	 *
	 * Called right before the Streamline evaluate, after EncodeTextures has written the dilated
	 * motion-vector copy. Before Upscaling evaluates the model on the render-resolution scene and
	 * returns its result for DLSS to reconstruct; Separate Upscaling prepares its private
	 * residual and returns @p a_color unchanged. Returns @p a_color whenever Neural Rendering
	 * does not run here (fails closed).
	 * @param a_color kMAIN, the colour DLSS would otherwise upscale.
	 * @param a_superResolutionMotionVectors Upscaling's dilated motion-vector copy (Separate
	 *        Upscaling hands it to its private DLSS-SR alongside the raw game vectors).
	 * @return The resource DLSS should upscale this frame.
	 */
	ID3D11Resource* PrepareUpscaleInput(ID3D11Resource* a_color, ID3D11Resource* a_superResolutionMotionVectors);

	/**
	 * @brief Seams S2 + S3: After / Separate Upscaling, and the Finished Image guide snapshot.
	 *
	 * Called from Upscaling::PerformUpscaling() between Upscale() and UpscaleDepth(). After
	 * Upscaling evaluates the model on the upscaled frame; Separate Upscaling composites its
	 * residual onto it. Either way the result is written back into @p a_upscaled, so
	 * Upscaling's sharpening/resolve consumes it unchanged. Finished Image snapshots its
	 * render-resolution depth here, because UpscaleDepth() expands kMAIN depth right after.
	 * @param a_upscaled Upscaling's display-resolution DLSS output (its sharpener input);
	 *        may be null when DLSS is not the upscale method.
	 */
	void ResolveUpscaledFrame(Texture2D* a_upscaled);

	// ---- Tonemap stage (Hooks.cpp) ----

	/**
	 * @brief Captures what the vanilla tonemap pass just applied, for the pre-tonemap placements' proxy.
	 *
	 * Called from the HDRTonemapBlendCinematic hook right after the vanilla pass ran. Reads
	 * ISHDR's Param / Cinematic / Tint constants from the pass's pixel constant group and the
	 * adaptation texture (AvgTex) the pass sampled at pixel-shader slot 2. Only valid while the
	 * vanilla tonemap owns the frame; pass null (or another owner) to invalidate the capture.
	 * Consumed one frame later by MakeDisplayTransform().
	 * @param a_param The pass's shader parameters, or null when the vanilla pass did not run.
	 */
	void CaptureDisplayTransform(RE::ImageSpaceShaderParam* a_param);

	/**
	 * @brief Applies Finished Image Neural Rendering in place to a game render target.
	 *
	 * Called from PostProcessingExtensions::Main_HDRTonemapBlendCinematic_Render (Hooks.cpp), at
	 * every point that hands the frame's tonemapped colour onward - right after
	 * State::HandlePostProcessing() when Effects11 owns the tonemap, and right after the vanilla
	 * tonemap/passthrough call otherwise (which covers Post Processing owning the tonemap too:
	 * by then the vanilla call has already taken its passthrough branch over Post Processing's
	 * result). Unlike the Before/After/Separate Upscaling placements, Finished Image therefore
	 * does not depend on any one feature owning the tonemap.
	 *
	 * @p a_target is the tonemap pass's output - kFRAMEBUFFER (UNORM in SDR), or HDR Display's
	 * float16 texture while it redirects that slot - never kMAIN, so the edit is written to
	 * finishedImageTexture, sized and formatted to match @p a_target, rather than the
	 * kMAIN-format outputTexture the other placements use.
	 *
	 * Delegates to EvaluateFinishedImage() and, on success, copies the edited result back into
	 * @p a_target so every caller downstream (HUD, menu, Present) sees it without needing to
	 * know Neural Rendering ran. A no-op whenever that call fails closed, or while a main
	 * menu/loading screen is open.
	 *
	 * @param a_target Game render target holding this frame's finished colour.
	 */
	void ApplyFinishedImage(RE::RENDER_TARGET a_target);

	// ---- Material categories (Deferred.cpp) ----

	/**
	 * @brief Snapshots Masks2's packed material categories before blended decals can touch it.
	 *
	 * Masks2 is deliberately blendable (vertex AO fades under translucent decals), but the
	 * material category packed into its low bits (NeuralRenderingCategories::Pack) is a discrete
	 * value - alpha-blending it produces a meaningless bit pattern, not "the nearer category".
	 * Called from Deferred's blended-decals hook, after opaque geometry but before decals draw.
	 * First of three steps; see RestoreCategories() and FinishCategoryCapture() for the
	 * forward-stage continuation.
	 */
	void CaptureCategories();

	/**
	 * @brief Hands the pre-decal category snapshot back to Masks2 once the deferred composite
	 * has consumed the decal-blended vertex AO, and arms the forward category capture.
	 *
	 * Alpha-blended lighting geometry (hair strands, hairline scalps, translucent clothing) is
	 * sorted and drawn forward, after Deferred::EndDeferred, where Masks2 is normally unbound.
	 * Those draws are what a hair category most often lives in, so BSBatchRenderer_RenderPassImmediately
	 * binds Masks2 back for exactly those draws and they write their category on top of the
	 * restored opaque snapshot. Restoring rather than writing over the live target keeps the
	 * decals' AO contribution to the composite untouched. Called from Deferred::EndDeferred
	 * after DeferredPasses().
	 */
	void RestoreCategories();

	/**
	 * @brief Re-snapshots Masks2 after the forward lighting draws and disarms the forward capture.
	 *
	 * Called at the start of the Main_PostProcessing frame bracket, after world and first-person
	 * geometry and before any Neural Rendering evaluation reads the snapshot.
	 */
	void FinishCategoryCapture();

	/**
	 * @brief Flags the render pass geometry's actor ownership for Neural Rendering categories.
	 *
	 * Hooked onto BSLightingShader::SetupGeometry, for every lighting draw that writes Masks2
	 * (the deferred pass and the forward draws between RestoreCategories and
	 * FinishCategoryCapture). Sets State::ExtraShaderDescriptors::IsHumanoidActor when the
	 * geometry's owning reference is an actor whose race carries the ActorTypeNPC keyword;
	 * Lighting.hlsl maps that flag to NeuralRenderingCategories::Equipment for everything the
	 * skin, hair and eye permutations did not already claim. Sets
	 * State::ExtraShaderDescriptors::IsHair when the geometry is part of one of the actor's hair
	 * or facial-hair head parts (the hairlines, braids and strands authored with a shader type
	 * other than hair tint), or when its material is hair tint or carries the hair soft-lighting
	 * flag (wigs worn as equipment); Lighting.hlsl maps that to NeuralRenderingCategories::Hair
	 * ahead of the technique-derived category.
	 * @param a_pass The render pass being set up.
	 */
	void SetupGeometryCategory(RE::BSRenderPass* a_pass);

	// ---- Controls (Menu hotkeys and settings UI) ----

	/**
	 * @brief Resets the Neural Rendering temporal history, and DLSS's with it, on the next frame.
	 *
	 * Used by loading transitions, the toggle hotkey and the comparison capture, so both
	 * histories restart on the same frame.
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
	 * @brief Builds the display transform the scene-linear proxy should replicate this frame.
	 *
	 * Combines the last vanilla capture (exposure and grading, when the vanilla tonemap owns the
	 * frame) with Post Processing's Histogram Auto Exposure (when active). Identity when neither
	 * applies, e.g. under Effects11, which keeps the previous plain-Reinhard proxy.
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
	 * @brief Snapshots Finished Image's render-resolution guides for this frame.
	 *
	 * Called from ResolveUpscaledFrame(), before UpscaleDepth(): copies kMAIN depth while it is
	 * still on the same render-resolution, jittered raster as kMOTION_VECTOR and the
	 * material-category snapshot, and records that raster's extent. A no-op unless Finished
	 * Image is active on DLSS.
	 */
	void CaptureFinishedImageGuides();

	/**
	 * @brief Evaluates Finished Image Neural Rendering on another feature's composited output.
	 *
	 * Called (via ApplyFinishedImage()) after the frame's tonemap has run - Effects11's, Post
	 * Processing's own, or vanilla ISHDR's, whichever owned it that frame - so the colour this
	 * sees is a genuinely finished, display-referred frame rather than the linear HDR scene
	 * colour the Before/After Upscaling placements have to approximate with a Reinhard proxy.
	 * The guides are not on @p a_colorIn's display-resolution, unjittered grid: motion vectors
	 * and the category snapshot stay at render resolution with the frame's TAA jitter, so depth
	 * is the matching snapshot CaptureFinishedImageGuides() took before UpscaleDepth() expanded
	 * kMAIN depth, and the guide extent and guide jitter are set exactly as the After Upscaling
	 * placement sets them. The guides are consumed on use, so the model runs at most once per
	 * upscaled frame. The active resolution is read from the same authoritative
	 * globals::game::graphicsState->screenWidth/Height every other Neural Rendering call site
	 * uses - not @p a_colorIn's own GetDesc(), which can legitimately be a larger,
	 * differently-padded allocation than the frame's active region.
	 *
	 * Fails closed: returns false and leaves @p a_colorIn untouched whenever Finished Image is
	 * not the active placement, the backend is unavailable, or a required resource/guide is
	 * missing.
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

	/** @brief Draws one per-category strengths tree node in the settings UI. */
	void DrawCategoryStrengths(const char* a_id, const char* a_label, CategoryStrengths& a_strengths, const char* a_tooltip = nullptr);

	/** kMAIN-format output of the Before/After/Separate placements. */
	Texture2D* outputTexture = nullptr;
	/**
	 * Finished Image output. Allocated lazily to match the tonemap pass's output target
	 * (kFRAMEBUFFER, or HDR Display's float16 redirect of it) exactly - not kMAIN, whose format
	 * differs, which would make the copy back a silent D3D11 no-op.
	 */
	Texture2D* finishedImageTexture = nullptr;
	/** Last target format rejected for Finished Image, so the warning logs once rather than per frame. */
	DXGI_FORMAT finishedImageRejectedFormat = DXGI_FORMAT_UNKNOWN;
	/**
	 * kMAIN depth copied by CaptureFinishedImageGuides() before UpscaleDepth() expands it to
	 * display resolution. Motion vectors and materialCategoriesSnapshot are never expanded, so
	 * Finished Image needs depth on that same render-resolution, jittered raster.
	 */
	Texture2D* finishedImageDepthSnapshot = nullptr;
	/** Render-resolution extent of the captured guides (dynamic resolution is locked off afterwards). */
	uint32_t finishedImageGuideWidth = 0;
	uint32_t finishedImageGuideHeight = 0;
	/** Set by the capture, consumed by the next Finished Image evaluation - one evaluation per upscaled frame. */
	bool finishedImageGuidesReady = false;

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
	 * Masks2's packed material categories: copied right after opaque geometry,
	 * before blended decals can alpha-blend into it and corrupt the category
	 * bits (CaptureCategories), then refreshed once the forward lighting draws
	 * have added theirs (FinishCategoryCapture). This is what every Neural
	 * Rendering evaluation call reads instead of the live Masks2.
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
	 * Set by the comparison hotkey / menu button. Serviced by the Main_PostProcessing hook,
	 * which drives compareStep over four frames (Neural Rendering forced off, then on, each
	 * with a warm-up frame) and grabs the off/on frames at the end of post-processing - before
	 * any HUD or menu is drawn - into Data/DLSS 5 Screenshots/.
	 */
	std::atomic<bool> comparePending{ false };
	// Render-thread only. 0 = idle; 1-4 = comparison capture frame (see ServiceComparison).
	int compareStep = 0;
	bool compareUserSetting = false;
	std::string compareStamp;

	/**
	 * Brackets Upscaling's own Main_PostProcessing hook (chained on the same call site, so it
	 * runs outside it): category capture finish, history reset and comparison state before
	 * upscaling, the comparison screenshot after compositing.
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
