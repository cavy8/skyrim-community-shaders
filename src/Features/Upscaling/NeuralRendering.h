#pragma once

#include <array>
#include <cstdint>
#include <memory>

struct ID3D11Resource;
struct ID3D11ShaderResourceView;

/**
 * @brief DLSS Neural Rendering (NGX Feature 18) backend.
 *
 * Feature 18 only ever exposes a D3D12 ABI, so this class does not talk to the
 * Streamline-owned NGX core. It drives a directly loaded nvngx_dlssnr.dll
 * through the transport layer in `NeuralRendering/`, bridging Skyrim's D3D11
 * resources into D3D12 with shared textures. The runtime DLL is proprietary and
 * must be supplied by the user; Community Shaders never ships it.
 */
class NeuralRendering final
{
public:
	/** @brief Per-material multipliers applied by the local colour resolve. */
	struct CategoryStrengths
	{
		float colorStrength = 1.0f;
		float transferStrength = 1.0f;
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

	/** @brief Settings passed to the Neural Rendering feature. */
	struct Options
	{
		uint32_t style = 3;
		float intensity = 0.8f;
		float colorStrength = 1.0f;
		/// Overall weight of the model's edit on the frame (0..2). Zero leaves the
		/// frame untouched, one applies the model's change exactly, two doubles its
		/// relative luminance change (still inside the resolve's ratio guard).
		float transferStrength = 1.0f;
		/// Restricts the model's chroma change to a saturation change (never a hue rotation,
		/// never past neutral) on renderer-neutral pixels, so a consistent colour bias in the
		/// model's palette cannot tint neutral shading. Released smoothly as the original pixel
		/// carries more chroma of its own; disabling it lets the model's colour changes apply
		/// everywhere, including on neutral surfaces (see ColorTransfer.hlsli, ResolveNeuralColor).
		bool hueGuard = true;
		/// Enables per-material multipliers. The global strengths above are still
		/// applied afterwards as the final adjustment layer.
		bool perCategoryStrengths = false;
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
		/// the way the user will. Set by the pre-tonemap placements (Upscaling builds it
		/// from the captured ISHDR pass and Post Processing's auto exposure); left at the
		/// identity by Finished Image, which already runs on the finished frame.
		DisplayTransform display{};

		/// DLSS-SR quality/preset selections mirrored from Upscaling settings when
		/// Separate Upscaling is active. They are ignored by the other placements.
		uint32_t superResolutionQualityMode = 1;
		uint32_t superResolutionPreset = 0;
	};

	NeuralRendering();
	~NeuralRendering();

	NeuralRendering(const NeuralRendering&) = delete;
	NeuralRendering& operator=(const NeuralRendering&) = delete;

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
	void DestroyResources();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
