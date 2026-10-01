#pragma once

#include <array>
#include <cstdint>
#include <memory>

struct ID3D11Resource;
struct ID3D11ShaderResourceView;

/**
 * @brief D3D11-facing Neural Rendering backend. Owns shared textures, transfer passes, and the failure
 * latch; keeps NGX/D3D12 transport details private.
 */
class NeuralRenderingBackend final
{
public:
	/**
	 * @brief Display transform for pre-tonemap proxies. Defaults to no exposure or grading; ignored for
	 * display gamma.
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

	/**
	 * @brief Frame inputs with active color and guide extents. Shared allocations exclude padded source
	 * margins.
	 */
	struct FrameInputs
	{
		ID3D11Resource* colorIn = nullptr;                          ///< Scene colour to enhance.
		ID3D11Resource* colorOut = nullptr;                         ///< Distinct destination for the result.
		ID3D11Resource* depth = nullptr;                            ///< Game depth buffer.
		ID3D11ShaderResourceView* depthSRV = nullptr;               ///< SRV over @c depth, used by the guide pass.
		ID3D11ShaderResourceView* materialCategoriesSRV = nullptr;  ///< Packed Masks2 material categories.
		ID3D11Resource* motionVectors = nullptr;                    ///< Motion vectors matching @c depth.
		/// SRV over @c motionVectors; the decode reprojects a previous frame's answer through it.
		/// Optional: without it alternating-frame mode evaluates every frame.
		ID3D11ShaderResourceView* motionVectorsSRV = nullptr;
		ID3D11Resource* superResolutionMotionVectors = nullptr;  ///< Processed motion field used by main DLSS.
		std::uint32_t width = 0;                                 ///< Colour/output active region width in pixels.
		std::uint32_t height = 0;                                ///< Colour/output active region height in pixels.
		std::uint32_t guideWidth = 0;                            ///< Depth/motion-vector active region width (render resolution).
		std::uint32_t guideHeight = 0;                           ///< Depth/motion-vector active region height (render resolution).
		/// Color projection jitter in render pixels (raster = unjittered position + offset). Zero for resolved
		/// color.
		float jitterOffsetX = 0.0f;
		float jitterOffsetY = 0.0f;
		/// Guide projection jitter relative to color, in guide texels. Zero when both share the same jitter.
		float guideJitterOffsetX = 0.0f;
		float guideJitterOffsetY = 0.0f;
		/// Model raster relative to the colour active region, per axis (0.25..1).
		/// Below one the model runs on a downsampled proxy and only its bounded
		/// edit returns to the full-resolution frame.
		float resolutionScaleX = 1.0f;
		float resolutionScaleY = 1.0f;
		/// How @c colorIn is encoded: 0 = linear open-ended HDR scene colour, 1 = finished
		/// gamma-2.2 display-referred frame (NeuralRendering::ColorDomain).
		std::uint32_t colorDomain = 0;
		/// How the scene-linear placements build the proxy (NeuralRendering::ProxyCurve);
		/// ignored in the display-gamma domain.
		std::uint32_t proxyCurve = 0;
		/// Display transform the scene-linear proxy replicates (identity by default).
		DisplayTransform display{};
		/// Display-gamma HDR peak relative to paper white; zero for SDR.
		float highlightWhite = 0.0f;
		/// Split-screen comparison: the decode passes the input through left of this fraction
		/// of the width; negative disables it. See NeuralRendering::CompareView.
		float wipePosition = -1.0f;
		/// The colour and guides are one held frame re-evaluated every frame (Frame Hold): the
		/// model gets zero motion and alternating-frame skips are off.
		bool staticMotion = false;
		float intensity = 0.8f;
		float colorStrength = 1.0f;
		/// Overall weight of the model's edit (0..2); one applies it exactly.
		float transferStrength = 1.0f;
		/// The two halves of the model's luminance change; see
		/// NeuralRendering::Options::broadLuminosity. The band passes are skipped unless
		/// some category's global x category products differ.
		float broadLuminosity = 1.0f;
		float detailLuminosity = 1.0f;
		/// Radius of the edge-aware blur that separates them, in model texels.
		float bandRadius = 8.0f;
		/// Two-sided guard (1/maxRatio..maxRatio) on the model/proxy luminance ratio;
		/// see NeuralRendering::Options::maxRatio.
		float maxRatio = 2.0f;
		/// Whether maxRatio is applied at all; see NeuralRendering::Options::ratioGuardEnabled.
		bool ratioGuardEnabled = false;
		std::array<float, 7> categoryColorStrengths{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<float, 7> categoryTransferStrengths{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		/// Per-category multipliers on Broad and Detail Luminosity (see
		/// NeuralRendering::CategoryStrengths::broadLuminosity).
		std::array<float, 7> categoryBroadLuminosity{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<float, 7> categoryDetailLuminosity{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		/// Per-category hue guard (see ColorTransfer.hlsli, ResolveNeuralColor). Indexed by
		/// NeuralRendering::MaterialCategory; only Hair (index 2) defaults on.
		std::array<bool, 7> categoryHueGuard{ false, false, true, false, false, false, false };
		/// Experimental, off by default: fade the edit across depth silhouettes when
		/// the model runs below the colour resolution; no effect at native scale.
		bool depthAwareResolve = false;
		/// Run the model every other frame and re-apply its previous answer to the
		/// fresh frame in between (experimental; halves the neural cost).
		bool alternateFrames = false;
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float skinStructureStrength = -1.0f;
		std::uint32_t style = 3;
		std::uint32_t outputWidth = 0;   ///< Separate-upscaling output width; zero for ordinary Evaluate().
		std::uint32_t outputHeight = 0;  ///< Separate-upscaling output height.
		std::uint32_t superResolutionQualityMode = 1;
		std::uint32_t superResolutionPreset = 0;
		bool automaticMask = true;
		/// Debug view: the decode renders each pixel's classified material category as a flat
		/// colour instead of blending the model's edit; see NeuralRendering::Options::debugCategoryView.
		bool debugCategoryView = false;
		/// Diagnostic: write Feature 18's answer directly, bypassing the resolve
		/// entirely; see NeuralRendering::Options::rawModelOutput. Only honoured
		/// in the display-gamma colour domain (Finished Image).
		bool rawModelOutput = false;
		/// Debug views and readbacks; see the matching NeuralRendering::Options fields.
		bool debugGuardClamp = false;
		bool debugBroadBand = false;
		bool debugDetailBand = false;
		bool measureModelPeak = false;
		bool reset = false;          ///< Force a history reset on this frame.
		bool depthInverted = false;  ///< Depth guide is Reverse Z (near = 1, far = 0).
	};

	NeuralRenderingBackend();
	~NeuralRenderingBackend();

	NeuralRenderingBackend(const NeuralRenderingBackend&) = delete;
	NeuralRenderingBackend& operator=(const NeuralRenderingBackend&) = delete;

	/**
	 * @brief Probes for a user-supplied nvngx_dlssnr.dll once and caches the result.
	 * @return True when the Neural Rendering runtime was found and is a supported version.
	 */
	bool IsAvailable();

	/**
	 * @brief Reports whether Feature 18 has produced at least one successful frame.
	 * @return True after a successful Execute; false while latched, uninitialised, or torn down.
	 */
	bool IsFeatureAvailable() const;

	/**
	 * @brief Delayed resolve diagnostics, updated only while measurement is enabled.
	 */
	struct DebugReadback
	{
		float modelPeakLuminance = 0.0f;
		float guardClampedPercent = 0.0f;
		bool valid = false;
	};
	DebugReadback GetDebugReadback() const;

	/**
	 * @brief Runs the full Neural Rendering pass for one frame.
	 * @param inputs Resources, active region, and tuning for this frame.
	 * @return True when the result was written to @c inputs.colorOut.
	 */
	bool Evaluate(const FrameInputs& inputs);

	/** @brief Prepare the private, temporally upscaled signed NR residual for this frame. */
	bool PrepareSeparateUpscaling(const FrameInputs& inputs);

	/** @brief Apply the prepared residual to the game's clean display-resolution DLSS output. */
	bool ResolveSeparateUpscaling(ID3D11Resource* cleanColor, ID3D11Resource* colorOut,
		std::uint32_t width, std::uint32_t height);

	/**
	 * @brief Release shared resources and private histories; retain the runtime/device and clear the
	 * failure latch.
	 */
	void DestroyResources();

private:
	struct State;
	std::unique_ptr<State> state;
};
